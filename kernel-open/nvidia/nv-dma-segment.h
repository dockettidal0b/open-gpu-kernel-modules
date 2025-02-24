/*
 * SPDX-FileCopyrightText: Copyright (c) 2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: MIT
 *
 * Header file for BAR1分段映射管理
 */

#ifndef _NV_DMA_SEGMENT_H_
#define _NV_DMA_SEGMENT_H_

#include "os-interface.h"
#include "nv.h"
#include "nv-linux.h"
#include "nv-p2p.h"

// 段管理器配置
#define NV_SEG_SIZE_64K   (64 * 1024)    // 基本段大小64KB
#define NV_SEG_MAX_COUNT  512            // 最大段数量
#define NV_SEG_CACHE_SIZE 64             // LRU缓存大小

// 段状态定义
typedef enum {
    NV_SEG_STATE_FREE = 0,      // 空闲
    NV_SEG_STATE_MAPPED,        // 已映射
    NV_SEG_STATE_PENDING,       // 等待映射
    NV_SEG_STATE_ERROR          // 错误状态
} nv_seg_state_t;

// 段描述符
typedef struct nv_segment {
    uint64_t        fb_addr;     // 显存地址
    uint64_t        bar1_addr;   // BAR1映射地址
    uint64_t        size;        // 段大小
    nv_seg_state_t  state;       // 段状态
    uint64_t        last_access; // 最后访问时间
    struct {
        atomic_t hits;          // 命中次数
        atomic_t misses;        // 未命中次数
    } stats;
    struct list_head list;      // 链表节点
} nv_segment_t;

// LRU缓存
typedef struct nv_segment_cache {
    struct list_head lru_list;    // LRU链表
    spinlock_t      lock;         // 访问锁
    atomic_t        count;        // 当前数量
    uint32_t        max_count;    // 最大容量
} nv_segment_cache_t;

// 段管理器
typedef struct nv_segment_mgr {
    nv_segment_t      *segments;       // 段数组
    uint32_t           seg_count;      // 段数量
    nv_segment_cache_t cache;          // LRU缓存
    spinlock_t         lock;           // 管理器锁
    struct {
        atomic64_t total_maps;        // 总映射次数
        atomic64_t cache_hits;        // 缓存命中
        atomic64_t cache_misses;      // 缓存未命中
        atomic64_t map_failures;      // 映射失败
    } stats;
} nv_segment_mgr_t;

// 接口函数声明
NV_STATUS nv_segment_mgr_init(nv_segment_mgr_t *mgr);
void nv_segment_mgr_destroy(nv_segment_mgr_t *mgr);
NV_STATUS nv_segment_map(nv_segment_mgr_t *mgr, uint64_t fb_addr, 
                        uint64_t size, uint64_t *bar1_addr);
void nv_segment_unmap(nv_segment_mgr_t *mgr, uint64_t bar1_addr);
NV_STATUS nv_segment_get_stats(nv_segment_mgr_t *mgr, 
                              void *stats, uint32_t size);

#endif // _NV_DMA_SEGMENT_H_
