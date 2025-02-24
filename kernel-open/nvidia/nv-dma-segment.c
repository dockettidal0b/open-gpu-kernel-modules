/*
 * SPDX-FileCopyrightText: Copyright (c) 2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: MIT
 *
 * BAR1分段映射管理实现
 */

#include "nv-dma-segment.h"

// 初始化段管理器
NV_STATUS nv_segment_mgr_init(nv_segment_mgr_t *mgr)
{
    NV_STATUS status;
    uint32_t i;

    if (!mgr)
        return NV_ERR_INVALID_ARGUMENT;

    // 分配段数组
    status = os_alloc_mem((void **)&mgr->segments,
                         sizeof(nv_segment_t) * NV_SEG_MAX_COUNT);
    if (status != NV_OK)
        return status;

    // 初始化段
    for (i = 0; i < NV_SEG_MAX_COUNT; i++) {
        mgr->segments[i].state = NV_SEG_STATE_FREE;
        mgr->segments[i].fb_addr = 0;
        mgr->segments[i].bar1_addr = 0;
        mgr->segments[i].size = 0;
        mgr->segments[i].last_access = 0;
        atomic_set(&mgr->segments[i].stats.hits, 0);
        atomic_set(&mgr->segments[i].stats.misses, 0);
        INIT_LIST_HEAD(&mgr->segments[i].list);
    }

    // 初始化LRU缓存
    INIT_LIST_HEAD(&mgr->cache.lru_list);
    NV_SPIN_LOCK_INIT(&mgr->cache.lock);
    atomic_set(&mgr->cache.count, 0);
    mgr->cache.max_count = NV_SEG_CACHE_SIZE;

    // 初始化管理器
    NV_SPIN_LOCK_INIT(&mgr->lock);
    mgr->seg_count = NV_SEG_MAX_COUNT;
    
    // 初始化统计计数器
    atomic64_set(&mgr->stats.total_maps, 0);
    atomic64_set(&mgr->stats.cache_hits, 0);
    atomic64_set(&mgr->stats.cache_misses, 0);
    atomic64_set(&mgr->stats.map_failures, 0);

    return NV_OK;
}

// 销毁段管理器
void nv_segment_mgr_destroy(nv_segment_mgr_t *mgr)
{
    if (!mgr)
        return;

    // 确保所有段都已释放
    nv_segment_t *seg;
    uint32_t i;
    
    for (i = 0; i < mgr->seg_count; i++) {
        seg = &mgr->segments[i];
        if (seg->state == NV_SEG_STATE_MAPPED) {
            nv_segment_unmap(mgr, seg->bar1_addr);
        }
    }

    // 释放资源
    if (mgr->segments)
        os_free_mem(mgr->segments);

    mgr->segments = NULL;
    mgr->seg_count = 0;
}

// 查找空闲段
static nv_segment_t* nv_segment_find_free(nv_segment_mgr_t *mgr)
{
    uint32_t i;
    nv_segment_t *seg;

    for (i = 0; i < mgr->seg_count; i++) {
        seg = &mgr->segments[i];
        if (seg->state == NV_SEG_STATE_FREE)
            return seg;
    }

    return NULL;
}

// 从LRU缓存中淘汰一个段
static nv_segment_t* nv_segment_evict_lru(nv_segment_mgr_t *mgr)
{
    nv_segment_t *seg = NULL;
    
    NV_SPIN_LOCK(&mgr->cache.lock);
    if (!list_empty(&mgr->cache.lru_list)) {
        seg = list_entry(mgr->cache.lru_list.prev,
                        nv_segment_t, list);
        list_del(&seg->list);
        atomic_dec(&mgr->cache.count);
    }
    NV_SPIN_UNLOCK(&mgr->cache.lock);

    return seg;
}

// 将段添加到LRU缓存
static void nv_segment_add_to_cache(nv_segment_mgr_t *mgr, 
                                  nv_segment_t *seg)
{
    NV_SPIN_LOCK(&mgr->cache.lock);
    
    // 如果缓存已满，先淘汰一个
    if (atomic_read(&mgr->cache.count) >= mgr->cache.max_count) {
        nv_segment_t *victim = nv_segment_evict_lru(mgr);
        if (victim) {
            victim->state = NV_SEG_STATE_FREE;
        }
    }

    // 添加到缓存头部
    list_add(&seg->list, &mgr->cache.lru_list);
    atomic_inc(&mgr->cache.count);
    
    NV_SPIN_UNLOCK(&mgr->cache.lock);
}

// 更新段的访问时间
static void nv_segment_update_access(nv_segment_mgr_t *mgr,
                                   nv_segment_t *seg)
{
    NV_SPIN_LOCK(&mgr->cache.lock);
    
    // 移动到LRU链表头部
    if (!list_empty(&seg->list)) {
        list_del(&seg->list);
        list_add(&seg->list, &mgr->cache.lru_list);
    }
    
    // 更新访问时间
    seg->last_access = os_get_timer_ticks();
    
    NV_SPIN_UNLOCK(&mgr->cache.lock);
}

// 映射新段
NV_STATUS nv_segment_map(nv_segment_mgr_t *mgr,
                        uint64_t fb_addr,
                        uint64_t size,
                        uint64_t *bar1_addr)
{
    NV_STATUS status = NV_OK;
    nv_segment_t *seg = NULL;

    if (!mgr || !bar1_addr)
        return NV_ERR_INVALID_ARGUMENT;

    // 统计计数
    atomic64_inc(&mgr->stats.total_maps);

    NV_SPIN_LOCK(&mgr->lock);

    // 先查找空闲段
    seg = nv_segment_find_free(mgr);
    if (!seg) {
        // 如果没有空闲段，尝试从缓存中淘汰
        seg = nv_segment_evict_lru(mgr);
        if (!seg) {
            atomic64_inc(&mgr->stats.map_failures);
            status = NV_ERR_NO_MEMORY;
            goto done;
        }
    }

    // 初始化段
    seg->fb_addr = fb_addr;
    seg->size = size;
    seg->state = NV_SEG_STATE_MAPPED;
    seg->last_access = os_get_timer_ticks();

    // 执行BAR1映射操作
    nv_state_t *nv = NV_GET_NV_STATE(NV_GET_NVL_FROM_NV_STATE(mgr));
    nv_dma_device_t dma_dev = {{ 0 }};
    void *priv = NULL;
    NvU64 dma_addr;

    dma_dev.dev = &nv->pci_dev->dev;
    dma_dev.addressable_range.limit = nv->pci_dev->dma_mask;

    // 计算需要映射的页数
    NvU32 page_count = (size + PAGE_SIZE - 1) >> PAGE_SHIFT;
    
    // 执行DMA映射
    status = nv_dma_map_pages(&dma_dev, page_count, &dma_addr, NV_TRUE, 
                             NV_MEMORY_UNCACHED, &priv);
    if (status != NV_OK) {
        atomic64_inc(&mgr->stats.map_failures);
        goto done;
    }

    // 设置段信息
    seg->bar1_addr = dma_addr;
    *bar1_addr = dma_addr;

    // 添加到LRU缓存
    nv_segment_add_to_cache(mgr, seg);
    atomic64_inc(&mgr->stats.cache_hits);

done:
    NV_SPIN_UNLOCK(&mgr->lock);
    return status;
}

// 解除段映射
void nv_segment_unmap(nv_segment_mgr_t *mgr, uint64_t bar1_addr)
{
    nv_segment_t *seg;
    uint32_t i;

    if (!mgr)
        return;

    NV_SPIN_LOCK(&mgr->lock);

    // 查找对应的段
    for (i = 0; i < mgr->seg_count; i++) {
        seg = &mgr->segments[i];
        if (seg->state == NV_SEG_STATE_MAPPED &&
            seg->bar1_addr == bar1_addr) {
            // 解除BAR1映射
            nv_state_t *nv = NV_GET_NV_STATE(NV_GET_NVL_FROM_NV_STATE(mgr));
            nv_dma_device_t dma_dev = {{ 0 }};
            void *priv = NULL;

            dma_dev.dev = &nv->pci_dev->dev;
            dma_dev.addressable_range.limit = nv->pci_dev->dma_mask;

            // 计算映射的页数
            NvU32 page_count = (seg->size + PAGE_SIZE - 1) >> PAGE_SHIFT;
            
            // 解除DMA映射
            nv_dma_unmap_pages(&dma_dev, page_count, &seg->bar1_addr, &priv);
            
            // 从LRU缓存中移除
            NV_SPIN_LOCK(&mgr->cache.lock);
            if (!list_empty(&seg->list))
                list_del(&seg->list);
            atomic_dec(&mgr->cache.count);
            NV_SPIN_UNLOCK(&mgr->cache.lock);

            // 重置段状态
            seg->state = NV_SEG_STATE_FREE;
            seg->fb_addr = 0;
            seg->bar1_addr = 0;
            seg->size = 0;
            break;
        }
    }

    NV_SPIN_UNLOCK(&mgr->lock);
}

// 获取统计信息
NV_STATUS nv_segment_get_stats(nv_segment_mgr_t *mgr,
                              void *stats,
                              uint32_t size)
{
    struct {
        uint64_t total_maps;    // 总映射次数
        uint64_t cache_hits;    // 缓存命中数
        uint64_t cache_misses;  // 缓存未命中数
        uint64_t map_failures;  // 映射失败次数
    } *s = (void *)stats;

    if (!mgr || !stats || size < sizeof(*s))
        return NV_ERR_INVALID_ARGUMENT;

    s->total_maps = atomic64_read(&mgr->stats.total_maps);
    s->cache_hits = atomic64_read(&mgr->stats.cache_hits);
    s->cache_misses = atomic64_read(&mgr->stats.cache_misses);
    s->map_failures = atomic64_read(&mgr->stats.map_failures);

    return NV_OK;
}
