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

#include "HashJoinRuntime.cpp"

#include <cuda.h>
#include <thrust/device_ptr.h>
#include <thrust/scan.h>

#define checkCudaErrors(err) CHECK_EQ(err, cudaSuccess)

template <typename F, typename... ARGS>
void cuda_kernel_launch_wrapper(F func, CUstream cuda_stream, ARGS&&... args) {
  int grid_size = -1;
  int block_size = -1;
  checkCudaErrors(cudaOccupancyMaxPotentialBlockSize(&grid_size, &block_size, func));
  func<<<grid_size, block_size, 0, cuda_stream>>>(std::forward<ARGS>(args)...);
  checkCudaErrors(cudaStreamSynchronize(cuda_stream));
}

__global__ void fill_hash_join_buff_wrapper(
    OneToOnePerfectJoinHashTableFillFuncArgs const args) {
  auto fill_hash_join_buff_func = args.type_info.uses_bw_eq
                                      ? SUFFIX(fill_hash_join_buff_bitwise_eq)
                                      : SUFFIX(fill_hash_join_buff);
  int partial_err = fill_hash_join_buff_func(args, -1, -1);
  atomicCAS(args.dev_err_buff, 0, partial_err);
}

__global__ void fill_hash_join_buff_bucketized_wrapper(
    OneToOnePerfectJoinHashTableFillFuncArgs const args) {
  int partial_err = SUFFIX(fill_hash_join_buff_bucketized)(args, -1, -1);
  atomicCAS(args.dev_err_buff, 0, partial_err);
}

void fill_hash_join_buff_on_device_bucketized(
    CUstream cuda_stream,
    OneToOnePerfectJoinHashTableFillFuncArgs const args) {
  cuda_kernel_launch_wrapper(fill_hash_join_buff_bucketized_wrapper, cuda_stream, args);
}

void fill_hash_join_buff_on_device(CUstream cuda_stream,
                                   OneToOnePerfectJoinHashTableFillFuncArgs const args) {
  cuda_kernel_launch_wrapper(fill_hash_join_buff_wrapper, cuda_stream, args);
}

__global__ void fill_hash_join_buff_wrapper_sharded_bucketized(
    OneToOnePerfectJoinHashTableFillFuncArgs const args,
    ShardInfo const shard_info) {
  int partial_err =
      SUFFIX(fill_hash_join_buff_sharded_bucketized)(args.buff,
                                                     args.invalid_slot_val,
                                                     args.for_semi_join,
                                                     args.join_column,
                                                     args.type_info,
                                                     shard_info,
                                                     NULL,
                                                     NULL,
                                                     -1,
                                                     -1,
                                                     args.bucket_normalization);
  atomicCAS(args.dev_err_buff, 0, partial_err);
}

__global__ void fill_hash_join_buff_wrapper_sharded(
    OneToOnePerfectJoinHashTableFillFuncArgs const args,
    ShardInfo const shard_info) {
  int partial_err = SUFFIX(fill_hash_join_buff_sharded)(args.buff,
                                                        args.invalid_slot_val,
                                                        args.for_semi_join,
                                                        args.join_column,
                                                        args.type_info,
                                                        shard_info,
                                                        NULL,
                                                        NULL,
                                                        -1,
                                                        -1);
  atomicCAS(args.dev_err_buff, 0, partial_err);
}

void fill_hash_join_buff_on_device_sharded_bucketized(
    CUstream cuda_stream,
    OneToOnePerfectJoinHashTableFillFuncArgs const args,
    ShardInfo const shard_info) {
  cuda_kernel_launch_wrapper(
      fill_hash_join_buff_wrapper_sharded_bucketized, cuda_stream, args, shard_info);
}

void fill_hash_join_buff_on_device_sharded(
    CUstream cuda_stream,
    OneToOnePerfectJoinHashTableFillFuncArgs const args,
    ShardInfo const shard_info) {
  cuda_kernel_launch_wrapper(
      fill_hash_join_buff_wrapper_sharded, cuda_stream, args, shard_info);
}

__global__ void init_hash_join_buff_wrapper(int32_t* buff,
                                            const int64_t hash_entry_count,
                                            const int32_t invalid_slot_val) {
  SUFFIX(init_hash_join_buff)(buff, hash_entry_count, invalid_slot_val, -1, -1);
}

void init_hash_join_buff_on_device(int32_t* buff,
                                   const int64_t hash_entry_count,
                                   const int32_t invalid_slot_val,
                                   CUstream cuda_stream) {
  cuda_kernel_launch_wrapper(
      init_hash_join_buff_wrapper, cuda_stream, buff, hash_entry_count, invalid_slot_val);
}

#define VALID_POS_FLAG 0

__global__ void set_valid_pos_flag(int32_t* pos_buff,
                                   const int32_t* count_buff,
                                   const int64_t entry_count) {
  const int32_t start = threadIdx.x + blockDim.x * blockIdx.x;
  const int32_t step = blockDim.x * gridDim.x;
  for (int64_t i = start; i < entry_count; i += step) {
    if (count_buff[i]) {
      pos_buff[i] = VALID_POS_FLAG;
    }
  }
}

__global__ void set_valid_pos(int32_t* pos_buff,
                              int32_t* count_buff,
                              const int64_t entry_count) {
  const int32_t start = threadIdx.x + blockDim.x * blockIdx.x;
  const int32_t step = blockDim.x * gridDim.x;
  for (int64_t i = start; i < entry_count; i += step) {
    if (VALID_POS_FLAG == pos_buff[i]) {
      pos_buff[i] = !i ? 0 : count_buff[i - 1];
    }
  }
}

template <typename COUNT_MATCHES_FUNCTOR, typename FILL_ROW_IDS_FUNCTOR>
void fill_one_to_many_hash_table_on_device_impl(int32_t* buff,
                                                const int64_t hash_entry_count,
                                                const JoinColumn& join_column,
                                                const JoinColumnTypeInfo& type_info,
                                                CUstream cuda_stream,
                                                COUNT_MATCHES_FUNCTOR count_matches_func,
                                                FILL_ROW_IDS_FUNCTOR fill_row_ids_func) {
  int32_t* pos_buff = buff;
  int32_t* count_buff = buff + hash_entry_count;
  auto qe_cuda_stream = reinterpret_cast<CUstream>(cuda_stream);
  checkCudaErrors(
      cudaMemsetAsync(count_buff, 0, hash_entry_count * sizeof(int32_t), qe_cuda_stream));
  checkCudaErrors(cudaStreamSynchronize(qe_cuda_stream));
  count_matches_func();

  cuda_kernel_launch_wrapper(
      set_valid_pos_flag, cuda_stream, pos_buff, count_buff, hash_entry_count);

  auto count_buff_dev_ptr = thrust::device_pointer_cast(count_buff);
  thrust::inclusive_scan(
      count_buff_dev_ptr, count_buff_dev_ptr + hash_entry_count, count_buff_dev_ptr);

  cuda_kernel_launch_wrapper(
      set_valid_pos, cuda_stream, pos_buff, count_buff, hash_entry_count);
  checkCudaErrors(
      cudaMemsetAsync(count_buff, 0, hash_entry_count * sizeof(int32_t), qe_cuda_stream));
  checkCudaErrors(cudaStreamSynchronize(qe_cuda_stream));
  fill_row_ids_func();
}

void fill_one_to_many_hash_table_on_device(
    CUstream cuda_stream,
    OneToManyPerfectJoinHashTableFillFuncArgs const args) {
  auto buff = args.buff;
  auto hash_entry_count = args.hash_entry_info.bucketized_hash_entry_count;
  auto count_matches_func = [count_buff = buff + hash_entry_count, &args, &cuda_stream] {
    cuda_kernel_launch_wrapper(
        SUFFIX(count_matches), cuda_stream, count_buff, args.join_column, args.type_info);
  };
  auto fill_row_ids_func = [buff, hash_entry_count, &args, &cuda_stream] {
    cuda_kernel_launch_wrapper(SUFFIX(fill_row_ids),
                               cuda_stream,
                               buff,
                               hash_entry_count,
                               args.join_column,
                               args.type_info,
                               args.for_window_framing);
  };
  fill_one_to_many_hash_table_on_device_impl(buff,
                                             hash_entry_count,
                                             args.join_column,
                                             args.type_info,
                                             cuda_stream,
                                             count_matches_func,
                                             fill_row_ids_func);
}

void fill_one_to_many_hash_table_on_device_bucketized(
    CUstream cuda_stream,
    OneToManyPerfectJoinHashTableFillFuncArgs const args) {
  auto hash_entry_count = args.hash_entry_info.getNormalizedHashEntryCount();
  auto const buff = args.buff;
  auto count_matches_func = [count_buff = buff + hash_entry_count, &args, &cuda_stream] {
    cuda_kernel_launch_wrapper(SUFFIX(count_matches_bucketized),
                               cuda_stream,
                               count_buff,
                               args.join_column,
                               args.type_info,
                               args.bucket_normalization);
  };
  auto fill_row_ids_func = [buff, hash_entry_count, &args, &cuda_stream] {
    cuda_kernel_launch_wrapper(SUFFIX(fill_row_ids_bucketized),
                               cuda_stream,
                               buff,
                               hash_entry_count,
                               args.join_column,
                               args.type_info,
                               args.bucket_normalization);
  };
  fill_one_to_many_hash_table_on_device_impl(buff,
                                             hash_entry_count,
                                             args.join_column,
                                             args.type_info,
                                             cuda_stream,
                                             count_matches_func,
                                             fill_row_ids_func);
}

void fill_one_to_many_hash_table_on_device_sharded(
    CUstream cuda_stream,
    OneToManyPerfectJoinHashTableFillFuncArgs const args,
    ShardInfo const shard_info) {
  auto hash_entry_count = args.hash_entry_info.bucketized_hash_entry_count;
  int32_t* pos_buff = args.buff;
  int32_t* count_buff = args.buff + hash_entry_count;
  checkCudaErrors(
      cudaMemsetAsync(count_buff, 0, hash_entry_count * sizeof(int32_t), cuda_stream));
  checkCudaErrors(cudaStreamSynchronize(cuda_stream));
  cuda_kernel_launch_wrapper(SUFFIX(count_matches_sharded),
                             cuda_stream,
                             count_buff,
                             args.join_column,
                             args.type_info,
                             shard_info);

  cuda_kernel_launch_wrapper(
      set_valid_pos_flag, cuda_stream, pos_buff, count_buff, hash_entry_count);

  auto count_buff_dev_ptr = thrust::device_pointer_cast(count_buff);
  thrust::inclusive_scan(thrust::cuda::par.on(cuda_stream),
                         count_buff_dev_ptr,
                         count_buff_dev_ptr + hash_entry_count,
                         count_buff_dev_ptr);
  cuda_kernel_launch_wrapper(
      set_valid_pos, cuda_stream, pos_buff, count_buff, hash_entry_count);
  checkCudaErrors(
      cudaMemsetAsync(count_buff, 0, hash_entry_count * sizeof(int32_t), cuda_stream));
  checkCudaErrors(cudaStreamSynchronize(cuda_stream));
  cuda_kernel_launch_wrapper(SUFFIX(fill_row_ids_sharded),
                             cuda_stream,
                             args.buff,
                             hash_entry_count,
                             args.join_column,
                             args.type_info,
                             shard_info);
}

template <typename T, typename KEY_HANDLER>
void fill_one_to_many_baseline_hash_table_on_device(int32_t* buff,
                                                    const T* composite_key_dict,
                                                    const int64_t hash_entry_count,
                                                    const KEY_HANDLER* key_handler,
                                                    const size_t num_elems,
                                                    const bool for_window_framing,
                                                    CUstream cuda_stream) {
  auto pos_buff = buff;
  auto count_buff = buff + hash_entry_count;
  checkCudaErrors(
      cudaMemsetAsync(count_buff, 0, hash_entry_count * sizeof(int32_t), cuda_stream));
  checkCudaErrors(cudaStreamSynchronize(cuda_stream));
  cuda_kernel_launch_wrapper(count_matches_baseline_gpu<T, KEY_HANDLER>,
                             cuda_stream,
                             count_buff,
                             composite_key_dict,
                             hash_entry_count,
                             key_handler,
                             num_elems);

  cuda_kernel_launch_wrapper(
      set_valid_pos_flag, cuda_stream, pos_buff, count_buff, hash_entry_count);

  auto count_buff_dev_ptr = thrust::device_pointer_cast(count_buff);
  thrust::inclusive_scan(thrust::cuda::par.on(cuda_stream),
                         count_buff_dev_ptr,
                         count_buff_dev_ptr + hash_entry_count,
                         count_buff_dev_ptr);
  cuda_kernel_launch_wrapper(
      set_valid_pos, cuda_stream, pos_buff, count_buff, hash_entry_count);
  checkCudaErrors(
      cudaMemsetAsync(count_buff, 0, hash_entry_count * sizeof(int32_t), cuda_stream));
  checkCudaErrors(cudaStreamSynchronize(cuda_stream));

  cuda_kernel_launch_wrapper(fill_row_ids_baseline_gpu<T, KEY_HANDLER>,
                             cuda_stream,
                             buff,
                             composite_key_dict,
                             hash_entry_count,
                             key_handler,
                             num_elems,
                             for_window_framing);
}

template <typename T>
__global__ void init_baseline_hash_join_buff_wrapper(int8_t* hash_join_buff,
                                                     const int64_t entry_count,
                                                     const size_t key_component_count,
                                                     const bool with_val_slot,
                                                     const int32_t invalid_slot_val) {
  SUFFIX(init_baseline_hash_join_buff)<T>(hash_join_buff,
                                          entry_count,
                                          key_component_count,
                                          with_val_slot,
                                          invalid_slot_val,
                                          -1,
                                          -1);
}

void init_baseline_hash_join_buff_on_device_32(int8_t* hash_join_buff,
                                               const int64_t entry_count,
                                               const size_t key_component_count,
                                               const bool with_val_slot,
                                               const int32_t invalid_slot_val,
                                               CUstream cuda_stream) {
  cuda_kernel_launch_wrapper(init_baseline_hash_join_buff_wrapper<int32_t>,
                             cuda_stream,
                             hash_join_buff,
                             entry_count,
                             key_component_count,
                             with_val_slot,
                             invalid_slot_val);
}

void init_baseline_hash_join_buff_on_device_64(int8_t* hash_join_buff,
                                               const int64_t entry_count,
                                               const size_t key_component_count,
                                               const bool with_val_slot,
                                               const int32_t invalid_slot_val,
                                               CUstream cuda_stream) {
  cuda_kernel_launch_wrapper(init_baseline_hash_join_buff_wrapper<int64_t>,
                             cuda_stream,
                             hash_join_buff,
                             entry_count,
                             key_component_count,
                             with_val_slot,
                             invalid_slot_val);
}

template <typename T, typename KEY_HANDLER>
__global__ void fill_baseline_hash_join_buff_wrapper(int8_t* hash_buff,
                                                     const int64_t entry_count,
                                                     const int32_t invalid_slot_val,
                                                     const bool for_semi_join,
                                                     const size_t key_component_count,
                                                     const bool with_val_slot,
                                                     int* err,
                                                     const KEY_HANDLER* key_handler,
                                                     const int64_t num_elems) {
  int partial_err = SUFFIX(fill_baseline_hash_join_buff)<T>(hash_buff,
                                                            entry_count,
                                                            invalid_slot_val,
                                                            for_semi_join,
                                                            key_component_count,
                                                            with_val_slot,
                                                            key_handler,
                                                            num_elems,
                                                            -1,
                                                            -1);
  atomicCAS(err, 0, partial_err);
}

void fill_baseline_hash_join_buff_on_device_32(int8_t* hash_buff,
                                               const int64_t entry_count,
                                               const int32_t invalid_slot_val,
                                               const bool for_semi_join,
                                               const size_t key_component_count,
                                               const bool with_val_slot,
                                               int* dev_err_buff,
                                               const GenericKeyHandler* key_handler,
                                               const int64_t num_elems,
                                               CUstream cuda_stream) {
  cuda_kernel_launch_wrapper(
      fill_baseline_hash_join_buff_wrapper<int32_t, GenericKeyHandler>,
      cuda_stream,
      hash_buff,
      entry_count,
      invalid_slot_val,
      for_semi_join,
      key_component_count,
      with_val_slot,
      dev_err_buff,
      key_handler,
      num_elems);
}

void fill_baseline_hash_join_buff_on_device_64(int8_t* hash_buff,
                                               const int64_t entry_count,
                                               const int32_t invalid_slot_val,
                                               const bool for_semi_join,
                                               const size_t key_component_count,
                                               const bool with_val_slot,
                                               int* dev_err_buff,
                                               const GenericKeyHandler* key_handler,
                                               const int64_t num_elems,
                                               CUstream cuda_stream) {
  cuda_kernel_launch_wrapper(
      fill_baseline_hash_join_buff_wrapper<unsigned long long, GenericKeyHandler>,
      cuda_stream,
      hash_buff,
      entry_count,
      invalid_slot_val,
      for_semi_join,
      key_component_count,
      with_val_slot,
      dev_err_buff,
      key_handler,
      num_elems);
}

void bbox_intersect_fill_baseline_hash_join_buff_on_device_64(
    int8_t* hash_buff,
    const int64_t entry_count,
    const int32_t invalid_slot_val,
    const size_t key_component_count,
    const bool with_val_slot,
    int* dev_err_buff,
    const BoundingBoxIntersectKeyHandler* key_handler,
    const int64_t num_elems,
    CUstream cuda_stream) {
  cuda_kernel_launch_wrapper(
      fill_baseline_hash_join_buff_wrapper<unsigned long long,
                                           BoundingBoxIntersectKeyHandler>,
      cuda_stream,
      hash_buff,
      entry_count,
      invalid_slot_val,
      false,
      key_component_count,
      with_val_slot,
      dev_err_buff,
      key_handler,
      num_elems);
}

void range_fill_baseline_hash_join_buff_on_device_64(int8_t* hash_buff,
                                                     const int64_t entry_count,
                                                     const int32_t invalid_slot_val,
                                                     const size_t key_component_count,
                                                     const bool with_val_slot,
                                                     int* dev_err_buff,
                                                     const RangeKeyHandler* key_handler,
                                                     const size_t num_elems,
                                                     CUstream cuda_stream) {
  cuda_kernel_launch_wrapper(
      fill_baseline_hash_join_buff_wrapper<unsigned long long, RangeKeyHandler>,
      cuda_stream,
      hash_buff,
      entry_count,
      invalid_slot_val,
      false,
      key_component_count,
      with_val_slot,
      dev_err_buff,
      key_handler,
      num_elems);
}

void fill_one_to_many_baseline_hash_table_on_device_32(
    int32_t* buff,
    const int32_t* composite_key_dict,
    const int64_t hash_entry_count,
    const size_t key_component_count,
    const GenericKeyHandler* key_handler,
    const int64_t num_elems,
    const bool for_window_framing,
    CUstream cuda_stream) {
  fill_one_to_many_baseline_hash_table_on_device<int32_t>(buff,
                                                          composite_key_dict,
                                                          hash_entry_count,
                                                          key_handler,
                                                          num_elems,
                                                          for_window_framing,
                                                          cuda_stream);
}

void fill_one_to_many_baseline_hash_table_on_device_64(
    int32_t* buff,
    const int64_t* composite_key_dict,
    const int64_t hash_entry_count,
    const GenericKeyHandler* key_handler,
    const int64_t num_elems,
    const bool for_window_framing,
    CUstream cuda_stream) {
  fill_one_to_many_baseline_hash_table_on_device<int64_t>(buff,
                                                          composite_key_dict,
                                                          hash_entry_count,
                                                          key_handler,
                                                          num_elems,
                                                          for_window_framing,
                                                          cuda_stream);
}

void bbox_intersect_fill_one_to_many_baseline_hash_table_on_device_64(
    int32_t* buff,
    const int64_t* composite_key_dict,
    const int64_t hash_entry_count,
    const BoundingBoxIntersectKeyHandler* key_handler,
    const int64_t num_elems,
    CUstream cuda_stream) {
  fill_one_to_many_baseline_hash_table_on_device<int64_t>(buff,
                                                          composite_key_dict,
                                                          hash_entry_count,
                                                          key_handler,
                                                          num_elems,
                                                          false,
                                                          cuda_stream);
}

void range_fill_one_to_many_baseline_hash_table_on_device_64(
    int32_t* buff,
    const int64_t* composite_key_dict,
    const size_t hash_entry_count,
    const RangeKeyHandler* key_handler,
    const size_t num_elems,
    CUstream cuda_stream) {
  fill_one_to_many_baseline_hash_table_on_device<int64_t>(buff,
                                                          composite_key_dict,
                                                          hash_entry_count,
                                                          key_handler,
                                                          num_elems,
                                                          false,
                                                          cuda_stream);
}

void approximate_distinct_tuples_on_device_bbox_intersect(
    uint8_t* hll_buffer,
    const uint32_t b,
    int32_t* row_counts_buffer,
    const BoundingBoxIntersectKeyHandler* key_handler,
    const int64_t num_elems,
    CUstream cuda_stream) {
  cuda_kernel_launch_wrapper(
      approximate_distinct_tuples_impl_gpu<BoundingBoxIntersectKeyHandler>,
      cuda_stream,
      hll_buffer,
      row_counts_buffer,
      b,
      num_elems,
      key_handler);

  auto row_counts_buffer_ptr = thrust::device_pointer_cast(row_counts_buffer);
  auto qe_cuda_stream = reinterpret_cast<CUstream>(cuda_stream);
  thrust::inclusive_scan(thrust::cuda::par.on(qe_cuda_stream),
                         row_counts_buffer_ptr,
                         row_counts_buffer_ptr + num_elems,
                         row_counts_buffer_ptr);
}

void approximate_distinct_tuples_on_device_range(uint8_t* hll_buffer,
                                                 const uint32_t b,
                                                 int32_t* row_counts_buffer,
                                                 const RangeKeyHandler* key_handler,
                                                 const size_t num_elems,
                                                 const size_t block_size_x,
                                                 const size_t grid_size_x,
                                                 CUstream cuda_stream) {
  auto qe_cuda_stream = reinterpret_cast<CUstream>(cuda_stream);
  approximate_distinct_tuples_impl_gpu<<<grid_size_x, block_size_x, 0, qe_cuda_stream>>>(
      hll_buffer, row_counts_buffer, b, num_elems, key_handler);
  checkCudaErrors(cudaStreamSynchronize(qe_cuda_stream));

  auto row_counts_buffer_ptr = thrust::device_pointer_cast(row_counts_buffer);
  thrust::inclusive_scan(thrust::cuda::par.on(qe_cuda_stream),
                         row_counts_buffer_ptr,
                         row_counts_buffer_ptr + num_elems,
                         row_counts_buffer_ptr);
}

void approximate_distinct_tuples_on_device(uint8_t* hll_buffer,
                                           const uint32_t b,
                                           const GenericKeyHandler* key_handler,
                                           const int64_t num_elems,
                                           CUstream cuda_stream) {
  cuda_kernel_launch_wrapper(approximate_distinct_tuples_impl_gpu<GenericKeyHandler>,
                             cuda_stream,
                             hll_buffer,
                             nullptr,
                             b,
                             num_elems,
                             key_handler);
}

void compute_bucket_sizes_on_device(double* bucket_sizes_buffer,
                                    const JoinColumn* join_column,
                                    const JoinColumnTypeInfo* type_info,
                                    const double* bucket_sz_threshold,
                                    CUstream cuda_stream) {
  cuda_kernel_launch_wrapper(compute_bucket_sizes_impl_gpu<2>,
                             cuda_stream,
                             bucket_sizes_buffer,
                             join_column,
                             type_info,
                             bucket_sz_threshold);
}
