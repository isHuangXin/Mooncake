# 基于 Mooncake 实现 KVCache Flat Memory System

## 问题

> 我要基于Mooncake实现一个KVCache Flat Memory System该怎么修改代码，这个系统的核心是将Mooncake的KVCache分层存储架构（GPU HBM，DRAM，SSD）变成了将GPU HBM，DRAM，SSD视为同一个层级的存储，不再将KVCache数据区分冷热，不再将热的数据放在HBM，不再将冷的数据放在SSD。而是直接将KVCache任意存在GPU HBM，DRAM，SSD的位置。并请告诉我这样做会不会让TTFT更长。

---

## 回答

### 一、架构对比

#### 1.1 原有分层架构 vs Flat Memory 架构

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                          架构对比                                            │
├────────────────────────────────┬────────────────────────────────────────────┤
│       原有分层架构              │         Flat Memory 架构                    │
├────────────────────────────────┼────────────────────────────────────────────┤
│                                │                                            │
│  ┌──────────────┐              │         ┌──────────────┐                   │
│  │  GPU HBM     │ ← 热数据      │         │  GPU HBM     │                   │
│  │  (L1 Cache)  │              │         └──────┬───────┘                   │
│  └──────┬───────┘              │                │                           │
│         │ 驱逐                  │                ▼                           │
│         ▼                      │     ┌─────────────────────┐                │
│  ┌──────────────┐              │     │ KVCache Prefix      │                │
│  │  Host DRAM   │ ← 温数据      │     │ Parser              │                │
│  │  (L2 Cache)  │              │     └─────────┬───────────┘                │
│  └──────┬───────┘              │               │                            │
│         │ Write-back           │               ▼                            │
│         ▼                      │     ┌─────────────────────┐                │
│  ┌──────────────┐              │     │ Flat Memory         │                │
│  │  Remote DRAM │ ← 共享数据    │     │ Interface           │                │
│  │  (L3 Cache)  │              │     └──┬──────┬───────┬───┘                │
│  └──────┬───────┘              │        │      │       │                    │
│         │ 异步持久化            │        ▼      ▼       ▼                    │
│         ▼                      │  ┌────────┬────────┬────────────┐          │
│  ┌──────────────┐              │  │ System │ Local  │ Shared     │          │
│  │  DFS/SSD     │ ← 冷数据      │  │ DRAM   │ SSD    │ Object/DFS │          │
│  └──────────────┘              │  └────────┴────────┴────────────┘          │
│                                │                                            │
│  特点：                         │  特点：                                     │
│  • 自动冷热分层                 │  • 统一存储视图                             │
│  • 基于访问频率迁移             │  • 无自动数据迁移                           │
│  • Soft Pin 保护热数据          │  • 用户/调度器决定放置位置                   │
│                                │  • 所有介质平等对待                         │
│                                │                                            │
└────────────────────────────────┴────────────────────────────────────────────┘
```

#### 1.2 核心区别

| 特性 | 原有分层架构 | Flat Memory 架构 |
|------|-------------|------------------|
| **数据分类** | 热/温/冷 三级 | 无分类，统一对待 |
| **放置策略** | 自动基于访问频率 | 用户/调度器显式指定 |
| **数据迁移** | 自动 Eviction/Write-back | 无自动迁移 |
| **Soft Pin** | 热数据保护 | 移除 |
| **存储视图** | 分层缓存 | 统一地址空间 |

---

### 二、代码修改方案

#### 2.1 需要修改的核心模块

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                        代码修改清单                                          │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  1. Master Service (控制面)                                                  │
│     ├── master_service.h/cpp     ← 移除 Eviction 逻辑                       │
│     ├── eviction_strategy.h      ← 保留但不使用，或直接删除                  │
│     └── replica.h                ← 移除 soft_pin 相关字段                   │
│                                                                             │
│  2. Allocation Strategy (放置策略)                                           │
│     ├── allocation_strategy.h    ← 新增 FlatAllocationStrategy              │
│     └── flat_memory_manager.h    ← 新增统一存储管理器                        │
│                                                                             │
│  3. Client Interface (客户端接口)                                            │
│     ├── client.h/cpp             ← 新增显式存储位置指定                      │
│     └── pybind_client.h          ← Python 接口更新                          │
│                                                                             │
│  4. HiCache Integration (SGLang/vLLM 集成)                                  │
│     └── hicache_storage.py       ← 移除 L1/L2/L3 层级概念                   │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

#### 2.2 详细修改步骤

##### Step 1: 创建 Flat Memory 类型定义

**新建文件**: `mooncake-store/include/flat_memory_types.h`

```cpp
#pragma once

#include <string>
#include <vector>
#include <optional>
#include "types.h"

namespace mooncake {

/**
 * @brief Storage medium types in Flat Memory System
 * All mediums are treated equally, no hierarchy
 */
enum class StorageMedium {
    GPU_HBM,           // GPU High Bandwidth Memory
    SYSTEM_DRAM,       // Host System DRAM
    LOCAL_SSD,         // Local NVMe SSD
    REMOTE_DRAM,       // Remote node DRAM (via RDMA)
    REMOTE_SSD,        // Remote SSD (via NVMe-oF or DFS)
    ANY                // Let system decide (round-robin or capacity-first)
};

/**
 * @brief Flat placement configuration
 * User explicitly specifies where to store data
 */
struct FlatPlacementConfig {
    StorageMedium preferred_medium{StorageMedium::ANY};
    std::vector<std::string> preferred_segments{};  // Specific segment names
    size_t replica_num{1};                          // Number of replicas
    
    // No soft_pin - all data treated equally
    // No eviction priority - no automatic data movement
};

/**
 * @brief Placement policy for Flat Memory System
 */
enum class PlacementPolicy {
    EXPLICIT,           // User explicitly specifies location
    ROUND_ROBIN,        // Distribute evenly across all mediums
    CAPACITY_FIRST,     // Place in medium with most free space
    LATENCY_FIRST,      // Prefer lower latency medium (HBM > DRAM > SSD)
    BANDWIDTH_FIRST     // Prefer higher bandwidth medium
};

/**
 * @brief Segment info with medium type
 */
struct FlatSegmentInfo {
    std::string segment_name;
    StorageMedium medium_type;
    uint64_t total_capacity;
    uint64_t used_capacity;
    uint64_t latency_ns;      // Estimated access latency
    uint64_t bandwidth_gbps;  // Estimated bandwidth
};

}  // namespace mooncake
```

##### Step 2: 创建 Flat Memory Manager

**新建文件**: `mooncake-store/include/flat_memory_manager.h`

```cpp
#pragma once

#include <memory>
#include <unordered_map>
#include <vector>
#include "flat_memory_types.h"
#include "allocation_strategy.h"

namespace mooncake {

/**
 * @brief Flat Memory Manager - Unified storage management without hierarchy
 * 
 * Key differences from original:
 * 1. No automatic data migration (eviction/write-back)
 * 2. No hot/cold classification
 * 3. User/scheduler controls data placement
 * 4. All storage mediums treated equally
 */
class FlatMemoryManager {
public:
    FlatMemoryManager() = default;
    ~FlatMemoryManager() = default;

    /**
     * @brief Register a storage segment with its medium type
     */
    ErrorCode RegisterSegment(const std::string& segment_name,
                              StorageMedium medium_type,
                              uint64_t capacity,
                              uint64_t latency_ns,
                              uint64_t bandwidth_gbps);

    /**
     * @brief Unregister a storage segment
     */
    ErrorCode UnregisterSegment(const std::string& segment_name);

    /**
     * @brief Allocate space for KVCache with explicit placement
     * @param size Size to allocate
     * @param config Placement configuration
     * @return Allocated segment name and offset, or error
     */
    tl::expected<std::pair<std::string, uint64_t>, ErrorCode>
    Allocate(size_t size, const FlatPlacementConfig& config);

    /**
     * @brief Get all segments of a specific medium type
     */
    std::vector<FlatSegmentInfo> GetSegmentsByMedium(StorageMedium medium) const;

    /**
     * @brief Get segment info
     */
    std::optional<FlatSegmentInfo> GetSegmentInfo(const std::string& name) const;

    /**
     * @brief Select segment based on placement policy
     */
    std::string SelectSegment(PlacementPolicy policy, size_t required_size);

private:
    std::unordered_map<std::string, FlatSegmentInfo> segments_;
    std::unordered_map<StorageMedium, std::vector<std::string>> medium_to_segments_;

    // Round-robin state
    size_t rr_index_{0};
};

}  // namespace mooncake
```

##### Step 3: 实现 Flat Memory Manager

**新建文件**: `mooncake-store/src/flat_memory_manager.cpp`

```cpp
#include "flat_memory_manager.h"
#include <algorithm>
#include <glog/logging.h>

namespace mooncake {

ErrorCode FlatMemoryManager::RegisterSegment(
    const std::string& segment_name,
    StorageMedium medium_type,
    uint64_t capacity,
    uint64_t latency_ns,
    uint64_t bandwidth_gbps) {
    
    if (segments_.count(segment_name)) {
        LOG(WARNING) << "Segment " << segment_name << " already registered";
        return ErrorCode::SEGMENT_ALREADY_EXISTS;
    }

    FlatSegmentInfo info{
        .segment_name = segment_name,
        .medium_type = medium_type,
        .total_capacity = capacity,
        .used_capacity = 0,
        .latency_ns = latency_ns,
        .bandwidth_gbps = bandwidth_gbps
    };

    segments_[segment_name] = info;
    medium_to_segments_[medium_type].push_back(segment_name);

    LOG(INFO) << "Registered flat segment: " << segment_name 
              << ", medium=" << static_cast<int>(medium_type)
              << ", capacity=" << capacity;

    return ErrorCode::OK;
}

ErrorCode FlatMemoryManager::UnregisterSegment(const std::string& segment_name) {
    auto it = segments_.find(segment_name);
    if (it == segments_.end()) {
        return ErrorCode::SEGMENT_NOT_FOUND;
    }

    auto medium = it->second.medium_type;
    auto& vec = medium_to_segments_[medium];
    vec.erase(std::remove(vec.begin(), vec.end(), segment_name), vec.end());
    
    segments_.erase(it);
    return ErrorCode::OK;
}

tl::expected<std::pair<std::string, uint64_t>, ErrorCode>
FlatMemoryManager::Allocate(size_t size, const FlatPlacementConfig& config) {
    
    std::vector<std::string> candidates;

    // Step 1: Determine candidate segments
    if (!config.preferred_segments.empty()) {
        // User explicitly specified segments
        candidates = config.preferred_segments;
    } else if (config.preferred_medium != StorageMedium::ANY) {
        // User specified medium type
        auto it = medium_to_segments_.find(config.preferred_medium);
        if (it != medium_to_segments_.end()) {
            candidates = it->second;
        }
    } else {
        // All segments are candidates
        for (const auto& [name, _] : segments_) {
            candidates.push_back(name);
        }
    }

    // Step 2: Find segment with enough space
    for (const auto& seg_name : candidates) {
        auto it = segments_.find(seg_name);
        if (it == segments_.end()) continue;

        auto& info = it->second;
        if (info.total_capacity - info.used_capacity >= size) {
            uint64_t offset = info.used_capacity;
            info.used_capacity += size;
            return std::make_pair(seg_name, offset);
        }
    }

    return tl::make_unexpected(ErrorCode::NO_AVAILABLE_HANDLE);
}

std::string FlatMemoryManager::SelectSegment(PlacementPolicy policy, size_t required_size) {
    std::vector<std::string> available;
    
    for (const auto& [name, info] : segments_) {
        if (info.total_capacity - info.used_capacity >= required_size) {
            available.push_back(name);
        }
    }

    if (available.empty()) return "";

    switch (policy) {
        case PlacementPolicy::ROUND_ROBIN: {
            rr_index_ = (rr_index_ + 1) % available.size();
            return available[rr_index_];
        }
        
        case PlacementPolicy::CAPACITY_FIRST: {
            return *std::max_element(available.begin(), available.end(),
                [this](const std::string& a, const std::string& b) {
                    return (segments_[a].total_capacity - segments_[a].used_capacity) <
                           (segments_[b].total_capacity - segments_[b].used_capacity);
                });
        }
        
        case PlacementPolicy::LATENCY_FIRST: {
            return *std::min_element(available.begin(), available.end(),
                [this](const std::string& a, const std::string& b) {
                    return segments_[a].latency_ns < segments_[b].latency_ns;
                });
        }
        
        case PlacementPolicy::BANDWIDTH_FIRST: {
            return *std::max_element(available.begin(), available.end(),
                [this](const std::string& a, const std::string& b) {
                    return segments_[a].bandwidth_gbps < segments_[b].bandwidth_gbps;
                });
        }
        
        default:
            return available[0];
    }
}

std::vector<FlatSegmentInfo> FlatMemoryManager::GetSegmentsByMedium(StorageMedium medium) const {
    std::vector<FlatSegmentInfo> result;
    auto it = medium_to_segments_.find(medium);
    if (it != medium_to_segments_.end()) {
        for (const auto& name : it->second) {
            auto seg_it = segments_.find(name);
            if (seg_it != segments_.end()) {
                result.push_back(seg_it->second);
            }
        }
    }
    return result;
}

std::optional<FlatSegmentInfo> FlatMemoryManager::GetSegmentInfo(const std::string& name) const {
    auto it = segments_.find(name);
    if (it != segments_.end()) {
        return it->second;
    }
    return std::nullopt;
}

}  // namespace mooncake
```

##### Step 4: 修改 Master Service - 禁用 Eviction

**修改文件**: `mooncake-store/include/master_service.h`

```cpp
// 添加编译开关
#ifdef FLAT_MEMORY_MODE

// 在 MasterServiceConfig 中添加
struct MasterServiceConfig {
    // ... 原有字段 ...
    
    // Flat Memory Mode - disable eviction
    bool enable_flat_memory_mode{false};
};

#endif
```

**修改文件**: `mooncake-store/src/master_service.cpp`

```cpp
// 在 EvictionThreadFunc 中添加检查
void MasterService::EvictionThreadFunc() {
#ifdef FLAT_MEMORY_MODE
    if (enable_flat_memory_mode_) {
        LOG(INFO) << "Flat Memory Mode enabled, eviction thread disabled";
        return;  // 直接返回，不执行任何驱逐
    }
#endif

    // 原有驱逐逻辑...
}
```

##### Step 5: 修改 ReplicateConfig - 移除 soft_pin

**修改文件**: `mooncake-store/include/replica.h`

```cpp
struct ReplicateConfig {
    size_t replica_num{1};
    
#ifdef FLAT_MEMORY_MODE
    // Flat Memory Mode: No soft_pin, explicit placement instead
    StorageMedium preferred_medium{StorageMedium::ANY};
    PlacementPolicy placement_policy{PlacementPolicy::CAPACITY_FIRST};
#else
    // Original mode
    bool with_soft_pin{false};
#endif

    std::vector<std::string> preferred_segments{};
    std::string preferred_segment{};  // Deprecated
    bool prefer_alloc_in_same_node{false};
};
```

##### Step 6: 新增 Python 接口

**修改文件**: `mooncake-store/include/pybind_client.h`

```cpp
// 添加 Flat Memory API
#ifdef FLAT_MEMORY_MODE

/**
 * @brief Put with explicit storage medium
 */
int put_to_medium(const std::string& key, 
                  const py::bytes& value,
                  int medium_type,  // 0=HBM, 1=DRAM, 2=SSD, 3=REMOTE_DRAM, 4=REMOTE_SSD
                  int replica_num = 1);

/**
 * @brief Get storage location info for a key
 */
py::dict get_storage_location(const std::string& key);

/**
 * @brief List all segments by medium type
 */
py::list list_segments_by_medium(int medium_type);

#endif
```

##### Step 7: 修改 CMakeLists.txt

**修改文件**: `mooncake-store/CMakeLists.txt`

```cmake
# 添加 Flat Memory Mode 编译选项
option(FLAT_MEMORY_MODE "Enable Flat Memory Mode (disable automatic tiering)" OFF)

if(FLAT_MEMORY_MODE)
    add_definitions(-DFLAT_MEMORY_MODE)
    message(STATUS "Flat Memory Mode enabled")
endif()

# 添加新文件
set(MOONCAKE_STORE_SOURCES
    # ... 原有文件 ...
    src/flat_memory_manager.cpp
)
```

---

### 三、对 TTFT 的影响分析

#### 3.1 TTFT 会变长吗？

**答案：取决于数据放置策略，大多数情况下会变长。**

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                      TTFT 影响分析                                           │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  原有分层架构 (有冷热分层):                                                   │
│  ┌─────────────────────────────────────────────────────────────────────┐   │
│  │  热数据 → GPU HBM (纳秒级) → TTFT 最优                               │   │
│  │  温数据 → Host DRAM (10-100ns) → TTFT 略增                          │   │
│  │  冷数据 → SSD/DFS (微秒级) → 需要预取，TTFT 增加                     │   │
│  │                                                                     │   │
│  │  关键点：系统自动将热数据放在 HBM，保证 TTFT                         │   │
│  └─────────────────────────────────────────────────────────────────────┘   │
│                                                                             │
│  Flat Memory 架构 (无冷热分层):                                              │
│  ┌─────────────────────────────────────────────────────────────────────┐   │
│  │  所有数据 → 可能在 HBM/DRAM/SSD 任意位置                             │   │
│  │                                                                     │   │
│  │  场景 1: 数据恰好在 HBM → TTFT 与原架构相同                          │   │
│  │  场景 2: 数据在 DRAM → TTFT 增加 ~100ns                             │   │
│  │  场景 3: 数据在 SSD → TTFT 增加 ~10μs (需要加载)                     │   │
│  │  场景 4: 数据在远程 → TTFT 增加网络延迟                              │   │
│  │                                                                     │   │
│  │  关键问题：无法保证热数据在 HBM，TTFT 可能显著增加                    │   │
│  └─────────────────────────────────────────────────────────────────────┘   │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

#### 3.2 各存储介质的延迟对比

| 存储介质 | 访问延迟 | 带宽 | 对 TTFT 的影响 |
|---------|---------|------|----------------|
| **GPU HBM** | ~10 ns | ~3 TB/s | 基准（最优） |
| **Host DRAM** | ~100 ns | ~200 GB/s | +10-100 ns |
| **Local NVMe SSD** | ~10 μs | ~7 GB/s | +10-100 μs |
| **Remote DRAM (RDMA)** | ~2 μs | ~200 Gbps | +2-10 μs |
| **Remote SSD (NVMe-oF)** | ~50 μs | ~100 Gbps | +50-200 μs |

#### 3.3 什么情况下 Flat Memory 更有优势？

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                   Flat Memory 的优势场景                                     │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  ✅ 适合 Flat Memory 的场景:                                                 │
│                                                                             │
│  1. 所有数据同等重要，无明显冷热区分                                          │
│     • 例如：批处理推理，所有请求的 KVCache 同等重要                           │
│                                                                             │
│  2. 外部调度器已经做好数据放置决策                                            │
│     • 例如：上层系统已知哪些数据会被访问，提前放到 HBM                         │
│                                                                             │
│  3. 追求更大的存储容量而非极致延迟                                            │
│     • 例如：超长上下文场景，需要存储大量 KVCache                              │
│                                                                             │
│  4. 避免数据迁移开销                                                         │
│     • 原架构的 Eviction/Write-back 有 CPU 和带宽开销                         │
│     • Flat Memory 没有自动迁移，减少了后台开销                                │
│                                                                             │
│  ❌ 不适合 Flat Memory 的场景:                                               │
│                                                                             │
│  1. 明显的热点数据（如系统提示）                                              │
│     • 原架构会自动将热数据放在 HBM                                           │
│                                                                             │
│  2. 对 TTFT 极其敏感的实时推理                                               │
│     • Flat Memory 可能导致热数据在慢速存储                                    │
│                                                                             │
│  3. 多租户共享，访问模式不可预测                                              │
│     • 无法预知哪些数据会被频繁访问                                           │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

### 四、优化建议：混合策略

为了避免 TTFT 显著增加，建议实现一个 **混合策略**：

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                      混合策略：Flat Memory + 智能预取                         │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│         ┌──────────────────┐                                               │
│         │    GPU HBM       │  ← 始终预留一部分作为"热缓存"                    │
│         │  (Working Set)   │                                               │
│         └────────┬─────────┘                                               │
│                  │                                                         │
│                  ▼                                                         │
│  ┌───────────────────────────────────────┐                                 │
│  │     KVCache Prefix Parser             │  ← 解析请求，预测需要的数据       │
│  │  (Predict which KV will be needed)    │                                 │
│  └───────────────────┬───────────────────┘                                 │
│                      │                                                     │
│          ┌───────────┴───────────┐                                         │
│          │   Prefetch Manager    │  ← 异步预取到 HBM                        │
│          │ (Async load to HBM)   │                                         │
│          └───────────┬───────────┘                                         │
│                      │                                                     │
│                      ▼                                                     │
│  ┌─────────────────────────────────────────────────────────┐               │
│  │              Flat Memory Pool                            │               │
│  │  ┌──────────┐  ┌──────────┐  ┌──────────────────────┐   │               │
│  │  │  DRAM    │  │   SSD    │  │  Remote Storage      │   │               │
│  │  └──────────┘  └──────────┘  └──────────────────────┘   │               │
│  └─────────────────────────────────────────────────────────┘               │
│                                                                             │
│  关键改进:                                                                   │
│  1. HBM 预留一部分作为 Working Set，存放即将使用的数据                        │
│  2. Prefix Parser 预测哪些 KVCache 会被用到                                  │
│  3. Prefetch Manager 异步将数据从 Flat Pool 加载到 HBM                       │
│  4. 保持 Flat Memory 的简单性，同时优化 TTFT                                 │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

#### 4.1 实现 Prefetch Manager

```cpp
/**
 * @brief Prefetch Manager for Flat Memory System
 * Async load data from slow storage to HBM before computation
 */
class FlatPrefetchManager {
public:
    /**
     * @brief Hint that keys will be needed soon
     * Triggers async prefetch to HBM
     */
    void PrefetchHint(const std::vector<std::string>& keys);

    /**
     * @brief Wait for prefetch to complete
     * @param timeout_ms Timeout in milliseconds
     * @return Number of keys successfully prefetched
     */
    size_t WaitPrefetch(uint64_t timeout_ms);

    /**
     * @brief Get prefetch status
     */
    struct PrefetchStatus {
        size_t pending;
        size_t completed;
        size_t failed;
    };
    PrefetchStatus GetStatus() const;

private:
    // HBM cache for prefetched data
    std::unique_ptr<HBMCache> hbm_cache_;
    
    // Async prefetch queue
    std::queue<std::string> prefetch_queue_;
    std::thread prefetch_thread_;
};
```

---

### 五、使用示例

#### 5.1 Python 使用示例

```python
from mooncake.store import MooncakeDistributedStore, StorageMedium, PlacementPolicy

# 初始化 Flat Memory Store
store = MooncakeDistributedStore(flat_memory_mode=True)
store.setup(
    local_hostname="node1",
    metadata_server="etcd://10.0.0.1:2379",
    global_segment_size=64 * 1024**3,  # 64GB
    protocol="rdma",
    device_name="mlx5_0",
    master_server_address="10.0.0.1:50051"
)

# 方式 1: 显式指定存储介质
store.put_to_medium(
    key="system_prompt_kv",
    value=kv_cache_data,
    medium=StorageMedium.SYSTEM_DRAM,  # 显式指定存在 DRAM
    replica_num=2
)

# 方式 2: 使用放置策略
store.put_with_policy(
    key="user_context_kv",
    value=kv_cache_data,
    policy=PlacementPolicy.CAPACITY_FIRST  # 放在容量最大的介质
)

# 方式 3: 显式指定 segment
store.put_to_segment(
    key="important_kv",
    value=kv_cache_data,
    segment_name="node2_dram_0"  # 直接指定 segment
)

# 查询数据存储位置
location = store.get_storage_location("system_prompt_kv")
print(f"Key stored in: {location['segment']}, medium: {location['medium']}")

# 预取数据到 HBM (优化 TTFT)
store.prefetch_to_hbm(["key1", "key2", "key3"])
store.wait_prefetch(timeout_ms=100)

# 获取数据
data = store.get("system_prompt_kv")
```

#### 5.2 配置文件示例

```json
{
    "flat_memory_mode": true,
    "default_placement_policy": "capacity_first",
    "segments": [
        {
            "name": "gpu0_hbm",
            "medium": "GPU_HBM",
            "capacity_gb": 40,
            "latency_ns": 10,
            "bandwidth_gbps": 3000
        },
        {
            "name": "host_dram",
            "medium": "SYSTEM_DRAM", 
            "capacity_gb": 512,
            "latency_ns": 100,
            "bandwidth_gbps": 200
        },
        {
            "name": "local_ssd",
            "medium": "LOCAL_SSD",
            "capacity_gb": 2000,
            "latency_ns": 10000,
            "bandwidth_gbps": 7
        }
    ],
    "prefetch": {
        "enabled": true,
        "hbm_cache_size_gb": 10,
        "prefetch_threshold_tokens": 256
    }
}
```

---

### 六、总结

#### 6.1 修改清单

| 文件 | 修改类型 | 说明 |
|------|---------|------|
| `flat_memory_types.h` | 新增 | 定义存储介质类型和放置策略 |
| `flat_memory_manager.h/cpp` | 新增 | 统一存储管理器 |
| `master_service.cpp` | 修改 | 添加 Flat Memory 模式检查，禁用 Eviction |
| `replica.h` | 修改 | 移除 soft_pin，添加显式放置配置 |
| `pybind_client.h` | 修改 | 添加 Flat Memory Python API |
| `CMakeLists.txt` | 修改 | 添加编译选项 |

#### 6.2 TTFT 影响总结

| 场景 | TTFT 变化 | 原因 |
|------|----------|------|
| 数据在 HBM | 无变化 | 与原架构相同 |
| 数据在 DRAM | +100ns | 需要 CPU↔GPU 传输 |
| 数据在 SSD | +10-100μs | 需要 SSD 读取 + 传输 |
| 使用预取优化 | 接近原架构 | 异步预加载到 HBM |

#### 6.3 建议

1. **如果对 TTFT 敏感**：保持原有分层架构，或使用混合策略
2. **如果追求大容量**：使用 Flat Memory + 预取优化
3. **如果有外部调度**：使用 Flat Memory，由外部系统控制数据放置
