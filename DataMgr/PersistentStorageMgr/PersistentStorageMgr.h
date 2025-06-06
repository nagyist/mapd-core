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

#pragma once

#include "DataMgr/AbstractBufferMgr.h"
#include "DataMgr/FileMgr/GlobalFileMgr.h"
#include "DataMgr/ForeignStorage/ForeignStorageCache.h"
#include "DataMgr/ForeignStorage/ForeignStorageMgr.h"

using namespace Data_Namespace;

class PersistentStorageMgr : public AbstractBufferMgr {
 public:
  PersistentStorageMgr(const std::string& data_dir,
                       const size_t num_reader_threads,
                       const File_Namespace::DiskCacheConfig& disk_cache_config);

  AbstractBuffer* createBuffer(const ChunkKey& chunk_key,
                               const size_t page_size,
                               const size_t initial_size) override;
  void deleteBuffer(const ChunkKey& chunk_key, const bool purge) override;
  void deleteBuffersWithPrefix(const ChunkKey& chunk_key_prefix,
                               const bool purge) override;
  AbstractBuffer* getBuffer(const ChunkKey& chunk_key, const size_t num_bytes) override;
  void fetchBuffer(const ChunkKey& chunk_key,
                   AbstractBuffer* destination_buffer,
                   const size_t num_bytes) override;
  AbstractBuffer* putBuffer(const ChunkKey& chunk_key,
                            AbstractBuffer* source_buffer,
                            const size_t num_bytes) override;
  void getChunkMetadataVecForKeyPrefix(ChunkMetadataVector& chunk_metadata,
                                       const ChunkKey& chunk_key_prefix) override;
  bool isBufferOnDevice(const ChunkKey& chunk_key) override;
  std::string printSlabs() override;
  size_t getMaxSize() const override;
  size_t getInUseSize() const override;
  size_t getAllocated() const override;
  bool isAllocationCapped() const override;
  void checkpoint() override;
  void checkpoint(const int db_id, const int tb_id) override;
  AbstractBuffer* alloc(const size_t num_bytes) override;
  void free(AbstractBuffer* buffer) override;
  MgrType getMgrType() override;
  std::string getStringMgrType() override;
  size_t getNumChunks() override;
  void removeTableRelatedDS(const int db_id, const int table_id) override;
  void removeMutableTableCacheData(const int db_id, const int table_id) const;

  File_Namespace::GlobalFileMgr* getGlobalFileMgr() const;
  foreign_storage::ForeignStorageMgr* getForeignStorageMgr() const;
  foreign_storage::ForeignStorageCache* getDiskCache() const;
  inline const File_Namespace::DiskCacheConfig getDiskCacheConfig() const {
    return disk_cache_config_;
  }
  inline const std::shared_ptr<ForeignStorageInterface> getForeignStorageInterface()
      const {
    return fsi_;
  }

 protected:
  bool isForeignStorage(const ChunkKey& chunk_key) const;
  AbstractBufferMgr* getStorageMgrForTableKey(const ChunkKey& table_key) const;
  bool isChunkPrefixCacheable(const ChunkKey& chunk_prefix) const;
  int recoverDataWrapperIfCachedAndGetHighestFragId(const ChunkKey& table_key);

  std::unique_ptr<File_Namespace::GlobalFileMgr> global_file_mgr_;
  std::unique_ptr<foreign_storage::ForeignStorageMgr> foreign_storage_mgr_;
  std::unique_ptr<foreign_storage::ForeignStorageCache> disk_cache_;
  File_Namespace::DiskCacheConfig disk_cache_config_;
  std::shared_ptr<ForeignStorageInterface> fsi_;

 private:
  std::unique_lock<std::mutex> getTableAccessLock(const ChunkKey& table_key);
  std::mutex& getTableAccessMutex(const ChunkKey& table_key);
  void deleteTableAccessMutex(const ChunkKey& table_key);

  std::mutex table_access_mutex_map_mutex_;

  using DbAndTableId = std::pair<int32_t, int32_t>;
  std::map<DbAndTableId, std::mutex> table_access_mutex_map_;
};
