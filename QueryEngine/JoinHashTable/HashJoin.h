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

#include <llvm/IR/Value.h>
#include <cstdint>
#include <set>
#include <string>

#include "Analyzer/Analyzer.h"
#include "DataMgr/Allocators/ThrustAllocator.h"
#include "QueryEngine/ColumnarResults.h"
#include "QueryEngine/CompilationOptions.h"
#include "QueryEngine/Descriptors/RowSetMemoryOwner.h"
#include "QueryEngine/ExpressionRange.h"
#include "QueryEngine/InputMetadata.h"
#include "QueryEngine/JoinHashTable/HashTable.h"
#include "QueryEngine/JoinHashTable/Runtime/HashJoinRuntime.h"
#include "Shared/DbObjectKeys.h"
#include "StringOps/StringOpInfo.h"

class CodeGenerator;

class JoinHashTableTooBig : public std::runtime_error {
 public:
  JoinHashTableTooBig(size_t cur_hash_table_size, size_t threshold_size)
      : std::runtime_error("The size of hash table is larger than a threshold (" +
                           ::toString(cur_hash_table_size) + " > " +
                           ::toString(threshold_size) + ")") {}
};

class TooManyHashEntries : public std::runtime_error {
 public:
  TooManyHashEntries()
      : std::runtime_error("Hash tables with more than 4B entries not supported yet") {}

  TooManyHashEntries(const std::string& reason) : std::runtime_error(reason) {}
};

class TableMustBeReplicated : public std::runtime_error {
 public:
  TableMustBeReplicated(const std::string& table_name)
      : std::runtime_error("Hash join failed: Table '" + table_name +
                           "' must be replicated.") {}
};

enum class InnerQualDecision { IGNORE = 0, UNKNOWN, LHS, RHS };

#ifndef __CUDACC__
inline std::ostream& operator<<(std::ostream& os, InnerQualDecision const decision) {
  constexpr char const* strings[]{"IGNORE", "UNKNOWN", "LHS", "RHS"};
  return os << strings[static_cast<int>(decision)];
}
#endif

class HashJoinFail : public std::runtime_error {
 public:
  HashJoinFail(const std::string& err_msg)
      : std::runtime_error(err_msg), inner_qual_decision(InnerQualDecision::UNKNOWN) {}
  HashJoinFail(const std::string& err_msg, InnerQualDecision qual_decision)
      : std::runtime_error(err_msg), inner_qual_decision(qual_decision) {}

  InnerQualDecision inner_qual_decision;
};

class NeedsOneToManyHash : public HashJoinFail {
 public:
  NeedsOneToManyHash() : HashJoinFail("Needs one to many hash") {}
};

class FailedToFetchColumn : public HashJoinFail {
 public:
  FailedToFetchColumn()
      : HashJoinFail("Not enough memory for columns involved in join") {}
};

class FailedToJoinOnVirtualColumn : public HashJoinFail {
 public:
  FailedToJoinOnVirtualColumn() : HashJoinFail("Cannot join on rowid") {}
};

class TooBigHashTableForBoundingBoxIntersect : public HashJoinFail {
 public:
  TooBigHashTableForBoundingBoxIntersect(const size_t bbox_intersect_hash_table_max_bytes)
      : HashJoinFail(
            "Could not create hash table for bounding box intersection with less than "
            "max allowed size of " +
            std::to_string(bbox_intersect_hash_table_max_bytes) + " bytes") {}
};

using InnerOuter = std::pair<const Analyzer::ColumnVar*, const Analyzer::Expr*>;
using InnerOuterStringOpInfos = std::pair<std::vector<StringOps_Namespace::StringOpInfo>,
                                          std::vector<StringOps_Namespace::StringOpInfo>>;

struct ColumnsForDevice {
  std::vector<JoinColumn> join_columns;
  std::vector<JoinColumnTypeInfo> join_column_types;
  std::vector<std::shared_ptr<Chunk_NS::Chunk>> chunks_owner;
  std::vector<JoinBucketInfo> join_buckets;
  std::vector<std::shared_ptr<void>> malloc_owner;

  void setBucketInfo(const std::vector<double>& bucket_sizes_for_dimension,
                     const std::vector<InnerOuter> inner_outer_pairs);
};

struct HashJoinMatchingSet {
  llvm::Value* elements;
  llvm::Value* count;
  llvm::Value* slot;
  llvm::Value* error_code;
};

struct CompositeKeyInfo {
  std::vector<const void*> sd_inner_proxy_per_key;
  std::vector<void*> sd_outer_proxy_per_key;
  std::unordered_map<int, ChunkKey> cache_key_chunks;  // used for the cache key
};

class DeviceAllocator;

class HashJoin {
 public:
  static constexpr size_t MAX_NUM_HASH_ENTRIES = size_t(1) << 31;
  virtual std::string toString(const ExecutorDeviceType device_type,
                               const int device_id = 0,
                               bool raw = false) const = 0;

  virtual std::string toStringFlat64(const ExecutorDeviceType device_type,
                                     const int device_id) const;

  virtual std::string toStringFlat32(const ExecutorDeviceType device_type,
                                     const int device_id) const;

  virtual DecodedJoinHashBufferSet toSet(const ExecutorDeviceType device_type,
                                         const int device_id) const = 0;

  virtual llvm::Value* codegenSlot(const CompilationOptions&, const size_t) = 0;

  virtual HashJoinMatchingSet codegenMatchingSet(const CompilationOptions&,
                                                 const size_t) = 0;

  virtual shared::TableKey getInnerTableId() const noexcept = 0;

  virtual int getInnerTableRteIdx() const noexcept = 0;

  virtual HashType getHashType() const noexcept = 0;

  static size_t getMaximumNumHashEntriesCanHold(MemoryLevel memory_level,
                                                const Executor* executor,
                                                size_t rowid_size) noexcept;

  static std::string generateTooManyHashEntriesErrMsg(size_t num_entries,
                                                      size_t threshold,
                                                      MemoryLevel memory_level) {
    std::ostringstream oss;
    oss << "Hash tables with more than " << threshold
        << " entries (# hash entries: " << num_entries << ") on "
        << ::toString(memory_level) << " not supported yet";
    return oss.str();
  }

  static bool layoutRequiresAdditionalBuffers(HashType layout) noexcept {
    return (layout == HashType::ManyToMany || layout == HashType::OneToMany);
  }

  static std::string getHashTypeString(HashType ht) noexcept {
    const char* HashTypeStrings[3] = {"OneToOne", "OneToMany", "ManyToMany"};
    return HashTypeStrings[static_cast<int>(ht)];
  };

  static HashJoinMatchingSet codegenMatchingSet(
      const std::vector<llvm::Value*>& hash_join_idx_args_in,
      const bool is_sharded,
      const bool col_is_nullable,
      const bool is_bw_eq,
      const int64_t sub_buff_size,
      Executor* executor,
      const bool is_bucketized = false);

  static llvm::Value* codegenHashTableLoad(const size_t table_idx, Executor* executor);

  virtual Data_Namespace::MemoryLevel getMemoryLevel() const noexcept = 0;

  virtual size_t offsetBufferOff() const noexcept = 0;

  virtual size_t countBufferOff() const noexcept = 0;

  virtual size_t payloadBufferOff() const noexcept = 0;

  virtual std::string getHashJoinType() const = 0;

  virtual bool isBitwiseEq() const = 0;

  JoinColumn fetchJoinColumn(
      const Analyzer::ColumnVar* hash_col,
      const std::vector<Fragmenter_Namespace::FragmentInfo>& fragment_info,
      const Data_Namespace::MemoryLevel effective_memory_level,
      const int device_id,
      std::vector<std::shared_ptr<Chunk_NS::Chunk>>& chunks_owner,
      DeviceAllocator* dev_buff_owner,
      std::vector<std::shared_ptr<void>>& malloc_owner,
      Executor* executor,
      ColumnCacheMap* column_cache);

  //! Make hash table from an in-flight SQL query's parse tree etc.
  static std::shared_ptr<HashJoin> getInstance(
      const std::shared_ptr<Analyzer::BinOper> qual_bin_oper,
      const std::vector<InputTableInfo>& query_infos,
      const Data_Namespace::MemoryLevel memory_level,
      const JoinType join_type,
      const HashType preferred_hash_type,
      const std::set<int>& device_ids,
      ColumnCacheMap& column_cache,
      Executor* executor,
      const HashTableBuildDagMap& hashtable_build_dag_map,
      const RegisteredQueryHint& query_hint,
      const TableIdToNodeMap& table_id_to_node_map);

  //! Make hash table from named tables and columns (such as for testing).
  static std::shared_ptr<HashJoin> getSyntheticInstance(
      std::string_view table1,
      std::string_view column1,
      const Catalog_Namespace::Catalog& catalog1,
      std::string_view table2,
      std::string_view column2,
      const Catalog_Namespace::Catalog& catalog2,
      const Data_Namespace::MemoryLevel memory_level,
      const HashType preferred_hash_type,
      const std::set<int>& device_ids,
      ColumnCacheMap& column_cache,
      Executor* executor);

  //! Make hash table from named tables and columns (such as for testing).
  static std::shared_ptr<HashJoin> getSyntheticInstance(
      const std::shared_ptr<Analyzer::BinOper> qual_bin_oper,
      const Data_Namespace::MemoryLevel memory_level,
      const HashType preferred_hash_type,
      const std::set<int>& device_ids,
      ColumnCacheMap& column_cache,
      Executor* executor);

  static std::pair<std::string, std::shared_ptr<HashJoin>> getSyntheticInstance(
      std::vector<std::shared_ptr<Analyzer::BinOper>>,
      const Data_Namespace::MemoryLevel memory_level,
      const HashType preferred_hash_type,
      const std::set<int>& device_ids,
      ColumnCacheMap& column_cache,
      Executor* executor);

  static shared::TableKey getInnerTableId(
      const std::vector<InnerOuter>& inner_outer_pairs) {
    CHECK(!inner_outer_pairs.empty());
    const auto first_inner_col = inner_outer_pairs.front().first;
    return first_inner_col->getTableKey();
  }

  static bool canAccessHashTable(bool allow_hash_table_recycling,
                                 bool invalid_cache_key,
                                 JoinType join_type);

  static void checkHashJoinReplicationConstraint(const shared::TableKey& table_key,
                                                 const size_t shard_count,
                                                 const Executor* executor);

  // Swap the columns if needed and make the inner column the first component.
  static std::pair<InnerOuter, InnerOuterStringOpInfos> normalizeColumnPair(
      const Analyzer::Expr* lhs,
      const Analyzer::Expr* rhs,
      const TemporaryTables* temporary_tables,
      const bool is_bbox_intersect = false);

  template <typename T>
  static const T* getHashJoinColumn(const Analyzer::Expr* expr);

  // Normalize each expression tuple
  static std::pair<std::vector<InnerOuter>, std::vector<InnerOuterStringOpInfos>>
  normalizeColumnPairs(const Analyzer::BinOper* condition,
                       const TemporaryTables* temporary_tables);

  size_t getJoinHashBufferSize(const ExecutorDeviceType device_type) {
    CHECK(device_type == ExecutorDeviceType::CPU);
    constexpr int cpu_device_id = 0;
    return getJoinHashBufferSize(device_type, cpu_device_id);
  }

  size_t getJoinHashBufferSize(const ExecutorDeviceType device_type,
                               const int device_id) const {
    std::shared_lock<std::shared_mutex> read_lock(hash_tables_mutex_);
    if (hash_tables_for_device_.empty()) {
      return 0;
    }
    auto hash_table = hash_tables_for_device_.begin()->second.get();
    CHECK(hash_table);
    return hash_table->getHashTableBufferSize(device_type);
  }

  int8_t* getJoinHashBuffer(const ExecutorDeviceType device_type,
                            const int device_id) const {
    std::shared_lock<std::shared_mutex> read_lock(hash_tables_mutex_);
    auto it = hash_tables_for_device_.find(device_id);
    if (it == hash_tables_for_device_.end()) {
      return nullptr;
    }
    auto hash_table = it->second.get();
#ifdef HAVE_CUDA
    if (device_type == ExecutorDeviceType::CPU) {
      return hash_table->getCpuBuffer();
    } else {
      CHECK(hash_table);
      const auto gpu_buff = hash_table->getGpuBuffer();
      return gpu_buff;
    }
#else
    CHECK(device_type == ExecutorDeviceType::CPU);
    return hash_table->getCpuBuffer();
#endif
  }

  void freeHashBufferMemory() {
    std::unique_lock<std::shared_mutex> write_lock(hash_tables_mutex_);
    auto empty_hash_tables = decltype(hash_tables_for_device_)();
    hash_tables_for_device_.swap(empty_hash_tables);
  }

  std::shared_ptr<HashTable> getHashTableForDevice(int device_id) const {
    std::shared_lock<std::shared_mutex> read_lock(hash_tables_mutex_);
    auto it = hash_tables_for_device_.find(device_id);
    CHECK(it != hash_tables_for_device_.end());
    auto hash_table = it->second;
    CHECK(hash_table);
    return hash_table;
  }

  void replaceHashTableForDevice(std::shared_ptr<HashTable>&& hash_table, int device_id) {
    std::unique_lock<std::shared_mutex> write_lock(hash_tables_mutex_);
    auto it = hash_tables_for_device_.find(device_id);
    CHECK(it != hash_tables_for_device_.end());
    hash_tables_for_device_[device_id] = std::move(hash_table);
  }

  std::shared_ptr<HashTable> getAnyHashTableForDevice() const {
    std::shared_lock<std::shared_mutex> read_lock(hash_tables_mutex_);
    if (hash_tables_for_device_.empty()) {
      return nullptr;
    }
    return hash_tables_for_device_.begin()->second;
  }

  void moveHashTableForDevice(std::shared_ptr<HashTable>&& hash_table, int device_id) {
    std::unique_lock<std::shared_mutex> write_lock(hash_tables_mutex_);
    auto [it, success] =
        hash_tables_for_device_.emplace(device_id, std::move(hash_table));
    CHECK(success);
  }

  void putHashTableForDevice(std::shared_ptr<HashTable> hash_table, int device_id) {
    std::unique_lock<std::shared_mutex> write_lock(hash_tables_mutex_);
    auto [it, success] = hash_tables_for_device_.emplace(device_id, hash_table);
    CHECK(success);
  }

  void clearHashTableForDevice() {
    std::unique_lock<std::shared_mutex> write_lock(hash_tables_mutex_);
    hash_tables_for_device_.clear();
  }

  static std::vector<int> collectFragmentIds(
      const std::vector<Fragmenter_Namespace::FragmentInfo>& fragments);

  static CompositeKeyInfo getCompositeKeyInfo(
      const std::vector<InnerOuter>& inner_outer_pairs,
      const Executor* executor,
      const std::vector<InnerOuterStringOpInfos>& inner_outer_string_op_infos_pairs = {});

  static std::vector<const StringDictionaryProxy::IdMap*>
  translateCompositeStrDictProxies(
      const CompositeKeyInfo& composite_key_info,
      const std::vector<InnerOuterStringOpInfos>& string_op_infos_for_keys,
      const Executor* executor);

  static std::pair<const StringDictionaryProxy*, StringDictionaryProxy*>
  getStrDictProxies(const InnerOuter& cols,
                    const Executor* executor,
                    const bool has_string_ops);

  static const StringDictionaryProxy::IdMap* translateInnerToOuterStrDictProxies(
      const InnerOuter& cols,
      const InnerOuterStringOpInfos& inner_outer_string_op_infos,
      ExpressionRange& old_col_range,
      const Executor* executor);

 protected:
  static llvm::Value* codegenColOrStringOper(
      const Analyzer::Expr* col_or_string_oper,
      const std::vector<StringOps_Namespace::StringOpInfo>& string_op_infos,
      CodeGenerator& code_generator,
      const CompilationOptions& co);

  virtual size_t getComponentBufferSize() const noexcept = 0;

  mutable std::shared_mutex hash_tables_mutex_;
  std::unordered_map<int, std::shared_ptr<HashTable>> hash_tables_for_device_;
};

std::ostream& operator<<(std::ostream& os, const DecodedJoinHashBufferEntry& e);

std::ostream& operator<<(std::ostream& os, const DecodedJoinHashBufferSet& s);

std::ostream& operator<<(std::ostream& os,
                         const InnerOuterStringOpInfos& inner_outer_string_op_infos);
std::ostream& operator<<(
    std::ostream& os,
    const std::vector<InnerOuterStringOpInfos>& inner_outer_string_op_infos_pairs);

std::string toString(const InnerOuterStringOpInfos& inner_outer_string_op_infos);

std::string toString(
    const std::vector<InnerOuterStringOpInfos>& inner_outer_string_op_infos_pairs);

std::shared_ptr<Analyzer::ColumnVar> getSyntheticColumnVar(
    std::string_view table,
    std::string_view column,
    int rte_idx,
    const Catalog_Namespace::Catalog& catalog);

size_t get_shard_count(const Analyzer::BinOper* join_condition, const Executor* executor);

size_t get_shard_count(
    std::pair<const Analyzer::ColumnVar*, const Analyzer::Expr*> equi_pair,
    const Executor* executor);
