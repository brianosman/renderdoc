/******************************************************************************
 * The MIT License (MIT)
 * 
 * Copyright (c) 2015 Baldur Karlsson
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 ******************************************************************************/

#pragma once

#include "core/resource_manager.h"

#include "vk_resources.h"

#include <algorithm>
#include <utility>

using std::pair;

class WrappedVulkan;

class VulkanResourceManager : public ResourceManager<WrappedVkRes*, TypedRealHandle, VkResourceRecord>
{
	public: 
		VulkanResourceManager(LogState s, Serialiser *ser, WrappedVulkan *core)
			: ResourceManager(s, ser), m_Core(core)
		{	}
		~VulkanResourceManager() { }

		void ClearWithoutReleasing()
		{
			// if any objects leaked past, it's no longer safe to delete them as we would
			// be calling Shutdown() after the device that owns them is destroyed. Instead
			// we just have to leak ourselves.
			RDCASSERT(m_LiveResourceMap.empty());
			RDCASSERT(m_InframeResourceMap.empty());
			RDCASSERT(m_InitialContents.empty());
			RDCASSERT(m_ResourceRecords.empty());
			RDCASSERT(m_CurrentResourceMap.empty());
			RDCASSERT(m_WrapperMap.empty());
			
			m_LiveResourceMap.clear();
			m_InframeResourceMap.clear();
			m_InitialContents.clear();
			m_ResourceRecords.clear();
			m_CurrentResourceMap.clear();
			m_WrapperMap.clear();
		}
		
		template<typename realtype>
		void AddLiveResource(ResourceId id, realtype obj)
		{
			ResourceManager::AddLiveResource(id, GetWrapped(obj));
		}

		using ResourceManager::AddResourceRecord;
		
		template<typename realtype>
		VkResourceRecord *AddResourceRecord(realtype &obj)
		{
			typename UnwrapHelper<realtype>::Outer *wrapped = GetWrapped(obj);
			VkResourceRecord *ret = wrapped->record = ResourceManager::AddResourceRecord(wrapped->id);

			ret->Resource = (WrappedVkRes *)wrapped;

			return ret;
		}
		
		// easy path for getting the unwrapped handle cast to the
		// write type. Saves a lot of work casting to either WrappedVkNonDispRes
		// or WrappedVkDispRes depending on the type, then ->real, then casting
		// when this is all we want to do in most cases
		template<typename realtype>
		realtype GetLiveHandle(ResourceId origid)
		{
			return realtype( (uint64_t) ((typename UnwrapHelper<realtype>::ParentType *)ResourceManager::GetLiveResource(origid)) );
		}

		template<typename realtype>
		realtype GetCurrentHandle(ResourceId id)
		{
			return realtype( (uint64_t) ((typename UnwrapHelper<realtype>::ParentType *)ResourceManager::GetCurrentResource(id)) );
		}
		
		// handling memory & image transitions
		template<typename SrcTransType>
		void RecordSingleTransition(vector< pair<ResourceId, ImageRegionState> > &trans, ResourceId id, const SrcTransType &t, uint32_t nummips, uint32_t numslices);

		void RecordTransitions(vector< pair<ResourceId, ImageRegionState> > &trans, map<ResourceId, ImageLayouts> &states,
			                     uint32_t numTransitions, const VkImageMemoryBarrier *transitions);

		void MergeTransitions(vector< pair<ResourceId, ImageRegionState> > &dsttrans,
		                      vector< pair<ResourceId, ImageRegionState> > &srctrans);

		void ApplyTransitions(vector< pair<ResourceId, ImageRegionState> > &trans, map<ResourceId, ImageLayouts> &states);

		void SerialiseImageStates(map<ResourceId, ImageLayouts> &states, vector<VkImageMemoryBarrier> &transitions);

		ResourceId GetID(WrappedVkRes *res)
		{
			if(res == NULL) return ResourceId();

			if(IsDispatchableRes(res))
				return ((WrappedVkDispRes *)res)->id;

			return ((WrappedVkNonDispRes *)res)->id;
		}
		
		template<typename realtype>
		WrappedVkNonDispRes *GetNonDispWrapper(realtype real)
		{
			return (WrappedVkNonDispRes *)GetWrapper(ToTypedHandle(real));
		}

		template<typename parenttype, typename realtype>
		ResourceId WrapResource(parenttype parentObj, realtype &obj)
		{
			RDCASSERT(obj != VK_NULL_HANDLE);

			ResourceId id = ResourceIDGen::GetNewUniqueID();
			typename UnwrapHelper<realtype>::Outer *wrapped = new typename UnwrapHelper<realtype>::Outer(obj, id);
			
			SetTableIfDispatchable(m_State >= WRITING, parentObj, m_Core, wrapped);

			AddCurrentResource(id, wrapped);

			AddWrapper(wrapped, ToTypedHandle(obj));

			obj = realtype((uint64_t)wrapped);

			return id;
		}
		
		template<typename realtype>
		void ReleaseWrappedResource(realtype obj, bool clearID = false)
		{
			ResourceId id = GetResID(obj);

			auto origit = m_OriginalIDs.find(id);
			if(origit != m_OriginalIDs.end())
				EraseLiveResource(origit->second);

			ResourceManager::MarkCleanResource(id);
			ResourceManager::RemoveWrapper(ToTypedHandle(Unwrap(obj)));
			ResourceManager::ReleaseCurrentResource(id);
			VkResourceRecord *record = GetRecord(obj);
			if(record)
			{
				// we need to lock here because the app could be creating
				// and deleting from this pool at the same time. We do know
				// though that the pool isn't going to be destroyed while
				// either allocation or freeing happens, so we only need to
				// lock against concurrent allocs or deletes of children.

				if(record->pool)
				{
					// here we lock against concurrent alloc/delete
					record->pool->LockChunks();
					for(auto it = record->pool->pooledChildren.begin(); it != record->pool->pooledChildren.end(); ++it)
					{
						if(*it == record)
						{
							// remove it from our pool so we don't try and destroy it
							record->pool->pooledChildren.erase(it);
							break;
						}
					}
					record->pool->UnlockChunks();
				}
				else if(record->pooledChildren.size())
				{
					// delete all of our children
					for(auto it = record->pooledChildren.begin(); it != record->pooledChildren.end(); ++it)
					{
						// unset record->pool so we don't recurse
						(*it)->pool = NULL;
						VkResourceType restype = IdentifyTypeByPtr((*it)->Resource);
						if(restype == eResDescriptorSet)
							ReleaseWrappedResource((VkDescriptorSet)(uint64_t)(*it)->Resource, true);
						else if(restype == eResCmdBuffer)
							ReleaseWrappedResource((VkCmdBuffer)(*it)->Resource, true);
						else if(restype == eResQueue)
							ReleaseWrappedResource((VkQueue)(*it)->Resource, true);
						else if(restype == eResPhysicalDevice)
							ReleaseWrappedResource((VkPhysicalDevice)(*it)->Resource, true);
						else
							RDCERR("Unexpected resource type %d as pooled child!", restype);
					}
					record->pooledChildren.clear();
				}
				
				record->Delete(this);
			}
			if(clearID)
			{
				// note the nulling of the wrapped object's ID here is rather unpleasant,
				// but the lesser of two evils to ensure that stale descriptor set slots
				// referencing the object behave safely. To do this correctly we would need
				// to maintain a list of back-references to every descriptor set that has
				// this object bound, and invalidate them. Instead we just make sure the ID
				// is always something sensible, since we know the deallocation doesn't
				// free the memory - the object is pool-allocated.
				// If a new object is allocated in that pool slot, it will still be a valid
				// ID and if the resource isn't ever referenced elsewhere, it will just be
				// a non-live ID to be ignored.

				if(IsDispatchableRes(GetWrapped(obj)))
					((WrappedVkDispRes *)GetWrapped(obj))->id = ResourceId();
				else
					((WrappedVkNonDispRes *)GetWrapped(obj))->id = ResourceId();
			}
			delete GetWrapped(obj);
		}
			
	private:
		bool SerialisableResource(ResourceId id, VkResourceRecord *record);

		bool ResourceTypeRelease(WrappedVkRes *res);

		bool Force_InitialState(WrappedVkRes *res);
		bool Need_InitialStateChunk(WrappedVkRes *res);
		bool Prepare_InitialState(WrappedVkRes *res);
		bool Serialise_InitialState(WrappedVkRes *res);
		void Create_InitialState(ResourceId id, WrappedVkRes *live, bool hasData);
		void Apply_InitialState(WrappedVkRes *live, InitialContentData initial);

		WrappedVulkan *m_Core;
};
