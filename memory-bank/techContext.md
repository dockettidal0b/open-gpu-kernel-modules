# NVIDIA GPU技术上下文

## 开发环境
### MacOS开发环境
1. 系统要求
   - macOS操作系统
   - 开发工具链配置
   - 交叉编译支持

2. 编译环境
   - 需要交叉编译工具链
   - 目标平台为Linux内核
   - 支持多平台构建

3. 开发工具
   - VSCode作为主要IDE
   - Git版本控制
   - 远程测试环境

# GPU技术上下文

## BAR1内存映射
### 硬件限制
1. 基本限制
   - PCIe BAR1空间最大支持32GB
   - 影响P2P直接访问能力
   - 涉及DMA映射管理

2. 架构约束
   - PCIe地址空间固定
   - 64KB页面对齐要求
   - DMA映射需要连续性

### 显存配置
1. 4090 24GB配置
   - BAR1限制在32GB内
   - 可完整映射所有显存
   - P2P功能完全可用

2. 4090 48GB配置
   - 超出32GB BAR1限制
   - 无法完整映射显存
   - 需要分段映射

## 分段映射实现
### 基础架构
1. 页面映射
```c
// 64KB页面管理
#define GPU_PAGE_SHIFT   16
#define GPU_PAGE_SIZE    (1UL << GPU_PAGE_SHIFT)
#define GPU_PAGE_MASK    (~(GPU_PAGE_SIZE - 1))

// 页面地址计算
static inline uint64_t calc_page_address(uint64_t addr) {
    return addr & GPU_PAGE_MASK;
}

// 段大小计算
static inline uint64_t calc_segment_size(uint64_t size) {
    return RM_ALIGN_UP(size, GPU_PAGE_SIZE);
}
```

2. GMMU配置
```c
struct GmmuConfig {
    // 使用系统内存非一致性访问
    NvU32 aperture;    // GMMU_APERTURE_SYS_NONCOH
    NvU32 kind;        // 内存类型
    NvU32 pageSize;    // 64KB对齐
    NvBool isValid;    // 映射有效位
};

// 初始化GMMU配置
void initGmmuConfig(struct GmmuConfig *config) {
    config->aperture = GMMU_APERTURE_SYS_NONCOH;
    config->pageSize = GPU_PAGE_SIZE;
    config->isValid = NV_TRUE;
}
```

3. DMA映射
```c
struct DmaMapping {
    uint64_t fbAddr;     // 显存物理地址
    uint64_t bar1Addr;   // BAR1映射地址
    uint64_t size;       // 映射大小
    NvBool   isActive;   // 映射状态
};

struct DmaManager {
    struct DmaMapping *mappings;
    int mappingCount;
    spinlock_t lock;
};
```

### 映射策略
1. 静态映射
```c
// 固定映射管理
struct StaticMapping {
    uint64_t base;          // 起始地址
    uint64_t size;          // 映射大小
    struct {
        void *pageTable;    // 页表指针
        void *dmaMapping;   // DMA映射
    } gpu;
    spinlock_t lock;        // 访问锁
};

// 初始化静态映射
NV_STATUS setupStaticMapping(struct StaticMapping *mapping) {
    // 对齐到64KB边界
    mapping->base = RM_ALIGN_UP(mapping->base, GPU_PAGE_SIZE);
    mapping->size = RM_ALIGN_DOWN(mapping->size, GPU_PAGE_SIZE);
    
    // 初始化GPU映射
    return initGpuMapping(&mapping->gpu);
}
```

2. 动态映射
```c
// 动态段管理
struct DynamicSegment {
    uint64_t vaddr;        // 虚拟地址
    uint64_t size;         // 段大小
    NvBool isValid;        // 有效状态
    struct {
        void *pageTable;   // 页表指针
        void *dmaMap;      // DMA映射
    } mapping;
};

// 段管理器
struct SegmentManager {
    struct DynamicSegment *segments;
    int segmentCount;
    struct LRUCache *cache;
    spinlock_t lock;
};
```

### 访问控制
1. 页表管理
```c
struct PageTableEntry {
    uint64_t physAddr;     // 物理地址
    uint64_t virtAddr;     // 虚拟地址
    NvU32 aperture;       // 访问类型
    NvU32 valid    : 1;   // 有效位
    NvU32 cached   : 1;   // 缓存位
    NvU32 readOnly : 1;   // 只读位
};

struct PageTable {
    struct PageTableEntry *entries;
    uint32_t entryCount;
    spinlock_t lock;
};
```

2. 映射生命周期
```c
enum MappingState {
    MAPPING_INIT,
    MAPPING_ACTIVE,
    MAPPING_INVALID,
    MAPPING_ERROR
};

struct MappingContext {
    uint64_t virtAddr;
    uint64_t physAddr;
    uint64_t size;
    enum MappingState state;
    void *userData;
};
```

## 优化实现
### 缓存管理
1. LRU缓存
```c
struct LRUNode {
    struct list_head list;
    uint64_t addr;
    uint64_t lastAccess;
    void *data;
};

struct LRUCache {
    struct list_head lru_list;
    int maxEntries;
    spinlock_t lock;
    atomic_t currentEntries;
};

// 更新访问时间
void updateAccess(struct LRUCache *cache, uint64_t addr) {
    struct LRUNode *node = findNode(cache, addr);
    if (node) {
        node->lastAccess = getTimestamp();
        list_move_tail(&node->list, &cache->lru_list);
    }
}
```

2. 预读策略
```c
struct PrefetchPolicy {
    bool enabled;
    uint64_t threshold;
    uint64_t maxSize;
    void (*prefetchCallback)(void *context, uint64_t addr, uint64_t size);
};

// 预取控制
struct PrefetchControl {
    struct PrefetchPolicy policy;
    atomic_t activeCount;
    spinlock_t lock;
};
```

### 监控系统
1. 性能计数器
```c
struct PerformanceCounters {
    atomic64_t hits;          // 缓存命中
    atomic64_t misses;        // 缓存未命中
    atomic64_t invalidations; // 映射失效
    atomic64_t errors;        // 错误计数
    struct {
        uint64_t total;
        uint64_t min;
        uint64_t max;
    } latency;
};

// 更新性能计数
void updateCounters(struct PerformanceCounters *counters, 
                   uint64_t latency,
                   bool hit) {
    if (hit)
        atomic64_inc(&counters->hits);
    else
        atomic64_inc(&counters->misses);
        
    // 更新延迟统计
    if (latency < counters->latency.min)
        counters->latency.min = latency;
    if (latency > counters->latency.max)
        counters->latency.max = latency;
}
```

2. 诊断支持
```c
struct DiagnosticInfo {
    struct {
        uint64_t totalMappings;
        uint64_t activeMappings;
        uint64_t failedMappings;
    } stats;
    
    struct {
        uint64_t bar1Total;
        uint64_t bar1Used;
        uint64_t bar1Free;
    } memory;
    
    struct {
        bool isEnabled;
        uint64_t errorCode;
        char errorMsg[256];
    } error;
};
```

## 实现注意事项
### 关键点
1. GMMU配置
   - 使用GMMU_APERTURE_SYS_NONCOH
   - 禁用peer aperture
   - 正确设置BAR1基地址

2. 页表管理
   - 64KB页面对齐
   - 维护映射生命周期
   - 处理部分映射情况

3. DMA配置
   - 配置正确的访问权限
   - 处理地址转换
   - 管理内存一致性
