/*
 * Copyright 2022 HEAVY.AI, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "DataMgr/BufferMgr/CpuBufferMgr/CpuBufferMgr.h"

#include "CudaMgr/CudaMgr.h"
#include "DataMgr/Allocators/ArenaAllocator.h"
#include "DataMgr/BufferMgr/CpuBufferMgr/CpuBuffer.h"

namespace Buffer_Namespace {

void CpuBufferMgr::addSlab(const size_t slab_size) {
  CHECK(allocator_);
  slabs_.resize(slabs_.size() + 1);
  try {
    slabs_.back() = reinterpret_cast<int8_t*>(allocator_->allocate(slab_size));
  } catch (std::bad_alloc&) {
    slabs_.resize(slabs_.size() - 1);
    throw FailedToCreateSlab(slab_size);
  }
  slab_segments_.resize(slab_segments_.size() + 1);
  slab_segments_[slab_segments_.size() - 1].emplace_back(0, slab_size / page_size_);
}

void CpuBufferMgr::freeAllMem() {
  CHECK(allocator_);
  initializeMem();
}

Buffer* CpuBufferMgr::createBuffer(BufferList::iterator seg_it, size_t page_size) {
  return new CpuBuffer(this, seg_it, device_id_, cuda_mgr_, page_size);
}

void CpuBufferMgr::initializeMem() {
  allocator_.reset(new DramArena(default_slab_size_ + kArenaBlockOverhead));
}

std::ostream& operator<<(std::ostream& os,
                         const CpuBufferMgr::CpuBufferMgrMemoryUsage& bm) {
  return os << "\"CPU Buffers\": {"
            << "\"Allocated MB\": " << bm.allocated / (1024. * 1024.) << ", "
            << "\"In Use MB\": " << bm.in_use / (1024. * 1024.) << "}";
}

}  // namespace Buffer_Namespace
