/*
 * Copyright 2019 Saman Ashkiani
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
 * implied. See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <cassert>
#include <iostream>
#include <random>

#include "concurrent/device/build.cuh"
#include "slab_hash_global.cuh"

/*
 * This is the main class that will be shallowly copied into the device to be
 * used at runtime. This class does not own the allocated memories on the gpu
 * (i.e., d_table_)
 */
template <typename KeyT, typename ValueT>
class GpuSlabHashContext<KeyT, ValueT, SlabHashType::ConcurrentMap> {
 public:
  // fixed known parameters:
  static constexpr uint32_t PRIME_DIVISOR_ = 4294967291u;

  GpuSlabHashContext()
      : num_buckets_(0), hash_x_(0), hash_y_(0), d_table_(nullptr) {}

  __device__ __host__ __forceinline__ uint32_t
  computeBucket(const KeyT& key) const {
    return (((hash_x_ ^ key) + hash_y_) % PRIME_DIVISOR_) % num_buckets_;
  }

  __device__ __host__ __forceinline__ concurrent_slab<KeyT, ValueT>*
  getDeviceTablePointer() {
    return d_table_;
  }

  __device__ __host__ __forceinline__ AllocatorContextT& getAllocatorContext() {
    return dynamic_allocator_;
  }

  // this function should be operated in a warp-wide fashion
  // TODO: add required asserts to make sure this is true in tests/debugs
  __device__ __forceinline__ SlabAllocAddressT
  allocateSlab(const uint32_t& laneId) {
    return dynamic_allocator_.warpAllocate(laneId);
  }

  // a thread-wide function to free the slab that was just allocated
  __device__ __forceinline__ void freeSlab(const SlabAllocAddressT slab_ptr) {
    dynamic_allocator_.freeUntouched(slab_ptr);
  }

  __device__ __forceinline__ uint32_t* getPointerFromSlab(
      const SlabAddressT& slab_address,
      const uint32_t laneId) {
    return dynamic_allocator_.getPointerFromSlab(slab_address, laneId);
  }

  __device__ __forceinline__ uint32_t* getPointerFromBucket(
      const uint32_t bucket_id,
      const uint32_t laneId) {
    return reinterpret_cast<uint32_t*>(d_table_) + bucket_id * BASE_UNIT_SIZE +
           laneId;
  }

  __host__ void initParameters(const uint32_t num_buckets,
                               const uint32_t hash_x,
                               const uint32_t hash_y,
                               concurrent_slab<KeyT, ValueT>* d_table,
                               AllocatorContextT* allocator_ctx) {
    num_buckets_ = num_buckets;
    hash_x_ = hash_x;
    hash_y_ = hash_y;
    d_table_ = d_table;
    dynamic_allocator_ = *allocator_ctx;
  }

 private:
  uint32_t num_buckets_;
  uint32_t hash_x_;
  uint32_t hash_y_;
  concurrent_slab<KeyT, ValueT>* d_table_;
  // a copy of dynamic allocator's context to be used on the GPU
  AllocatorContextT dynamic_allocator_;
};

/*
 * This class owns the allocated memory for the hash table
 */
template <typename KeyT, typename ValueT, uint32_t DEVICE_IDX>
class GpuSlabHash<KeyT, ValueT, DEVICE_IDX, SlabHashType::ConcurrentMap> {
 private:
  // fixed known parameters:
  static constexpr uint32_t BLOCKSIZE_ = 128;
  static constexpr uint32_t WARP_WIDTH_ = 32;
  static constexpr uint32_t PRIME_DIVISOR_ = 4294967291u;

  struct hash_function {
    uint32_t x;
    uint32_t y;
  } hf_;

  // total number of buckets (slabs) for this hash table
  uint32_t num_buckets_;

  // a raw pointer to the initial allocated memory for all buckets
  concurrent_slab<KeyT, ValueT>* d_table_;

  // slab hash context, contains everything that a GPU application needs to be
  // able to use this data structure
  GpuSlabHashContext<KeyT, ValueT, SlabHashType::ConcurrentMap> gpu_context_;

  // const pointer to an allocator that all instances of slab hash are going to
  // use. The allocator itself is not owned by this class
  DynamicAllocatorT* dynamic_allocator_;

 public:
  GpuSlabHash(const uint32_t num_buckets,
              DynamicAllocatorT* dynamic_allocator,
              const time_t seed = 0)
      : num_buckets_(num_buckets),
        d_table_(nullptr),
        dynamic_allocator_(dynamic_allocator) {
    // a single slab on a ConcurrentMap should be 128 bytes
    assert(sizeof(concurrent_slab<KeyT, ValueT>) ==
           (WARP_WIDTH_ * sizeof(uint32_t)));

    int32_t devCount = 0;
    CHECK_CUDA_ERROR(cudaGetDeviceCount(&devCount));
    assert(DEVICE_IDX < devCount);

    CHECK_CUDA_ERROR(cudaSetDevice(DEVICE_IDX));

    // allocating initial buckets:
    CHECK_CUDA_ERROR(
        cudaMalloc((void**)&d_table_,
                   sizeof(concurrent_slab<KeyT, ValueT>) * num_buckets_));

    CHECK_CUDA_ERROR(cudaMemset(
        d_table_, 0xFF, sizeof(concurrent_slab<KeyT, ValueT>) * num_buckets_));

    // creating a random number generator:
    std::mt19937 rng(seed ? seed : time(0));
    hf_.x = rng() % PRIME_DIVISOR_;
    if (hf_.x < 1)
      hf_.x = 1;
    hf_.y = rng() % PRIME_DIVISOR_;

    // initializing the gpu_context_:
    gpu_context_.initParameters(num_buckets_, hf_.x, hf_.y, d_table_,
                                dynamic_allocator_->getContextPtr());
  }

  ~GpuSlabHash() {
    CHECK_CUDA_ERROR(cudaSetDevice(DEVICE_IDX));
    CHECK_CUDA_ERROR(cudaFree(d_table_));
  }

  // returns some debug information about the slab hash
  std::string to_string();

  void bulk_build(KeyT* d_key, ValueT* d_value, uint32_t num_keys) {
    uint32_t num_blocks = (num_keys + BLOCKSIZE_ - 1) / BLOCKSIZE_;
    // calling the kernel for bulk build:
    // build_table_kernel<KeyT, ValueT><<<num_blocks, BLOCKSIZE_>>>(
    //     d_key, d_value, num_keys, d_table_, num_buckets_, ctx_alloc_, hf_);
    CHECK_CUDA_ERROR(cudaSetDevice(DEVICE_IDX));
    build_table_kernel<KeyT, ValueT>
        <<<num_blocks, BLOCKSIZE_>>>(d_key, d_value, num_keys, gpu_context_);
  }
};

template <typename KeyT, typename ValueT, uint32_t DEVICE_IDX>
std::string GpuSlabHash<KeyT, ValueT, DEVICE_IDX, SlabHashType::ConcurrentMap>::
    to_string() {
  std::string result;
  result += " ==== GpuSlabHash: \n";
  result += "\t Running on device \t\t " + std::to_string(DEVICE_IDX) + "\n";
  result += "\t SlabHashType:     \t\t ConcurrentMap\n";
  result += "\t Number of buckets:\t\t " + std::to_string(num_buckets_) + "\n";
  result +=
      "\t d_table_ address: \t\t " +
      std::to_string(reinterpret_cast<uint64_t>(static_cast<void*>(d_table_))) +
      "\n";
  result += "\t hash function = \t\t (" + std::to_string(hf_.x) + ", " +
            std::to_string(hf_.y) + ")\n";
  return result;
}