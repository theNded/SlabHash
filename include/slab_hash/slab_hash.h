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

#include <thrust/pair.h>
#include <cassert>
#include <memory>

#include "memory_alloc.h"
#include "slab_alloc.h"

/**
 * Interface
 **/
class Slab {
public:
    ptr_t pair_ptrs[31];
    ptr_t next_slab_ptr;
};

template <typename _Key, typename _Value, typename _Hash>
class SlabHashContext;

template <typename _Key, typename _Value, typename _Hash>
class SlabHash {
public:
    SlabHash(const uint32_t max_bucket_count,
             const uint32_t max_keyvalue_count,
             uint32_t device_idx);

    ~SlabHash();

    double ComputeLoadFactor(int flag);

    void Insert(_Key* keys, _Value* values, uint32_t num_keys);
    void Search(_Key* keys, _Value* values, uint8_t* founds, uint32_t num_keys);
    void Remove(_Key* keys, uint32_t num_keys);

private:
    uint32_t num_buckets_;

    Slab* bucket_list_head_;

    SlabHashContext<_Key, _Value, _Hash> gpu_context_;

    std::shared_ptr<MemoryAlloc<thrust::pair<_Key, _Value>>> pair_allocator_;
    std::shared_ptr<SlabAlloc> slab_list_allocator_;

    uint32_t device_idx_;
};

/**
 * Implementation
 **/
template <typename _Key, typename _Value, typename _Hash>
class SlabHashContext {
public:
    SlabHashContext();
    __host__ void Setup(Slab* bucket_list_head,
                        const uint32_t num_buckets,
                        const SlabAllocContext& allocator_ctx,
                        const MemoryAllocContext<thrust::pair<_Key, _Value>>&
                                pair_allocator_ctx);

    /* Core SIMT operations */
    __device__ thrust::pair<iterator_t, bool> Insert(bool& lane_active,
                                                     const uint32_t lane_id,
                                                     const uint32_t bucket_id,
                                                     const _Key& key,
                                                     const _Value& value);

    __device__ thrust::pair<iterator_t, bool> Search(bool& lane_active,
                                                     const uint32_t lane_id,
                                                     const uint32_t bucket_id,
                                                     const _Key& key);

    __device__ bool Remove(bool& lane_active,
                           const uint32_t lane_id,
                           const uint32_t bucket_id,
                           const _Key& key);

    /* Hash function */
    __device__ __host__ uint32_t ComputeBucket(const _Key& key) const;

    __device__ __host__ SlabAllocContext& get_slab_alloc_ctx() {
        return slab_list_allocator_ctx_;
    }
    __device__ __host__ MemoryAllocContext<thrust::pair<_Key, _Value>>
    get_pair_alloc_ctx() {
        return pair_allocator_ctx_;
    }

    __device__ __forceinline__ ptr_t* get_unit_ptr_from_list_nodes(
            const ptr_t slab_ptr, const uint32_t lane_id) {
        return slab_list_allocator_ctx_.get_unit_ptr_from_slab(slab_ptr,
                                                               lane_id);
    }
    __device__ __forceinline__ ptr_t* get_unit_ptr_from_list_head(
            const uint32_t bucket_id, const uint32_t lane_id) {
        return reinterpret_cast<uint32_t*>(bucket_list_head_) +
               bucket_id * BASE_UNIT_SIZE + lane_id;
    }

private:
    __device__ __forceinline__ void WarpSyncKey(const _Key& key,
                                                const uint32_t lane_id,
                                                _Key& ret);
    __device__ __forceinline__ int32_t WarpFindKey(const _Key& src_key,
                                                   const uint32_t lane_id,
                                                   const uint32_t unit_data);
    __device__ __forceinline__ int32_t WarpFindEmpty(const uint32_t unit_data);

    __device__ __forceinline__ ptr_t AllocateSlab(const uint32_t lane_id);
    __device__ __forceinline__ void FreeSlab(const ptr_t slab_ptr);

private:
    uint32_t num_buckets_;
    _Hash hash_fn_;

    Slab* bucket_list_head_;
    SlabAllocContext slab_list_allocator_ctx_;
    MemoryAllocContext<thrust::pair<_Key, _Value>> pair_allocator_ctx_;
};

/**
 * Definitions
 **/
template <typename _Key, typename _Value, typename _Hash>
SlabHashContext<_Key, _Value, _Hash>::SlabHashContext()
    : num_buckets_(0), bucket_list_head_(nullptr) {
    static_assert(sizeof(Slab) == (WARP_WIDTH * sizeof(ptr_t)));
}

template <typename _Key, typename _Value, typename _Hash>
__host__ void SlabHashContext<_Key, _Value, _Hash>::Setup(
        Slab* bucket_list_head,
        const uint32_t num_buckets,
        const SlabAllocContext& allocator_ctx,
        const MemoryAllocContext<thrust::pair<_Key, _Value>>&
                pair_allocator_ctx) {
    bucket_list_head_ = bucket_list_head;

    num_buckets_ = num_buckets;
    slab_list_allocator_ctx_ = allocator_ctx;
    pair_allocator_ctx_ = pair_allocator_ctx;
}

template <typename _Key, typename _Value, typename _Hash>
__device__ __host__ __forceinline__ uint32_t
SlabHashContext<_Key, _Value, _Hash>::ComputeBucket(const _Key& key) const {
    return hash_fn_(key) % num_buckets_;
}

template <typename _Key, typename _Value, typename _Hash>
__device__ __forceinline__ void
SlabHashContext<_Key, _Value, _Hash>::WarpSyncKey(const _Key& key,
                                                  const uint32_t lane_id,
                                                  _Key& ret) {
    const int chunks = sizeof(_Key) / sizeof(int);
#pragma unroll 1
    for (size_t i = 0; i < chunks; ++i) {
        ((int*)(&ret))[i] = __shfl_sync(ACTIVE_LANES_MASK, ((int*)(&key))[i],
                                        lane_id, WARP_WIDTH);
    }
}

template <typename _Key, typename _Value, typename _Hash>
__device__ int32_t SlabHashContext<_Key, _Value, _Hash>::WarpFindKey(
        const _Key& key, const uint32_t lane_id, const ptr_t ptr) {
    bool is_lane_found =
            /* select key lanes */
            ((1 << lane_id) & PAIR_PTR_LANES_MASK)
            /* validate key addrs */
            && (ptr != EMPTY_PAIR_PTR)
            /* find keys in memory heap */
            && pair_allocator_ctx_.extract(ptr).first == key;

    return __ffs(__ballot_sync(PAIR_PTR_LANES_MASK, is_lane_found)) - 1;
}

template <typename _Key, typename _Value, typename _Hash>
__device__ __forceinline__ int32_t
SlabHashContext<_Key, _Value, _Hash>::WarpFindEmpty(const ptr_t ptr) {
    bool is_lane_empty = (ptr == EMPTY_PAIR_PTR);

    return __ffs(__ballot_sync(PAIR_PTR_LANES_MASK, is_lane_empty)) - 1;
}

template <typename _Key, typename _Value, typename _Hash>
__device__ __forceinline__ ptr_t
SlabHashContext<_Key, _Value, _Hash>::AllocateSlab(const uint32_t lane_id) {
    return slab_list_allocator_ctx_.WarpAllocate(lane_id);
}

template <typename _Key, typename _Value, typename _Hash>
__device__ __forceinline__ void SlabHashContext<_Key, _Value, _Hash>::FreeSlab(
        const ptr_t slab_ptr) {
    slab_list_allocator_ctx_.FreeUntouched(slab_ptr);
}

template <typename _Key, typename _Value, typename _Hash>
__device__ thrust::pair<iterator_t, bool>
SlabHashContext<_Key, _Value, _Hash>::Search(bool& to_search,
                                             const uint32_t lane_id,
                                             const uint32_t bucket_id,
                                             const _Key& query_key) {
    uint32_t work_queue = 0;
    uint32_t prev_work_queue = work_queue;
    uint32_t curr_slab_ptr = HEAD_SLAB_PTR;

    iterator_t iterator = NULL_ITERATOR;
    bool mask = false;

    /** > Loop when we have active lanes **/
    while ((work_queue = __ballot_sync(ACTIVE_LANES_MASK, to_search))) {
        /** 0. Restart from linked list head if the last query is finished **/
        curr_slab_ptr =
                (prev_work_queue != work_queue) ? HEAD_SLAB_PTR : curr_slab_ptr;
        uint32_t src_lane = __ffs(work_queue) - 1;
        uint32_t src_bucket =
                __shfl_sync(ACTIVE_LANES_MASK, bucket_id, src_lane, WARP_WIDTH);

        _Key src_key;
        WarpSyncKey(query_key, src_lane, src_key);

        /* Each lane in the warp reads a uint in the slab in parallel */
        const uint32_t unit_data =
                (curr_slab_ptr == HEAD_SLAB_PTR)
                        ? *(get_unit_ptr_from_list_head(src_bucket, lane_id))
                        : *(get_unit_ptr_from_list_nodes(curr_slab_ptr,
                                                         lane_id));

        int32_t lane_found = WarpFindKey(src_key, lane_id, unit_data);

        /** 1. Found in this slab, SUCCEED **/
        if (lane_found >= 0) {
            /* broadcast found value */
            ptr_t found_pair_internal_ptr = __shfl_sync(
                    ACTIVE_LANES_MASK, unit_data, lane_found, WARP_WIDTH);

            if (lane_id == src_lane) {
                to_search = false;

                iterator = found_pair_internal_ptr;
                mask = true;
            }
        }

        /** 2. Not found in this slab **/
        else {
            /* broadcast next slab: lane 31 reads 'next' */
            ptr_t next_slab_ptr = __shfl_sync(ACTIVE_LANES_MASK, unit_data,
                                              NEXT_SLAB_PTR_LANE, WARP_WIDTH);

            /** 2.1. Next slab is empty, ABORT **/
            if (next_slab_ptr == EMPTY_SLAB_PTR) {
                if (lane_id == src_lane) {
                    to_search = false;
                }
            }
            /** 2.2. Next slab exists, RESTART **/
            else {
                curr_slab_ptr = next_slab_ptr;
            }
        }

        prev_work_queue = work_queue;
    }

    return thrust::make_pair(iterator, mask);
}

/*
 * Insert: ABORT if found
 * replacePair: REPLACE if found
 * WE DO NOT ALLOW DUPLICATE KEYS
 */
template <typename _Key, typename _Value, typename _Hash>
__device__ thrust::pair<iterator_t, bool>
SlabHashContext<_Key, _Value, _Hash>::Insert(bool& to_be_inserted,
                                             const uint32_t lane_id,
                                             const uint32_t bucket_id,
                                             const _Key& key,
                                             const _Value& value) {
    uint32_t work_queue = 0;
    uint32_t prev_work_queue = 0;
    uint32_t curr_slab_ptr = HEAD_SLAB_PTR;

    iterator_t iterator = NULL_ITERATOR;
    bool mask = false;

    /** WARNING: Allocation should be finished in warp,
     * results are unexpected otherwise **/
    int prealloc_pair_internal_ptr = EMPTY_PAIR_PTR;
    if (to_be_inserted) {
        prealloc_pair_internal_ptr = pair_allocator_ctx_.Allocate();
        pair_allocator_ctx_.extract(prealloc_pair_internal_ptr) =
                thrust::make_pair(key, value);
    }

    /** > Loop when we have active lanes **/
    while ((work_queue = __ballot_sync(ACTIVE_LANES_MASK, to_be_inserted))) {
        /** 0. Restart from linked list head if last insertion is finished **/
        curr_slab_ptr =
                (prev_work_queue != work_queue) ? HEAD_SLAB_PTR : curr_slab_ptr;
        uint32_t src_lane = __ffs(work_queue) - 1;
        uint32_t src_bucket =
                __shfl_sync(ACTIVE_LANES_MASK, bucket_id, src_lane, WARP_WIDTH);
        _Key src_key;
        WarpSyncKey(key, src_lane, src_key);

        /* Each lane in the warp reads a uint in the slab */
        uint32_t unit_data =
                (curr_slab_ptr == HEAD_SLAB_PTR)
                        ? *(get_unit_ptr_from_list_head(src_bucket, lane_id))
                        : *(get_unit_ptr_from_list_nodes(curr_slab_ptr,
                                                         lane_id));

        int32_t lane_found = WarpFindKey(src_key, lane_id, unit_data);
        int32_t lane_empty = WarpFindEmpty(unit_data);

        /** Branch 1: key already existing, ABORT **/
        if (lane_found >= 0) {
            if (lane_id == src_lane) {
                /* free memory heap */
                to_be_inserted = false;
                pair_allocator_ctx_.Free(prealloc_pair_internal_ptr);
            }
        }

        /** Branch 2: empty slot available, try to insert **/
        else if (lane_empty >= 0) {
            if (lane_id == src_lane) {
                // TODO: check why we cannot put malloc here
                const uint32_t* unit_data_ptr =
                        (curr_slab_ptr == HEAD_SLAB_PTR)
                                ? get_unit_ptr_from_list_head(src_bucket,
                                                              lane_empty)
                                : get_unit_ptr_from_list_nodes(curr_slab_ptr,
                                                               lane_empty);
                ptr_t old_pair_internal_ptr =
                        atomicCAS((unsigned int*)unit_data_ptr, EMPTY_PAIR_PTR,
                                  prealloc_pair_internal_ptr);

                /** Branch 2.1: SUCCEED **/
                if (old_pair_internal_ptr == EMPTY_PAIR_PTR) {
                    to_be_inserted = false;

                    iterator = prealloc_pair_internal_ptr;
                    mask = true;
                }
                /** Branch 2.2: failed: RESTART
                 *  In the consequent attempt,
                 *  > if the same key was inserted in this slot,
                 *    we fall back to Branch 1;
                 *  > if a different key was inserted,
                 *    we go to Branch 2 or 3.
                 * **/
            }
        }

        /** Branch 3: nothing found in this slab, goto next slab **/
        else {
            /* broadcast next slab */
            ptr_t next_slab_ptr = __shfl_sync(ACTIVE_LANES_MASK, unit_data,
                                              NEXT_SLAB_PTR_LANE, WARP_WIDTH);

            /** Branch 3.1: next slab existing, RESTART this lane **/
            if (next_slab_ptr != EMPTY_SLAB_PTR) {
                curr_slab_ptr = next_slab_ptr;
            }

            /** Branch 3.2: next slab empty, try to allocate one **/
            else {
                ptr_t new_next_slab_ptr = AllocateSlab(lane_id);

                if (lane_id == NEXT_SLAB_PTR_LANE) {
                    const uint32_t* unit_data_ptr =
                            (curr_slab_ptr == HEAD_SLAB_PTR)
                                    ? get_unit_ptr_from_list_head(
                                              src_bucket, NEXT_SLAB_PTR_LANE)
                                    : get_unit_ptr_from_list_nodes(
                                              curr_slab_ptr,
                                              NEXT_SLAB_PTR_LANE);

                    ptr_t old_next_slab_ptr =
                            atomicCAS((unsigned int*)unit_data_ptr,
                                      EMPTY_SLAB_PTR, new_next_slab_ptr);

                    /** Branch 3.2.1: other thread allocated, RESTART lane
                     *  In the consequent attempt, goto Branch 2' **/
                    if (old_next_slab_ptr != EMPTY_SLAB_PTR) {
                        FreeSlab(new_next_slab_ptr);
                    }
                    /** Branch 3.2.2: this thread allocated, RESTART lane,
                     * 'goto Branch 2' **/
                }
            }
        }

        prev_work_queue = work_queue;
    }

    return thrust::make_pair(iterator, mask);
}

template <typename _Key, typename _Value, typename _Hash>
__device__ bool SlabHashContext<_Key, _Value, _Hash>::Remove(
        bool& to_be_deleted,
        const uint32_t lane_id,
        const uint32_t bucket_id,
        const _Key& key) {
    uint32_t work_queue = 0;
    uint32_t prev_work_queue = 0;
    uint32_t curr_slab_ptr = HEAD_SLAB_PTR;

    bool mask = false;

    /** > Loop when we have active lanes **/
    while ((work_queue = __ballot_sync(ACTIVE_LANES_MASK, to_be_deleted))) {
        /** 0. Restart from linked list head if last insertion is finished
         * **/
        curr_slab_ptr =
                (prev_work_queue != work_queue) ? HEAD_SLAB_PTR : curr_slab_ptr;
        uint32_t src_lane = __ffs(work_queue) - 1;
        uint32_t src_bucket =
                __shfl_sync(ACTIVE_LANES_MASK, bucket_id, src_lane, WARP_WIDTH);

        _Key src_key;
        WarpSyncKey(key, src_lane, src_key);

        const uint32_t unit_data =
                (curr_slab_ptr == HEAD_SLAB_PTR)
                        ? *(get_unit_ptr_from_list_head(src_bucket, lane_id))
                        : *(get_unit_ptr_from_list_nodes(curr_slab_ptr,
                                                         lane_id));

        int32_t lane_found = WarpFindKey(src_key, lane_id, unit_data);

        /** Branch 1: key found **/
        if (lane_found >= 0) {
            ptr_t src_pair_internal_ptr = __shfl_sync(
                    ACTIVE_LANES_MASK, unit_data, lane_found, WARP_WIDTH);

            if (lane_id == src_lane) {
                uint32_t* unit_data_ptr =
                        (curr_slab_ptr == HEAD_SLAB_PTR)
                                ? get_unit_ptr_from_list_head(src_bucket,
                                                              lane_found)
                                : get_unit_ptr_from_list_nodes(curr_slab_ptr,
                                                               lane_found);
                ptr_t pair_to_delete = *unit_data_ptr;

                // TODO: keep in mind the potential double free problem
                ptr_t old_key_value_pair =
                        atomicCAS((unsigned int*)(unit_data_ptr),
                                  pair_to_delete, EMPTY_PAIR_PTR);
                /** Branch 1.1: this thread reset, free src_addr **/
                if (old_key_value_pair == pair_to_delete) {
                    pair_allocator_ctx_.Free(src_pair_internal_ptr);
                    mask = true;
                }
                /** Branch 1.2: other thread did the job, avoid double free
                 * **/
                to_be_deleted = false;
            }
        } else {  // no matching slot found:
            ptr_t next_slab_ptr = __shfl_sync(ACTIVE_LANES_MASK, unit_data,
                                              NEXT_SLAB_PTR_LANE, WARP_WIDTH);
            if (next_slab_ptr == EMPTY_SLAB_PTR) {
                // not found:
                to_be_deleted = false;
            } else {
                curr_slab_ptr = next_slab_ptr;
            }
        }
        prev_work_queue = work_queue;
    }

    return mask;
}

//=== Individual search kernel:
template <typename _Key, typename _Value, typename _Hash>
__global__ void SearchKernel(SlabHashContext<_Key, _Value, _Hash> slab_hash_ctx,
                             _Key* keys,
                             _Value* values,
                             uint8_t* founds,
                             uint32_t num_queries) {
    uint32_t tid = threadIdx.x + blockIdx.x * blockDim.x;
    uint32_t lane_id = threadIdx.x & 0x1F;

    /* This warp is idle */
    if ((tid - lane_id) >= num_queries) {
        return;
    }

    /* Initialize the memory allocator on each warp */
    slab_hash_ctx.get_slab_alloc_ctx().Init(tid, lane_id);

    bool lane_active = false;
    uint32_t bucket_id = 0;
    _Key key;

    if (tid < num_queries) {
        lane_active = true;
        key = keys[tid];
        bucket_id = slab_hash_ctx.ComputeBucket(key);
    }

    thrust::pair<iterator_t, bool> result =
            slab_hash_ctx.Search(lane_active, lane_id, bucket_id, key);

    if (tid < num_queries) {
        bool found = result.second;
        founds[tid] = found;
        values[tid] = found ? slab_hash_ctx.get_pair_alloc_ctx()
                                      .extract(result.first)
                                      .second
                            : _Value(0);
    }
}

template <typename _Key, typename _Value, typename _Hash>
__global__ void InsertKernel(SlabHashContext<_Key, _Value, _Hash> slab_hash_ctx,
                             _Key* keys,
                             _Value* values,
                             uint32_t num_keys) {
    uint32_t tid = threadIdx.x + blockIdx.x * blockDim.x;
    uint32_t lane_id = threadIdx.x & 0x1F;

    if ((tid - lane_id) >= num_keys) {
        return;
    }

    slab_hash_ctx.get_slab_alloc_ctx().Init(tid, lane_id);

    bool lane_active = false;
    uint32_t bucket_id = 0;
    _Key key;
    _Value value;

    if (tid < num_keys) {
        lane_active = true;
        key = keys[tid];
        value = values[tid];
        bucket_id = slab_hash_ctx.ComputeBucket(key);
    }

    slab_hash_ctx.Insert(lane_active, lane_id, bucket_id, key, value);
}

template <typename _Key, typename _Value, typename _Hash>
__global__ void RemoveKernel(SlabHashContext<_Key, _Value, _Hash> slab_hash_ctx,
                             _Key* keys,
                             uint32_t num_keys) {
    uint32_t tid = threadIdx.x + blockIdx.x * blockDim.x;
    uint32_t lane_id = threadIdx.x & 0x1F;

    if ((tid - lane_id) >= num_keys) {
        return;
    }

    slab_hash_ctx.get_slab_alloc_ctx().Init(tid, lane_id);

    bool lane_active = false;
    uint32_t bucket_id = 0;
    _Key key;

    if (tid < num_keys) {
        lane_active = true;
        key = keys[tid];
        bucket_id = slab_hash_ctx.ComputeBucket(key);
    }

    slab_hash_ctx.Remove(lane_active, lane_id, bucket_id, key);
}

/*
 * This kernel can be used to compute total number of elements within each
 * bucket. The final results per bucket is stored in d_count_result array
 */
template <typename _Key, typename _Value, typename _Hash>
__global__ void bucket_count_kernel(
        SlabHashContext<_Key, _Value, _Hash> slab_hash_ctx,
        uint32_t* d_count_result,
        uint32_t num_buckets) {
    // global warp ID
    uint32_t tid = threadIdx.x + blockIdx.x * blockDim.x;
    uint32_t wid = tid >> 5;
    // assigning a warp per bucket
    if (wid >= num_buckets) {
        return;
    }

    uint32_t lane_id = threadIdx.x & 0x1F;

    // initializing the memory allocator on each warp:
    slab_hash_ctx.get_slab_alloc_ctx().Init(tid, lane_id);

    uint32_t count = 0;

    uint32_t src_unit_data =
            *slab_hash_ctx.get_unit_ptr_from_list_head(wid, lane_id);

    count += __popc(__ballot_sync(PAIR_PTR_LANES_MASK,
                                  src_unit_data != EMPTY_PAIR_PTR));
    uint32_t next = __shfl_sync(0xFFFFFFFF, src_unit_data, 31, 32);

    while (next != EMPTY_SLAB_PTR) {
        src_unit_data =
                *slab_hash_ctx.get_unit_ptr_from_list_nodes(next, lane_id);
        count += __popc(__ballot_sync(PAIR_PTR_LANES_MASK,
                                      src_unit_data != EMPTY_PAIR_PTR));
        next = __shfl_sync(0xFFFFFFFF, src_unit_data, 31, 32);
    }
    // writing back the results:
    if (lane_id == 0) {
        d_count_result[wid] = count;
    }
}

/*
 * This kernel goes through all allocated bitmaps for a slab_hash_ctx's
 * allocator and store number of allocated slabs.
 * TODO: this should be moved into allocator's codebase (violation of layers)
 */
template <typename _Key, typename _Value, typename _Hash>
__global__ void compute_stats_allocators(
        uint32_t* d_count_super_block,
        SlabHashContext<_Key, _Value, _Hash> slab_hash_ctx) {
    uint32_t tid = threadIdx.x + blockIdx.x * blockDim.x;

    int num_bitmaps =
            slab_hash_ctx.get_slab_alloc_ctx().NUM_MEM_BLOCKS_PER_SUPER_BLOCK_ *
            32;
    if (tid >= num_bitmaps) {
        return;
    }

    for (int i = 0; i < slab_hash_ctx.get_slab_alloc_ctx().num_super_blocks_;
         i++) {
        uint32_t read_bitmap = *(
                slab_hash_ctx.get_slab_alloc_ctx().get_ptr_for_bitmap(i, tid));
        atomicAdd(&d_count_super_block[i], __popc(read_bitmap));
    }
}

template <typename _Key, typename _Value, typename _Hash>
SlabHash<_Key, _Value, _Hash>::SlabHash(const uint32_t max_bucket_count,
                                        const uint32_t max_keyvalue_count,
                                        uint32_t device_idx)
    : num_buckets_(max_bucket_count),
      device_idx_(device_idx),
      bucket_list_head_(nullptr) {
    // allocate an initialize the allocator:
    pair_allocator_ = std::make_shared<MemoryAlloc<thrust::pair<_Key, _Value>>>(
            max_keyvalue_count);
    slab_list_allocator_ = std::make_shared<SlabAlloc>();

    int32_t device_count = 0;
    CHECK_CUDA(cudaGetDeviceCount(&device_count));
    assert(device_idx_ < device_count);
    CHECK_CUDA(cudaSetDevice(device_idx_));

    // allocating initial buckets:
    CHECK_CUDA(cudaMalloc(&bucket_list_head_, sizeof(Slab) * num_buckets_));
    CHECK_CUDA(
            cudaMemset(bucket_list_head_, 0xFF, sizeof(Slab) * num_buckets_));

    gpu_context_.Setup(bucket_list_head_, num_buckets_,
                       slab_list_allocator_->getContext(),
                       pair_allocator_->gpu_context_);
}

template <typename _Key, typename _Value, typename _Hash>
SlabHash<_Key, _Value, _Hash>::~SlabHash() {
    CHECK_CUDA(cudaSetDevice(device_idx_));
    CHECK_CUDA(cudaFree(bucket_list_head_));
}

template <typename _Key, typename _Value, typename _Hash>
void SlabHash<_Key, _Value, _Hash>::Insert(_Key* keys,
                                           _Value* values,
                                           uint32_t num_keys) {
    const uint32_t num_blocks = (num_keys + BLOCKSIZE_ - 1) / BLOCKSIZE_;
    // calling the kernel for bulk build:
    CHECK_CUDA(cudaSetDevice(device_idx_));
    InsertKernel<_Key, _Value, _Hash>
            <<<num_blocks, BLOCKSIZE_>>>(gpu_context_, keys, values, num_keys);
}

template <typename _Key, typename _Value, typename _Hash>
void SlabHash<_Key, _Value, _Hash>::Search(_Key* keys,
                                           _Value* values,
                                           uint8_t* founds,
                                           uint32_t num_queries) {
    CHECK_CUDA(cudaSetDevice(device_idx_));
    const uint32_t num_blocks = (num_queries + BLOCKSIZE_ - 1) / BLOCKSIZE_;
    SearchKernel<_Key, _Value, _Hash><<<num_blocks, BLOCKSIZE_>>>(
            gpu_context_, keys, values, founds, num_queries);
}

template <typename _Key, typename _Value, typename _Hash>
void SlabHash<_Key, _Value, _Hash>::Remove(_Key* keys, uint32_t num_keys) {
    CHECK_CUDA(cudaSetDevice(device_idx_));
    const uint32_t num_blocks = (num_keys + BLOCKSIZE_ - 1) / BLOCKSIZE_;
    RemoveKernel<_Key, _Value, _Hash>
            <<<num_blocks, BLOCKSIZE_>>>(gpu_context_, keys, num_keys);
}

template <typename _Key, typename _Value, typename _Hash>
double SlabHash<_Key, _Value, _Hash>::ComputeLoadFactor(int flag = 0) {
    uint32_t* h_bucket_count = new uint32_t[num_buckets_];
    uint32_t* d_bucket_count;
    CHECK_CUDA(cudaMalloc((void**)&d_bucket_count,
                          sizeof(uint32_t) * num_buckets_));
    CHECK_CUDA(cudaMemset(d_bucket_count, 0, sizeof(uint32_t) * num_buckets_));

    const auto& dynamic_alloc = gpu_context_.get_slab_alloc_ctx();
    const uint32_t num_super_blocks = dynamic_alloc.num_super_blocks_;
    uint32_t* h_count_super_blocks = new uint32_t[num_super_blocks];
    uint32_t* d_count_super_blocks;
    CHECK_CUDA(cudaMalloc((void**)&d_count_super_blocks,
                          sizeof(uint32_t) * num_super_blocks));
    CHECK_CUDA(cudaMemset(d_count_super_blocks, 0,
                          sizeof(uint32_t) * num_super_blocks));
    //---------------------------------
    // counting the number of inserted elements:
    const uint32_t blocksize = 128;
    const uint32_t num_blocks = (num_buckets_ * 32 + blocksize - 1) / blocksize;
    bucket_count_kernel<_Key, _Value, _Hash><<<num_blocks, blocksize>>>(
            gpu_context_, d_bucket_count, num_buckets_);
    CHECK_CUDA(cudaMemcpy(h_bucket_count, d_bucket_count,
                          sizeof(uint32_t) * num_buckets_,
                          cudaMemcpyDeviceToHost));

    int total_elements_stored = 0;
    for (int i = 0; i < num_buckets_; i++) {
        total_elements_stored += h_bucket_count[i];
    }

    if (flag) {
        printf("## Total elements stored: %d (%lu bytes).\n",
               total_elements_stored,
               total_elements_stored * (sizeof(_Key) + sizeof(_Value)));
    }

    // counting total number of allocated memory units:
    int num_mem_units = dynamic_alloc.NUM_MEM_BLOCKS_PER_SUPER_BLOCK_ * 32;
    int num_cuda_blocks = (num_mem_units + blocksize - 1) / blocksize;
    compute_stats_allocators<<<num_cuda_blocks, blocksize>>>(
            d_count_super_blocks, gpu_context_);

    CHECK_CUDA(cudaMemcpy(h_count_super_blocks, d_count_super_blocks,
                          sizeof(uint32_t) * num_super_blocks,
                          cudaMemcpyDeviceToHost));

    // computing load factor
    int total_mem_units = num_buckets_;
    for (int i = 0; i < num_super_blocks; i++)
        total_mem_units += h_count_super_blocks[i];

    double load_factor =
            double(total_elements_stored * (sizeof(_Key) + sizeof(_Value))) /
            double(total_mem_units * WARP_WIDTH * sizeof(uint32_t));

    if (d_count_super_blocks) CHECK_CUDA(cudaFree(d_count_super_blocks));
    if (d_bucket_count) CHECK_CUDA(cudaFree(d_bucket_count));
    delete[] h_bucket_count;
    delete[] h_count_super_blocks;

    return load_factor;
}
