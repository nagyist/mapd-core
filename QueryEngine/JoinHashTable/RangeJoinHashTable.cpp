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
#include "QueryEngine/JoinHashTable/RangeJoinHashTable.h"

#include "QueryEngine/CodeGenerator.h"
#include "QueryEngine/Execute.h"
#include "QueryEngine/JoinHashTable/Builders/BaselineHashTableBuilder.h"
#include "QueryEngine/JoinHashTable/PerfectJoinHashTable.h"
#include "QueryEngine/JoinHashTable/Runtime/HashJoinKeyHandlers.h"
#include "QueryEngine/JoinHashTable/Runtime/JoinHashTableGpuUtils.h"
#include "QueryEngine/LoopControlFlow/JoinLoop.h"

// clang-format off
///
/// NOTE(jclay): Handling Range Joins With Mixed Compression:
/// ========================================
///
///  First, let's take a concrete example of a query that is rewritten as a range join.
///  Notice in the first code block, that the condition operator is a bounding box intersection operator.
///  The LHS is a column, and the RHS is the range operator. In order to have the hash table
///  build and probe work properly, we need to ensure that the approriate runtime functions 
///  are selected. The following breakdown is provided to help document how the appropriate 
///  runtime funditon is selected.
///
///    * The LHS of the RangeOper is used to build the hash table
///    * The LHS of the BoundingBoxIntersectionOper + the RHS of the RangeOper is used as probe
///
/// SELECT count(*) FROM t1, t2 where ST_Distance(t1.p1_comp32, t2.p1) <= 6.3;
///
///   BinOper condition
///   -----------------------
///   ((BoundingBoxIntersect)
///     (ColumnVar table: (t1) column: (p1_comp32) GEOMETRY(POINT, 4326) ENCODING  COMPRESSED(32))
///     (RangeOper)
///        (ColumnVar table: (t2) column: (p1) GEOMETRY(POINT, 4326) ENCODING NONE),
///        (Const 6.330000))
/// 
///
///   RangeOper condition
///   -----------------------
///
///     [(ColumnVar table: 5 (t2) column: 1 rte: 1 GEOMETRY(POINT, 4326) ENCODING NONE),
///      (Const 6.330000)]
///
/// Same example as above, annotated:
///
///   SELECT count(*) FROM t1, t2 where 
///          ST_Distance(
///                        t1.p1_comp32,      << Bounding Box Intersection Condition LHS
///                        t2.p1              << RangeOper LHS
///                     ) <= 6.3;             << RangeOper RHS
///
/// In this case, we select the uncompressed runtime functions when building the hash table 
/// over t2.p1. When performing the probe, we must select the *compressed* runtime functions.
///
// clang-format on

std::shared_ptr<RangeJoinHashTable> RangeJoinHashTable::getInstance(
    const std::shared_ptr<Analyzer::BinOper> condition,
    const Analyzer::RangeOper* range_expr,
    const std::vector<InputTableInfo>& query_infos,
    const Data_Namespace::MemoryLevel memory_level,
    const JoinType join_type,
    const std::set<int>& device_ids,
    ColumnCacheMap& column_cache,
    Executor* executor,
    const HashTableBuildDagMap& hashtable_build_dag_map,
    const RegisteredQueryHint& query_hints,
    const TableIdToNodeMap& table_id_to_node_map) {
  // the hash table is built over the LHS of the range oper. we then use the lhs
  // of the bin oper + the rhs of the range oper for the probe
  auto range_expr_col_var =
      dynamic_cast<const Analyzer::ColumnVar*>(range_expr->get_left_operand());
  if (!range_expr_col_var || !range_expr_col_var->get_type_info().is_geometry()) {
    throw HashJoinFail("Could not build hash tables for range join | " +
                       range_expr->toString());
  }

  CHECK(range_expr_col_var->get_type_info().is_geometry());

  auto coords_column_key = range_expr_col_var->getColumnKey();
  coords_column_key.column_id = coords_column_key.column_id + 1;
  const auto coords_cd = Catalog_Namespace::get_metadata_for_column(coords_column_key);
  CHECK(coords_cd);

  auto range_join_inner_col_expr = makeExpr<Analyzer::ColumnVar>(
      coords_cd->columnType, coords_column_key, range_expr_col_var->get_rte_idx());

  std::vector<InnerOuter> inner_outer_pairs;
  inner_outer_pairs.emplace_back(
      InnerOuter{dynamic_cast<Analyzer::ColumnVar*>(range_join_inner_col_expr.get()),
                 condition->get_left_operand()});

  const auto& query_info =
      get_inner_query_info(HashJoin::getInnerTableId(inner_outer_pairs), query_infos)
          .info;

  const auto total_entries = 2 * query_info.getNumTuplesUpperBound();
  if (total_entries > HashJoin::MAX_NUM_HASH_ENTRIES) {
    throw TooManyHashEntries();
  }

  const auto shard_count = memory_level == Data_Namespace::GPU_LEVEL
                               ? BaselineJoinHashTable::getShardCountForCondition(
                                     condition.get(), executor, inner_outer_pairs)
                               : 0;

  auto join_hash_table = std::make_shared<RangeJoinHashTable>(condition,
                                                              join_type,
                                                              range_expr,
                                                              range_join_inner_col_expr,
                                                              query_infos,
                                                              memory_level,
                                                              column_cache,
                                                              executor,
                                                              inner_outer_pairs,
                                                              device_ids,
                                                              query_hints,
                                                              hashtable_build_dag_map,
                                                              table_id_to_node_map);
  HashJoin::checkHashJoinReplicationConstraint(
      HashJoin::getInnerTableId(inner_outer_pairs), shard_count, executor);
  try {
    join_hash_table->reifyWithLayout(HashType::OneToMany);
  } catch (const HashJoinFail& e) {
    throw HashJoinFail(std::string("Could not build a 1-to-1 correspondence for columns "
                                   "involved in equijoin | ") +
                       e.what());
  } catch (const ColumnarConversionNotSupported& e) {
    throw HashJoinFail(std::string("Could not build hash tables for equijoin | ") +
                       e.what());
  } catch (const JoinHashTableTooBig& e) {
    throw e;
  } catch (const std::exception& e) {
    LOG(FATAL) << "Fatal error while attempting to build hash tables for join: "
               << e.what();
  }

  return join_hash_table;
}

void RangeJoinHashTable::reifyWithLayout(const HashType layout) {
  auto timer = DEBUG_TIMER(__func__);
  CHECK(layout == HashType::OneToMany);

  const auto& query_info =
      get_inner_query_info(HashJoin::getInnerTableId(inner_outer_pairs_), query_infos_)
          .info;

  if (query_info.fragments.empty()) {
    return;
  }

  const auto& table_key = getInnerTableId();
  VLOG(1) << "Reify with layout " << getHashTypeString(layout) << "for " << table_key;

  std::unordered_map<int, ColumnsForDevice> columns_per_device;
  std::unordered_map<int, std::vector<Fragmenter_Namespace::FragmentInfo>>
      fragments_per_device;
  const auto shard_count = shardCount();
  effective_memory_level_ = getEffectiveMemoryLevel(inner_outer_pairs_);
  for (auto device_id : device_ids_) {
    fragments_per_device.emplace(
        device_id,
        shard_count
            ? only_shards_for_device(
                  query_info.fragments, effective_memory_level_, device_id, device_ids_)
            : query_info.fragments);
    DeviceAllocator* device_allocator{nullptr};
    if (memory_level_ == Data_Namespace::MemoryLevel::GPU_LEVEL) {
      device_allocator = executor_->getCudaAllocator(device_id);
      CHECK(device_allocator);
    }
    // for bounding box intersection, we need to fetch columns regardless of the
    // availability of cached hash table to calculate various params, i.e., bucket size
    // info todo(yoonmin) : relax this
    const auto columns_for_device = fetchColumnsForDevice(
        fragments_per_device[device_id], device_id, device_allocator);
    columns_per_device.emplace(device_id, columns_for_device);
  }

  inverse_bucket_sizes_for_dimension_.clear();

  const auto bucket_range =
      dynamic_cast<const Analyzer::Constant*>(range_expr_->get_right_operand());

  CHECK(bucket_range);
  CHECK(bucket_range->get_type_info().is_fp() &&
        bucket_range->get_type_info().get_size() == 8);  // TODO

  const auto bucket_range_datum = bucket_range->get_constval();

  inverse_bucket_sizes_for_dimension_.emplace_back(1. / bucket_range_datum.doubleval);
  inverse_bucket_sizes_for_dimension_.emplace_back(1. / bucket_range_datum.doubleval);

  setInverseBucketSizeInfo(inverse_bucket_sizes_for_dimension_, columns_per_device);

  // to properly lookup cached hash table, we need to use join columns listed as lhs and
  // rhs of the bbox intersect op instead of physical (and hidden) column tailored to
  // range join expr in other words, we need to use geometry column (point) instead of its
  // hidden array column i.e., see `get_physical_cols` function
  std::vector<InnerOuter> inner_outer_pairs_for_cache_lookup;
  inner_outer_pairs_for_cache_lookup.emplace_back(InnerOuter{
      dynamic_cast<const Analyzer::ColumnVar*>(range_expr_->get_left_operand()),
      condition_->get_left_operand()});
  auto hashtable_access_path_info =
      HashtableRecycler::getHashtableAccessPathInfo(inner_outer_pairs_for_cache_lookup,
                                                    {},
                                                    condition_->get_optype(),
                                                    join_type_,
                                                    hashtable_build_dag_map_,
                                                    device_ids_,
                                                    shard_count,
                                                    fragments_per_device,
                                                    executor_);
  hashtable_cache_key_ = hashtable_access_path_info.hashed_query_plan_dag;
  table_keys_ = hashtable_access_path_info.table_keys;

  auto get_inner_table_key = [&inner_outer_pairs_for_cache_lookup]() {
    auto col_var = inner_outer_pairs_for_cache_lookup.front().first;
    return col_var->getTableKey();
  };

  if (table_keys_.empty()) {
    const auto& inner_table_key = get_inner_table_key();
    table_keys_ = DataRecyclerUtil::getAlternativeTableKeys(
        composite_key_info_.cache_key_chunks, inner_table_key);
  }
  CHECK(!table_keys_.empty());

  setBoundingBoxIntersectionMetaInfo(
      max_hashtable_size_, bucket_threshold_, inverse_bucket_sizes_for_dimension_);
  generateCacheKey(max_hashtable_size_,
                   bucket_threshold_,
                   inverse_bucket_sizes_for_dimension_,
                   fragments_per_device,
                   device_ids_);

  if (HashtableRecycler::isInvalidHashTableCacheKey(hashtable_cache_key_) &&
      get_inner_table_key().table_id > 0) {
    std::vector<size_t> per_device_chunk_key;
    for (auto device_id : device_ids_) {
      auto chunk_key_hash = boost::hash_value(composite_key_info_.cache_key_chunks);
      boost::hash_combine(chunk_key_hash,
                          HashJoin::collectFragmentIds(fragments_per_device[device_id]));
      per_device_chunk_key.push_back(chunk_key_hash);
      AlternativeCacheKeyForBoundingBoxIntersection cache_key{
          inner_outer_pairs_for_cache_lookup,
          columns_per_device.begin()->second.join_columns.front().num_elems,
          chunk_key_hash,
          condition_->get_optype(),
          max_hashtable_size_,
          bucket_threshold_,
          {}};
      hashtable_cache_key_[device_id] = getAlternativeCacheKey(cache_key);
      hash_table_cache_->addQueryPlanDagForTableKeys(hashtable_cache_key_[device_id],
                                                     table_keys_);
    }
  }

  if (effective_memory_level_ == Data_Namespace::MemoryLevel::CPU_LEVEL) {
    std::lock_guard<std::mutex> cpu_hash_table_buff_lock(cpu_hash_table_buff_mutex_);
    if (auto generic_hash_table =
            initHashTableOnCpuFromCache(hashtable_cache_key_.begin()->second,
                                        CacheItemType::BBOX_INTERSECT_HT,
                                        DataRecyclerUtil::CPU_DEVICE_IDENTIFIER)) {
      if (auto hash_table =
              std::dynamic_pointer_cast<BaselineHashTable>(generic_hash_table)) {
        // See if a hash table of a different layout was returned.
        // If it was OneToMany, we can reuse it on ManyToMany.
        if (layout == HashType::ManyToMany &&
            hash_table->getLayout() == HashType::OneToMany) {
          // use the cached hash table
          layout_override_ = HashType::ManyToMany;
        }

        if (memory_level_ == Data_Namespace::MemoryLevel::GPU_LEVEL) {
#ifdef HAVE_CUDA
          for (auto device_id : device_ids_) {
            copyCpuHashTableToGpu(hash_table, device_id);
          }
#else
          UNREACHABLE();
#endif
        } else {
          CHECK_EQ(Data_Namespace::CPU_LEVEL, memory_level_);
          // do not move hash_table to keep valid ptr of it within the hash table recycler
          constexpr int cpu_device_id = 0;
          putHashTableForDevice(hash_table, cpu_device_id);
        }
        return;
      }
    }
  }

  auto [entry_count, emitted_keys_count] =
      computeRangeHashTableCounts(shard_count, columns_per_device);

  size_t hash_table_size = BoundingBoxIntersectJoinHashTable::calculateHashTableSize(
      inverse_bucket_sizes_for_dimension_.size(), emitted_keys_count, entry_count);

  VLOG(1) << "Finalized range join hash table: entry count " << entry_count
          << " hash table size " << hash_table_size;

  std::vector<std::future<void>> init_threads;
  for (auto device_id : device_ids_) {
    init_threads.push_back(
        std::async(std::launch::async,
                   &RangeJoinHashTable::reifyForDevice,
                   this,
                   /* columns_for_device      */ columns_per_device[device_id],
                   /* layout_type             */ layout,
                   /* entry_count             */ entry_count,
                   /* emitted_keys_count      */ emitted_keys_count,
                   /* device_id               */ device_id,
                   /* parent_thread_local_ids */ logger::thread_local_ids()));
  }
  for (auto& init_thread : init_threads) {
    init_thread.wait();
  }
  for (auto& init_thread : init_threads) {
    init_thread.get();
  }
}

void RangeJoinHashTable::reifyForDevice(
    const ColumnsForDevice& columns_for_device,
    const HashType layout,
    const size_t entry_count,
    const size_t emitted_keys_count,
    const int device_id,
    const logger::ThreadLocalIds parent_thread_local_ids) {
  logger::LocalIdsScopeGuard lisg = parent_thread_local_ids.setNewThreadId();
  DEBUG_TIMER_NEW_THREAD(parent_thread_local_ids.thread_id_);
  CHECK_EQ(getKeyComponentWidth(), size_t(8));
  CHECK(layoutRequiresAdditionalBuffers(layout));
  BaselineHashTableEntryInfo hash_table_entry_info(entry_count,
                                                   emitted_keys_count,
                                                   sizeof(int32_t),
                                                   getKeyComponentCount(),
                                                   getKeyComponentWidth(),
                                                   layout,
                                                   false);
  if (effective_memory_level_ == Data_Namespace::MemoryLevel::CPU_LEVEL) {
    VLOG(1) << "Building range join hash table on CPU.";
    auto hash_table = initHashTableOnCpu(columns_for_device.join_columns,
                                         columns_for_device.join_column_types,
                                         columns_for_device.join_buckets,
                                         hash_table_entry_info);
    CHECK(hash_table);
#ifdef HAVE_CUDA
    if (memory_level_ == Data_Namespace::MemoryLevel::GPU_LEVEL) {
      copyCpuHashTableToGpu(hash_table, device_id);
    } else {
#else
    CHECK_EQ(Data_Namespace::CPU_LEVEL, effective_memory_level_);
#endif
      moveHashTableForDevice(std::move(hash_table), device_id);
#ifdef HAVE_CUDA
    }
#endif
  } else {
#ifdef HAVE_CUDA
    auto hash_table = initHashTableOnGpu(columns_for_device.join_columns,
                                         columns_for_device.join_column_types,
                                         columns_for_device.join_buckets,
                                         hash_table_entry_info,
                                         device_id);
    moveHashTableForDevice(std::move(hash_table), device_id);
#else
    UNREACHABLE();
#endif
  }
}
// #endif

#ifdef HAVE_CUDA
std::shared_ptr<BaselineHashTable> RangeJoinHashTable::initHashTableOnGpu(
    const std::vector<JoinColumn>& join_columns,
    const std::vector<JoinColumnTypeInfo>& join_column_types,
    const std::vector<JoinBucketInfo>& join_bucket_info,
    const BaselineHashTableEntryInfo hash_table_entry_info,
    const size_t device_id) {
  CHECK_EQ(memory_level_, Data_Namespace::MemoryLevel::GPU_LEVEL);

  VLOG(1) << "Building range join hash table on GPU.";

  BaselineJoinHashTableBuilder builder;
  auto device_allocator = executor_->getCudaAllocator(device_id);
  CHECK(device_allocator);
  auto join_columns_gpu = transfer_vector_of_flat_objects_to_gpu(
      join_columns, *device_allocator, "Range hash join input column(s)");
  CHECK_EQ(join_columns.size(), 1u);
  CHECK(!join_bucket_info.empty());

  auto& inverse_bucket_sizes_for_dimension =
      join_bucket_info[0].inverse_bucket_sizes_for_dimension;

  auto bucket_sizes_gpu =
      transfer_vector_of_flat_objects_to_gpu(inverse_bucket_sizes_for_dimension,
                                             *device_allocator,
                                             "Range join hashtable bucket sizes");

  const auto key_handler = RangeKeyHandler(isInnerColCompressed(),
                                           inverse_bucket_sizes_for_dimension.size(),
                                           join_columns_gpu,
                                           bucket_sizes_gpu);

  const auto err = builder.initHashTableOnGpu(&key_handler,
                                              join_columns,
                                              join_type_,
                                              hash_table_entry_info,
                                              device_id,
                                              executor_,
                                              query_hints_);
  if (err) {
    throw HashJoinFail(
        std::string("Unrecognized error when initializing GPU range join hash table (") +
        std::to_string(err) + std::string(")"));
  }
  return builder.getHashTable();
}
#endif

std::shared_ptr<BaselineHashTable> RangeJoinHashTable::initHashTableOnCpu(
    const std::vector<JoinColumn>& join_columns,
    const std::vector<JoinColumnTypeInfo>& join_column_types,
    const std::vector<JoinBucketInfo>& join_bucket_info,
    const BaselineHashTableEntryInfo hash_table_entry_info) {
  auto timer = DEBUG_TIMER(__func__);
  decltype(std::chrono::steady_clock::now()) ts1, ts2;
  ts1 = std::chrono::steady_clock::now();
  const auto composite_key_info =
      HashJoin::getCompositeKeyInfo(inner_outer_pairs_, executor_);
  CHECK(!join_columns.empty());
  CHECK(!join_bucket_info.empty());

  CHECK(layoutRequiresAdditionalBuffers(hash_table_entry_info.getHashTableLayout()));
  const auto key_component_count =
      join_bucket_info[0].inverse_bucket_sizes_for_dimension.size();

  auto key_handler =
      RangeKeyHandler(isInnerColCompressed(),
                      key_component_count,
                      &join_columns[0],
                      join_bucket_info[0].inverse_bucket_sizes_for_dimension.data());

  BaselineJoinHashTableBuilder builder;
  const StrProxyTranslationMapsPtrsAndOffsets
      dummy_str_proxy_translation_maps_ptrs_and_offsets;
  const auto err =
      builder.initHashTableOnCpu(&key_handler,
                                 composite_key_info,
                                 join_columns,
                                 join_column_types,
                                 join_bucket_info,
                                 dummy_str_proxy_translation_maps_ptrs_and_offsets,
                                 hash_table_entry_info,
                                 join_type_,
                                 executor_,
                                 query_hints_);
  ts2 = std::chrono::steady_clock::now();
  if (err) {
    throw HashJoinFail(std::string("Unrecognized error when initializing CPU "
                                   "range join hash table (") +
                       std::to_string(err) + std::string(")"));
  }
  std::shared_ptr<BaselineHashTable> hash_table = builder.getHashTable();
  auto hashtable_build_time =
      std::chrono::duration_cast<std::chrono::milliseconds>(ts2 - ts1).count();
  putHashTableOnCpuToCache(hashtable_cache_key_.begin()->second,
                           CacheItemType::BBOX_INTERSECT_HT,
                           hash_table,
                           DataRecyclerUtil::CPU_DEVICE_IDENTIFIER,
                           hashtable_build_time);
  return hash_table;
}

std::pair<size_t, size_t> RangeJoinHashTable::computeRangeHashTableCounts(
    const size_t shard_count,
    std::unordered_map<int, ColumnsForDevice>& columns_per_device) {
  CHECK(!inverse_bucket_sizes_for_dimension_.empty());
  const auto [tuple_count, emitted_keys_count] =
      approximateTupleCount(inverse_bucket_sizes_for_dimension_,
                            columns_per_device,
                            max_hashtable_size_,
                            bucket_threshold_);
  const auto entry_count = 2 * std::max(tuple_count, size_t(1));

  return std::make_pair(
      get_entries_per_device(entry_count, shard_count, device_ids_, memory_level_),
      emitted_keys_count);
}

std::pair<size_t, size_t> RangeJoinHashTable::approximateTupleCount(
    const std::vector<double>& inverse_bucket_sizes_for_dimension,
    std::unordered_map<int, ColumnsForDevice>& columns_per_device,
    const size_t chosen_max_hashtable_size,
    const double chosen_bucket_threshold) {
#ifdef _WIN32
  // WIN32 needs have C++20 set for designated initialisation to work
  CountDistinctDescriptor count_distinct_desc{
      CountDistinctImplType::Bitmap,
      0,
      0,
      11,
      true,
      effective_memory_level_ == Data_Namespace::MemoryLevel::GPU_LEVEL
          ? ExecutorDeviceType::GPU
          : ExecutorDeviceType::CPU,
      1,
  };
#else
  CountDistinctDescriptor count_distinct_desc{
      .impl_type_ = CountDistinctImplType::Bitmap,
      .min_val = 0,
      .bucket_size = 0,
      .bitmap_sz_bits = 11,
      .approximate = true,
      .device_type = effective_memory_level_ == Data_Namespace::MemoryLevel::GPU_LEVEL
                         ? ExecutorDeviceType::GPU
                         : ExecutorDeviceType::CPU,
      .sub_bitmap_count = 1,
  };
#endif
  const auto padded_size_bytes = count_distinct_desc.bitmapPaddedSizeBytes();

  CHECK(!columns_per_device.empty() &&
        !columns_per_device.begin()->second.join_columns.empty());
  if (columns_per_device.begin()->second.join_columns.front().num_elems == 0) {
    return std::make_pair(0, 0);
  }

  for (auto& kv : columns_per_device) {
    kv.second.setBucketInfo(inverse_bucket_sizes_for_dimension, inner_outer_pairs_);
  }

  // Number of keys must match dimension of buckets
  CHECK_EQ(columns_per_device.begin()->second.join_columns.size(),
           columns_per_device.begin()->second.join_buckets.size());
  if (effective_memory_level_ == Data_Namespace::MemoryLevel::CPU_LEVEL) {
    const auto composite_key_info =
        HashJoin::getCompositeKeyInfo(inner_outer_pairs_, executor_);
    int thread_count = cpu_threads();
    std::vector<uint8_t> hll_buffer_all_cpus(thread_count * padded_size_bytes);
    auto hll_result = &hll_buffer_all_cpus[0];

    std::vector<int32_t> num_keys_for_row;
    num_keys_for_row.resize(columns_per_device.begin()->second.join_columns[0].num_elems);

    approximate_distinct_tuples_range(
        hll_result,
        num_keys_for_row,
        count_distinct_desc.bitmap_sz_bits,
        padded_size_bytes,
        columns_per_device.begin()->second.join_columns,
        columns_per_device.begin()->second.join_column_types,
        columns_per_device.begin()->second.join_buckets,
        isInnerColCompressed(),
        thread_count);

    for (int i = 1; i < thread_count; ++i) {
      hll_unify(hll_result,
                hll_result + i * padded_size_bytes,
                size_t(1) << count_distinct_desc.bitmap_sz_bits);
    }
    return std::make_pair(hll_size(hll_result, count_distinct_desc.bitmap_sz_bits),
                          num_keys_for_row.size() > 0 ? num_keys_for_row.back() : 0);
  }
#ifdef HAVE_CUDA
  std::unordered_map<int, std::vector<uint8_t>> host_hll_buffers;
  std::unordered_map<int, size_t> emitted_keys_count_device_threads;
  for (auto device_id : device_ids_) {
    std::vector<uint8_t> host_hll_buffer;
    host_hll_buffer.resize(count_distinct_desc.bitmapPaddedSizeBytes());
    host_hll_buffers.emplace(device_id, std::move(host_hll_buffer));
    emitted_keys_count_device_threads.emplace(device_id, 0u);
  }
  std::vector<std::future<void>> approximate_distinct_device_threads;
  for (auto device_id : device_ids_) {
    approximate_distinct_device_threads.emplace_back(std::async(
        std::launch::async,
        [device_id,
         &columns_per_device,
         &count_distinct_desc,
         &host_hll_buffers,
         &emitted_keys_count_device_threads,
         this] {
          auto device_allocator = executor_->getCudaAllocator(device_id);
          CHECK(device_allocator);
          auto device_hll_buffer =
              device_allocator->alloc(count_distinct_desc.bitmapPaddedSizeBytes());
          device_allocator->zeroDeviceMem(device_hll_buffer,
                                          count_distinct_desc.bitmapPaddedSizeBytes());
          const auto& columns_for_device = columns_per_device[device_id];
          auto join_columns_gpu =
              transfer_vector_of_flat_objects_to_gpu(columns_for_device.join_columns,
                                                     *device_allocator,
                                                     "Range hash join input column(s)");

          CHECK_GT(columns_for_device.join_buckets.size(), 0u);
          const auto& bucket_sizes_for_dimension =
              columns_for_device.join_buckets[0].inverse_bucket_sizes_for_dimension;
          auto bucket_sizes_gpu =
              device_allocator->alloc(bucket_sizes_for_dimension.size() * sizeof(double));
          device_allocator->copyToDevice(
              bucket_sizes_gpu,
              bucket_sizes_for_dimension.data(),
              bucket_sizes_for_dimension.size() * sizeof(double),
              "Range join hashtable bucket sizes");
          const size_t row_counts_buffer_sz =
              columns_per_device.begin()->second.join_columns[0].num_elems *
              sizeof(int32_t);
          auto row_counts_buffer = device_allocator->alloc(row_counts_buffer_sz);
          device_allocator->zeroDeviceMem(row_counts_buffer, row_counts_buffer_sz);
          const auto key_handler =
              RangeKeyHandler(isInnerColCompressed(),
                              bucket_sizes_for_dimension.size(),
                              join_columns_gpu,
                              reinterpret_cast<double*>(bucket_sizes_gpu));
          const auto key_handler_gpu = transfer_flat_object_to_gpu(
              key_handler, *device_allocator, "Range hash join key handler");
          approximate_distinct_tuples_on_device_range(
              reinterpret_cast<uint8_t*>(device_hll_buffer),
              count_distinct_desc.bitmap_sz_bits,
              reinterpret_cast<int32_t*>(row_counts_buffer),
              key_handler_gpu,
              columns_for_device.join_columns[0].num_elems,
              executor_->blockSize(),
              executor_->gridSize(),
              executor_->getCudaStream(device_id));

          auto& host_emitted_keys_count = emitted_keys_count_device_threads[device_id];
          device_allocator->copyFromDevice(
              &host_emitted_keys_count,
              row_counts_buffer +
                  (columns_per_device.begin()->second.join_columns[0].num_elems - 1) *
                      sizeof(int32_t),
              sizeof(int32_t),
              "Range join hashtable emitted key count");

          auto& host_hll_buffer = host_hll_buffers[device_id];
          device_allocator->copyFromDevice(&host_hll_buffer[0],
                                           device_hll_buffer,
                                           count_distinct_desc.bitmapPaddedSizeBytes(),
                                           "Range join hashtable hyperloglog buffer");
        }));
  }
  for (auto& child : approximate_distinct_device_threads) {
    child.get();
  }
  CHECK_EQ(Data_Namespace::MemoryLevel::GPU_LEVEL, effective_memory_level_);
  auto it = host_hll_buffers.begin();
  auto& result_hll_buffer = it->second;
  it++;
  auto hll_result = reinterpret_cast<int32_t*>(&result_hll_buffer[0]);
  for (; it != host_hll_buffers.end(); it++) {
    auto& host_hll_buffer = it->second;
    hll_unify(hll_result,
              reinterpret_cast<int32_t*>(&host_hll_buffer[0]),
              size_t(1) << count_distinct_desc.bitmap_sz_bits);
  }
  size_t emitted_keys_count = 0;
  for (auto const& kv : emitted_keys_count_device_threads) {
    emitted_keys_count += kv.second;
  }
  return std::make_pair(hll_size(hll_result, count_distinct_desc.bitmap_sz_bits),
                        emitted_keys_count);
#else
  UNREACHABLE();
  return {0, 0};
#endif  // HAVE_CUDA
}

#define LL_CONTEXT executor_->cgen_state_->context_
#define LL_BUILDER executor_->cgen_state_->ir_builder_
#define LL_INT(v) executor_->cgen_state_->llInt(v)
#define LL_FP(v) executor_->cgen_state_->llFp(v)
#define ROW_FUNC executor_->cgen_state_->row_func_

llvm::Value* RangeJoinHashTable::codegenKey(const CompilationOptions& co,
                                            llvm::Value* offset_ptr) {
  const auto key_component_width = getKeyComponentWidth();
  CHECK(key_component_width == 4 || key_component_width == 8);
  const auto key_size_lv = LL_INT(getKeyComponentCount() * key_component_width);
  llvm::Value* key_buff_lv{nullptr};
  switch (key_component_width) {
    case 4:
      key_buff_lv =
          LL_BUILDER.CreateAlloca(llvm::Type::getInt32Ty(LL_CONTEXT), key_size_lv);
      break;
    case 8:
      key_buff_lv =
          LL_BUILDER.CreateAlloca(llvm::Type::getInt64Ty(LL_CONTEXT), key_size_lv);
      break;
    default:
      CHECK(false);
  }

  const auto& inner_outer_pair = inner_outer_pairs_[0];
  const auto outer_col = inner_outer_pair.second;
  const auto outer_col_ti = outer_col->get_type_info();

  if (outer_col_ti.is_geometry()) {
    CodeGenerator code_generator(executor_);
    // TODO(adb): for points we will use the coords array, but for other
    // geometries we will need to use the bounding box. For now only support
    // points.
    CHECK_EQ(outer_col_ti.get_type(), kPOINT);
    CHECK_EQ(inverse_bucket_sizes_for_dimension_.size(), static_cast<size_t>(2));

    llvm::Value* arr_ptr{nullptr};
    // prepare point column (arr) ptr to generate code for hash table key
    if (auto outer_col_var = dynamic_cast<const Analyzer::ColumnVar*>(outer_col)) {
      const auto col_lvs = code_generator.codegen(outer_col, true, co);
      CHECK_EQ(col_lvs.size(), size_t(1));
      auto column_key = outer_col_var->getColumnKey();
      if (column_key.table_id < 0) {
        // todo: relax this
        throw QueryNotSupported(
            "Geospatial columns not yet supported in this temporary table context.");
      }
      column_key.column_id = column_key.column_id + 1;
      const auto coords_cd = Catalog_Namespace::get_metadata_for_column(column_key);
      CHECK(coords_cd);
      const auto coords_ti = coords_cd->columnType;

      const auto array_buff_ptr = executor_->cgen_state_->emitExternalCall(
          "array_buff",
          llvm::Type::getInt8PtrTy(executor_->cgen_state_->context_),
          {col_lvs.front(), code_generator.posArg(outer_col)});
      CHECK(array_buff_ptr);
      CHECK(coords_ti.get_elem_type().get_type() == kTINYINT)
          << "Only TINYINT coordinates columns are supported in bounding box "
             "intersection.";
      arr_ptr =
          code_generator.castArrayPointer(array_buff_ptr, coords_ti.get_elem_type());
    } else if (auto geo_expr_outer_col =
                   dynamic_cast<const Analyzer::GeoOperator*>(outer_col)) {
      const auto geo_expr_name = geo_expr_outer_col->getName();
      if (func_resolve(geo_expr_name, "ST_Point"sv, "ST_Transform"sv, "ST_Centroid"sv)) {
        // note that ST_SetSRID changes type info of the column, and is handled by
        // translation phase, so when we use ST_SETSRID(ST_POINT(x, y), 4326)
        // as a join column expression, we recognize it as ST_POINT (with SRID as 4326)
        const auto col_lvs = code_generator.codegen(outer_col, true, co);
        // listed functions keep point coordinates in the local variable (let say S)
        // which is corresponding to the pointer that col_lvs[0] holds
        // thus, all we need is to retrieve necessary coordinate from the S by varying
        // its offset (i.e., i == 0 means x coordinate)
        arr_ptr = LL_BUILDER.CreatePointerCast(
            col_lvs[0], llvm::Type::getInt8PtrTy(executor_->cgen_state_->context_));
      } else {
        throw std::runtime_error(
            "RHS key of the range join operator has a geospatial function which is not "
            "supported yet: " +
            geo_expr_name);
      }
    } else {
      throw std::runtime_error("Range join operator has an invalid rhs key: " +
                               outer_col->toString());
    }

    // load and unpack offsets
    const auto offset =
        LL_BUILDER.CreateLoad(offset_ptr->getType()->getPointerElementType(),
                              offset_ptr,
                              "packed_bucket_offset");
    const auto x_offset =
        LL_BUILDER.CreateTrunc(offset, llvm::Type::getInt32Ty(LL_CONTEXT));

    const auto y_offset_shifted =
        LL_BUILDER.CreateLShr(offset, LL_INT(static_cast<int64_t>(32)));
    const auto y_offset =
        LL_BUILDER.CreateTrunc(y_offset_shifted, llvm::Type::getInt32Ty(LL_CONTEXT));

    const auto x_bucket_offset =
        LL_BUILDER.CreateSExt(x_offset, llvm::Type::getInt64Ty(LL_CONTEXT));
    const auto y_bucket_offset =
        LL_BUILDER.CreateSExt(y_offset, llvm::Type::getInt64Ty(LL_CONTEXT));

    for (size_t i = 0; i < 2; i++) {
      const auto key_comp_dest_lv = LL_BUILDER.CreateGEP(
          key_buff_lv->getType()->getScalarType()->getPointerElementType(),
          key_buff_lv,
          LL_INT(i));

      const auto funcName = isProbeCompressed() ? "get_bucket_key_for_range_compressed"
                                                : "get_bucket_key_for_range_double";

      // Note that get_bucket_key_for_range_compressed will need to be
      // specialized for future compression schemes
      auto bucket_key = executor_->cgen_state_->emitExternalCall(
          funcName,
          get_int_type(64, LL_CONTEXT),
          {arr_ptr, LL_INT(i), LL_FP(inverse_bucket_sizes_for_dimension_[i])});

      auto bucket_key_shifted = i == 0
                                    ? LL_BUILDER.CreateAdd(x_bucket_offset, bucket_key)
                                    : LL_BUILDER.CreateAdd(y_bucket_offset, bucket_key);

      const auto col_lv = LL_BUILDER.CreateSExt(
          bucket_key_shifted, get_int_type(key_component_width * 8, LL_CONTEXT));
      LL_BUILDER.CreateStore(col_lv, key_comp_dest_lv);
    }
  } else {
    LOG(FATAL) << "Range join key currently only supported for geospatial types.";
  }
  return key_buff_lv;
}

HashJoinMatchingSet RangeJoinHashTable::codegenMatchingSetWithOffset(
    const CompilationOptions& co,
    const size_t index,
    llvm::Value* range_offset) {
  const auto key_component_width = getKeyComponentWidth();
  CHECK(key_component_width == 4 || key_component_width == 8);

  auto key_buff_lv = codegenKey(co, range_offset);
  CHECK(getHashType() == HashType::OneToMany);

  auto hash_ptr = codegenHashTableLoad(index, executor_);
  const auto composite_dict_ptr_type =
      llvm::Type::getIntNPtrTy(LL_CONTEXT, key_component_width * 8);

  const auto composite_key_dict =
      hash_ptr->getType()->isPointerTy()
          ? LL_BUILDER.CreatePointerCast(hash_ptr, composite_dict_ptr_type)
          : LL_BUILDER.CreateIntToPtr(hash_ptr, composite_dict_ptr_type);

  const auto key_component_count = getKeyComponentCount();

  const auto funcName =
      "get_composite_key_index_" + std::to_string(key_component_width * 8);

  const auto key = executor_->cgen_state_->emitExternalCall(funcName,
                                                            get_int_type(64, LL_CONTEXT),
                                                            {key_buff_lv,
                                                             LL_INT(key_component_count),
                                                             composite_key_dict,
                                                             LL_INT(getEntryCount())});

  auto one_to_many_ptr = hash_ptr;
  if (one_to_many_ptr->getType()->isPointerTy()) {
    one_to_many_ptr =
        LL_BUILDER.CreatePtrToInt(hash_ptr, llvm::Type::getInt64Ty(LL_CONTEXT));
  } else {
    CHECK(one_to_many_ptr->getType()->isIntegerTy(64));
  }
  const auto composite_key_dict_size = offsetBufferOff();
  one_to_many_ptr =
      LL_BUILDER.CreateAdd(one_to_many_ptr, LL_INT(composite_key_dict_size));

  return HashJoin::codegenMatchingSet(
      /* hash_join_idx_args_in */ {one_to_many_ptr,
                                   key,
                                   LL_INT(int64_t(0)),
                                   LL_INT(getEntryCount() - 1)},
      /* is_sharded            */ false,
      /* is_nullable           */ false,
      /* is_bw_eq              */ false,
      /* sub_buff_size         */ getComponentBufferSize(),
      /* executor              */ executor_);
}
