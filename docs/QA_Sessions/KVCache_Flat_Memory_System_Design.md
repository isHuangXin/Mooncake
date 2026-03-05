# KVCache Flat Memory System 设计文档

## 目录
1. [概述](#1-概述)
2. [原始Mooncake分层存储架构分析](#2-原始mooncake分层存储架构分析)
3. [KVCache Flat Memory System 设计理念](#3-kvcache-flat-memory-system-设计理念)
4. [核心代码修改方案](#4-核心代码修改方案)
5. [对TTFT的影响分析](#5-对ttft的影响分析)
6. [实现细节](#6-实现细节)
7. [性能权衡与优化建议](#7-性能权衡与优化建议)
8. [总结](#8-总结)

---

## 1. 概述

### 1.1 背景
Mooncake原有架构采用**分层存储策略**，将KVCache数据按照访问频率（冷热）分布在不同存储层级：
- **GPU HBM**: 存储热数据（~纳秒级访问延迟）
- **System DRAM**: 存储中间数据（~10-100纳秒）
- **Local SSD**: 存储温数据（~微秒级）
- **Shared Object/File**: 存储冷数据（~微秒级）

### 1.2 KVCache Flat Memory System 目标
将上述分层存储架构转变为**扁平化存储架构**：
- 所有存储介质（GPU HBM、DRAM、SSD）被视为**同一层级**
- **不再区分冷热数据**
- KVCache可以**任意存储**在任何存储位置
- 通过**统一的Flat Memory Interface**管理所有存储

```
┌─────────────────────────────────────────────────────────────────┐
│                        原始分层架构                               │
├─────────────────────────────────────────────────────────────────┤
│   GPU HBM        ──┐                                            │
│   (热数据)          │                                            │
│                    │   延时降低                                  │
│   System DRAM    ──┤    ↑                                       │
│   (中间数据)        │    │                                       │
│                    │    │                                       │
│   Local SSD      ──┤    ↓                                       │
│   (温数据)          │   延时增加                                  │
│                    │                                            │
│   Shared File    ──┘                                            │
│   (冷数据)                                                       │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                     Flat Memory 架构                             │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│   ┌─────────────────────────────────────────────────────────┐   │
│   │                    GPU HBM                              │   │
│   └─────────────────────────────────────────────────────────┘   │
│                              │                                  │
│                              ▼                                  │
│   ┌─────────────────────────────────────────────────────────┐   │
│   │              KVCache Prefix Parser                      │   │
│   └─────────────────────────────────────────────────────────┘   │
│                              │                                  │
│                              ▼                                  │
│   ┌─────────────────────────────────────────────────────────┐   │
│   │            Flat Memory Interface                        │   │
│   └─────────────────────────────────────────────────────────┘   │
│                    │         │         │                        │
│                    ▼         ▼         ▼                        │
│   ┌─────────────────────────────────────────────────────────┐   │
│   │            Flat Memory Management System                │   │
│   ├─────────────┬─────────────┬─────────────────────────────┤   │
│   │  System     │   Local     │     Shared Object/File      │   │
│   │   DRAM      │    SSD      │   (Remote SSD & HDD)        │   │
│   └─────────────┴─────────────┴─────────────────────────────┘   │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

---

## 2. 原始Mooncake分层存储架构分析

### 2.1 核心组件

#### 2.1.1 存储后端 (`storage_backend.h`)
```cpp
enum class StorageBackendType { 
    kFilePerKey,      // 文件按Key存储
    kBucket,          // 桶存储
    kOffsetAllocator  // 偏移分配器
};
```

#### 2.1.2 副本类型 (`replica.h`)
```cpp
enum class ReplicaType {
    MEMORY,     // 内存副本 (GPU HBM / DRAM)
    DISK,       // 磁盘副本 (SSD)
    LOCAL_DISK  // 本地磁盘副本
};
```

#### 2.1.3 驱逐策略 (`eviction_strategy.h`)
- **LRU (Least Recently Used)**: 基于访问时间
- **FIFO (First In First Out)**: 基于插入顺序

#### 2.1.4 分配策略 (`allocation_strategy.h`)
```cpp
class AllocationStrategy {
    // 根据优先级分配到不同的segment
    virtual tl::expected<std::vector<Replica>, ErrorCode> Allocate(
        const AllocatorManager& allocator_manager, 
        const size_t slice_length,
        const size_t replica_num,
        const std::vector<std::string>& preferred_segments,
        const std::set<std::string>& excluded_segments) = 0;
};
```

### 2.2 分层逻辑所在位置

1. **Master Service** (`master_service.h`): 管理segment分配和驱逐决策
2. **Client** (`real_client.h`): Put/Get操作时的层级选择
3. **File Storage** (`file_storage.h`): SSD offload逻辑
4. **Eviction Strategy**: 决定哪些数据需要从上层迁移到下层

---

## 3. KVCache Flat Memory System 设计理念

### 3.1 核心设计原则

1. **存储位置透明化**: 应用层不需要知道数据存储在哪个介质
2. **无冷热区分**: 所有数据被同等对待，不基于访问频率迁移
3. **统一地址空间**: 通过Flat Memory Interface提供统一的访问接口
4. **基于策略的放置**: 可配置的放置策略（容量优先、延迟优先等）

### 3.2 新组件设计

```
┌──────────────────────────────────────────────────────────────┐
│                  Flat Memory Manager                         │
├──────────────────────────────────────────────────────────────┤
│                                                              │
│  ┌─────────────────┐  ┌─────────────────┐  ┌──────────────┐  │
│  │ Storage Pool    │  │ Placement       │  │ Address      │  │
│  │ Registry        │  │ Policy Engine   │  │ Resolver     │  │
│  └─────────────────┘  └─────────────────┘  └──────────────┘  │
│                                                              │
│  ┌─────────────────────────────────────────────────────────┐ │
│  │              Unified Storage Backend                    │ │
│  ├─────────────────┬─────────────────┬─────────────────────┤ │
│  │ HBM Segment     │ DRAM Segment    │ SSD Segment         │ │
│  │ Allocator       │ Allocator       │ Allocator           │ │
│  └─────────────────┴─────────────────┴─────────────────────┘ │
│                                                              │
└──────────────────────────────────────────────────────────────┘
```

---

## 4. 核心代码修改方案

### 4.1 新增文件

#### 4.1.1 `flat_memory_types.h` - 类型定义
```cpp
// mooncake-store/include/flat_memory_types.h
#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <variant>

namespace mooncake {
namespace flat {

/**
 * @brief 存储介质类型（扁平化，不区分层级）
 */
enum class StorageMedium {
    GPU_HBM,        // GPU高带宽内存
    SYSTEM_DRAM,    // 系统内存
    LOCAL_SSD,      // 本地SSD
    REMOTE_STORAGE  // 远程存储
};

/**
 * @brief 放置策略类型
 */
enum class PlacementPolicy {
    CAPACITY_FIRST,     // 容量优先：优先使用容量大的存储
    LATENCY_FIRST,      // 延迟优先：优先使用延迟低的存储
    ROUND_ROBIN,        // 轮询：均匀分布
    RANDOM,             // 随机放置
    LOCALITY_AWARE      // 位置感知：根据数据亲和性放置
};

/**
 * @brief 扁平化存储段描述符
 */
struct FlatSegmentDescriptor {
    std::string segment_id;
    StorageMedium medium;
    uint64_t base_address;
    size_t capacity;
    size_t used;
    std::string transport_endpoint;
    
    // 介质特性（用于策略决策，但不用于层级划分）
    uint64_t estimated_latency_ns;  // 估计访问延迟
    uint64_t bandwidth_gbps;        // 带宽
    
    bool isAvailable() const { return (capacity - used) > 0; }
    size_t availableSpace() const { return capacity - used; }
};

/**
 * @brief 存储位置（扁平化）
 */
struct FlatStorageLocation {
    std::string segment_id;
    StorageMedium medium;
    uint64_t offset;
    size_t size;
    std::string transport_endpoint;
};

/**
 * @brief 放置配置（替代原有的ReplicateConfig）
 */
struct FlatPlacementConfig {
    PlacementPolicy policy = PlacementPolicy::CAPACITY_FIRST;
    size_t replica_num = 1;
    std::vector<StorageMedium> preferred_mediums;  // 可选的首选介质
    std::vector<std::string> preferred_segments;    // 可选的首选segment
    bool allow_any_medium = true;                   // 是否允许任意介质
    
    // 不再有热度相关的配置
    // 移除: bool with_soft_pin
    // 移除: 冷热数据标记
};

}  // namespace flat
}  // namespace mooncake
```

#### 4.1.2 `flat_memory_manager.h` - 扁平内存管理器
```cpp
// mooncake-store/include/flat_memory_manager.h
#pragma once

#include <memory>
#include <unordered_map>
#include <vector>
#include <shared_mutex>

#include "flat_memory_types.h"
#include "types.h"
#include "allocator.h"

namespace mooncake {
namespace flat {

/**
 * @brief 扁平内存管理器 - 核心组件
 * 
 * 不区分存储层级，将所有存储介质视为统一的存储池
 */
class FlatMemoryManager {
public:
    FlatMemoryManager() = default;
    ~FlatMemoryManager() = default;
    
    /**
     * @brief 注册存储段（不区分类型，统一管理）
     */
    ErrorCode RegisterSegment(const FlatSegmentDescriptor& descriptor);
    
    /**
     * @brief 注销存储段
     */
    ErrorCode UnregisterSegment(const std::string& segment_id);
    
    /**
     * @brief 分配存储空间（基于放置策略，不基于冷热）
     * 
     * @param size 请求的大小
     * @param config 放置配置
     * @return 分配的存储位置
     */
    tl::expected<FlatStorageLocation, ErrorCode> Allocate(
        size_t size,
        const FlatPlacementConfig& config = FlatPlacementConfig{});
    
    /**
     * @brief 批量分配
     */
    tl::expected<std::vector<FlatStorageLocation>, ErrorCode> AllocateBatch(
        const std::vector<size_t>& sizes,
        const FlatPlacementConfig& config = FlatPlacementConfig{});
    
    /**
     * @brief 释放存储空间
     */
    ErrorCode Deallocate(const FlatStorageLocation& location);
    
    /**
     * @brief 获取所有可用段（不分层）
     */
    std::vector<FlatSegmentDescriptor> GetAllSegments() const;
    
    /**
     * @brief 获取总容量统计（跨所有介质）
     */
    struct CapacityStats {
        size_t total_capacity;
        size_t total_used;
        std::unordered_map<StorageMedium, size_t> capacity_by_medium;
        std::unordered_map<StorageMedium, size_t> used_by_medium;
    };
    CapacityStats GetCapacityStats() const;

private:
    /**
     * @brief 基于策略选择存储段
     */
    tl::expected<std::string, ErrorCode> SelectSegment(
        size_t required_size,
        const FlatPlacementConfig& config);
    
    /**
     * @brief 容量优先选择算法
     */
    std::string SelectByCapacity(size_t required_size);
    
    /**
     * @brief 延迟优先选择算法
     */
    std::string SelectByLatency(size_t required_size);
    
    /**
     * @brief 轮询选择算法
     */
    std::string SelectByRoundRobin(size_t required_size);
    
    /**
     * @brief 随机选择算法
     */
    std::string SelectRandom(size_t required_size);

private:
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, FlatSegmentDescriptor> segments_;
    std::unordered_map<std::string, std::shared_ptr<BufferAllocatorBase>> allocators_;
    
    // 轮询索引
    std::atomic<size_t> round_robin_index_{0};
};

}  // namespace flat
}  // namespace mooncake
```

#### 4.1.3 `flat_memory_manager.cpp` - 实现
```cpp
// mooncake-store/src/flat_memory_manager.cpp
#include "flat_memory_manager.h"
#include <algorithm>
#include <random>
#include <glog/logging.h>

namespace mooncake {
namespace flat {

ErrorCode FlatMemoryManager::RegisterSegment(
    const FlatSegmentDescriptor& descriptor) {
    std::unique_lock lock(mutex_);
    
    if (segments_.contains(descriptor.segment_id)) {
        LOG(WARNING) << "Segment already registered: " << descriptor.segment_id;
        return ErrorCode::SEGMENT_ALREADY_EXISTS;
    }
    
    segments_[descriptor.segment_id] = descriptor;
    
    LOG(INFO) << "Registered flat segment: " << descriptor.segment_id
              << ", medium: " << static_cast<int>(descriptor.medium)
              << ", capacity: " << descriptor.capacity;
    
    return ErrorCode::OK;
}

ErrorCode FlatMemoryManager::UnregisterSegment(const std::string& segment_id) {
    std::unique_lock lock(mutex_);
    
    auto it = segments_.find(segment_id);
    if (it == segments_.end()) {
        return ErrorCode::SEGMENT_NOT_FOUND;
    }
    
    segments_.erase(it);
    allocators_.erase(segment_id);
    
    return ErrorCode::OK;
}

tl::expected<FlatStorageLocation, ErrorCode> FlatMemoryManager::Allocate(
    size_t size,
    const FlatPlacementConfig& config) {
    
    // 选择存储段（基于策略，不基于冷热）
    auto segment_result = SelectSegment(size, config);
    if (!segment_result) {
        return tl::make_unexpected(segment_result.error());
    }
    
    std::unique_lock lock(mutex_);
    
    const std::string& segment_id = segment_result.value();
    auto& descriptor = segments_[segment_id];
    
    // 分配空间
    if (descriptor.availableSpace() < size) {
        return tl::make_unexpected(ErrorCode::NO_AVAILABLE_HANDLE);
    }
    
    FlatStorageLocation location;
    location.segment_id = segment_id;
    location.medium = descriptor.medium;
    location.offset = descriptor.used;
    location.size = size;
    location.transport_endpoint = descriptor.transport_endpoint;
    
    descriptor.used += size;
    
    return location;
}

tl::expected<std::string, ErrorCode> FlatMemoryManager::SelectSegment(
    size_t required_size,
    const FlatPlacementConfig& config) {
    
    std::shared_lock lock(mutex_);
    
    // 首先尝试首选介质/段
    if (!config.preferred_segments.empty()) {
        for (const auto& preferred : config.preferred_segments) {
            auto it = segments_.find(preferred);
            if (it != segments_.end() && it->second.availableSpace() >= required_size) {
                return preferred;
            }
        }
    }
    
    if (!config.preferred_mediums.empty()) {
        for (const auto& medium : config.preferred_mediums) {
            for (const auto& [id, desc] : segments_) {
                if (desc.medium == medium && desc.availableSpace() >= required_size) {
                    return id;
                }
            }
        }
    }
    
    // 如果不允许使用任意介质，且首选不可用，则失败
    if (!config.allow_any_medium) {
        return tl::make_unexpected(ErrorCode::SEGMENT_NOT_FOUND);
    }
    
    // 基于策略选择（核心变化：不再基于冷热）
    switch (config.policy) {
        case PlacementPolicy::CAPACITY_FIRST:
            return SelectByCapacity(required_size);
        case PlacementPolicy::LATENCY_FIRST:
            return SelectByLatency(required_size);
        case PlacementPolicy::ROUND_ROBIN:
            return SelectByRoundRobin(required_size);
        case PlacementPolicy::RANDOM:
            return SelectRandom(required_size);
        default:
            return SelectByCapacity(required_size);
    }
}

std::string FlatMemoryManager::SelectByCapacity(size_t required_size) {
    // 选择可用容量最大的段（不考虑介质类型）
    std::string best_segment;
    size_t max_available = 0;
    
    for (const auto& [id, desc] : segments_) {
        size_t available = desc.availableSpace();
        if (available >= required_size && available > max_available) {
            max_available = available;
            best_segment = id;
        }
    }
    
    if (best_segment.empty()) {
        return "";  // Will be handled as error
    }
    return best_segment;
}

std::string FlatMemoryManager::SelectByLatency(size_t required_size) {
    // 选择延迟最低的段（但仍然是扁平化选择，不强制层级）
    std::string best_segment;
    uint64_t min_latency = UINT64_MAX;
    
    for (const auto& [id, desc] : segments_) {
        if (desc.availableSpace() >= required_size && 
            desc.estimated_latency_ns < min_latency) {
            min_latency = desc.estimated_latency_ns;
            best_segment = id;
        }
    }
    
    return best_segment;
}

std::string FlatMemoryManager::SelectByRoundRobin(size_t required_size) {
    std::vector<std::string> available;
    
    for (const auto& [id, desc] : segments_) {
        if (desc.availableSpace() >= required_size) {
            available.push_back(id);
        }
    }
    
    if (available.empty()) {
        return "";
    }
    
    size_t index = round_robin_index_.fetch_add(1) % available.size();
    return available[index];
}

std::string FlatMemoryManager::SelectRandom(size_t required_size) {
    std::vector<std::string> available;
    
    for (const auto& [id, desc] : segments_) {
        if (desc.availableSpace() >= required_size) {
            available.push_back(id);
        }
    }
    
    if (available.empty()) {
        return "";
    }
    
    static std::random_device rd;
    static std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, available.size() - 1);
    
    return available[dis(gen)];
}

FlatMemoryManager::CapacityStats FlatMemoryManager::GetCapacityStats() const {
    std::shared_lock lock(mutex_);
    
    CapacityStats stats{};
    
    for (const auto& [id, desc] : segments_) {
        stats.total_capacity += desc.capacity;
        stats.total_used += desc.used;
        stats.capacity_by_medium[desc.medium] += desc.capacity;
        stats.used_by_medium[desc.medium] += desc.used;
    }
    
    return stats;
}

}  // namespace flat
}  // namespace mooncake
```

### 4.2 修改现有文件

#### 4.2.1 修改 `eviction_strategy.h` - 禁用驱逐
```cpp
// 新增: NoEvictionStrategy（扁平化系统不需要驱逐）
class NoEvictionStrategy : public EvictionStrategy {
public:
    virtual ErrorCode AddKey(const std::string& key) override {
        // 仅记录，不参与驱逐决策
        all_key_list_.push_front(key);
        all_key_idx_map_[key] = all_key_list_.begin();
        return ErrorCode::OK;
    }
    
    virtual ErrorCode UpdateKey(const std::string& key) override {
        // 无操作 - 扁平化系统不跟踪访问热度
        return ErrorCode::OK;
    }
    
    virtual std::string EvictKey(void) override {
        // 扁平化系统不主动驱逐
        // 只有在空间真正不足时才会删除
        return "";
    }
};
```

#### 4.2.2 修改 `allocation_strategy.h` - 扁平化分配
```cpp
// 新增: FlatAllocationStrategy
class FlatAllocationStrategy : public AllocationStrategy {
public:
    explicit FlatAllocationStrategy(
        std::shared_ptr<flat::FlatMemoryManager> flat_manager)
        : flat_manager_(flat_manager) {}
    
    tl::expected<std::vector<Replica>, ErrorCode> Allocate(
        const AllocatorManager& allocator_manager,
        const size_t slice_length,
        const size_t replica_num,
        const std::vector<std::string>& preferred_segments,
        const std::set<std::string>& excluded_segments) override {
        
        // 使用扁平化管理器分配，不区分存储层级
        flat::FlatPlacementConfig config;
        config.replica_num = replica_num;
        config.preferred_segments = preferred_segments;
        config.policy = flat::PlacementPolicy::CAPACITY_FIRST;  // 可配置
        
        auto locations = flat_manager_->AllocateBatch(
            std::vector<size_t>(replica_num, slice_length), config);
        
        if (!locations) {
            return tl::make_unexpected(locations.error());
        }
        
        // 转换为Replica格式
        std::vector<Replica> replicas;
        // ... 转换逻辑
        
        return replicas;
    }

private:
    std::shared_ptr<flat::FlatMemoryManager> flat_manager_;
};
```

#### 4.2.3 修改 `real_client.h` / `real_client.cpp` - 使用扁平化管理
```cpp
// 在 RealClient 类中添加
private:
    // 扁平化内存管理器
    std::shared_ptr<flat::FlatMemoryManager> flat_memory_manager_;
    bool use_flat_memory_ = false;  // 配置开关

public:
    /**
     * @brief 启用扁平化内存模式
     */
    void EnableFlatMemoryMode() {
        use_flat_memory_ = true;
        flat_memory_manager_ = std::make_shared<flat::FlatMemoryManager>();
    }
    
    /**
     * @brief Put操作（扁平化版本）
     */
    tl::expected<void, ErrorCode> put_flat(
        const std::string& key, 
        std::span<const char> value,
        const flat::FlatPlacementConfig& config);
```

#### 4.2.4 修改 `file_storage.cpp` - 移除offload逻辑
```cpp
// 在扁平化模式下，移除自动offload
tl::expected<void, ErrorCode> FileStorage::Heartbeat() {
    if (use_flat_memory_mode_) {
        // 扁平化模式：不进行自动offload
        // 仅同步元数据
        return SyncMetadata();
    }
    
    // 原有逻辑（分层模式）
    // ...
}
```

### 4.3 配置修改

#### 4.3.1 新增配置项
```cpp
// config.h 或 master_config.h
struct FlatMemoryConfig {
    bool enabled = false;                                    // 是否启用扁平化模式
    flat::PlacementPolicy default_policy = 
        flat::PlacementPolicy::CAPACITY_FIRST;              // 默认放置策略
    bool disable_auto_migration = true;                      // 禁用自动迁移
    bool disable_eviction = true;                           // 禁用驱逐
    std::vector<flat::StorageMedium> available_mediums;     // 可用存储介质
};
```

#### 4.3.2 环境变量支持
```cpp
// 新增环境变量
// MOONCAKE_FLAT_MEMORY_ENABLED=true
// MOONCAKE_FLAT_PLACEMENT_POLICY=capacity_first|latency_first|round_robin|random
// MOONCAKE_DISABLE_AUTO_MIGRATION=true
// MOONCAKE_DISABLE_EVICTION=true
```

---

## 5. 对TTFT的影响分析

### 5.1 什么是TTFT

**TTFT (Time To First Token)** 是LLM推理中的关键指标，表示从用户发送请求到生成第一个token的时间。

```
用户请求 ──> Prefill阶段 ──> 第一个Token
         │<───── TTFT ─────>│
```

### 5.2 分层架构对TTFT的优化

原始Mooncake分层架构通过以下方式优化TTFT：

1. **热数据在HBM**: 高频访问的KVCache保持在GPU HBM，访问延迟极低
2. **预取机制**: 从L2/L3预取数据到GPU
3. **冷热分离**: 冷数据不占用宝贵的HBM空间

### 5.3 扁平化架构对TTFT的影响

#### 5.3.1 **可能导致TTFT增长的因素**

| 因素 | 原因 | 影响程度 |
|------|------|---------|
| **数据位置不确定** | KVCache可能存储在SSD，访问延迟从纳秒变为微秒 | ⚠️ 高 |
| **缺少热点优化** | 没有将热数据优先放在HBM | ⚠️ 高 |
| **I/O路径变长** | 可能需要从多种介质读取 | ⚠️ 中 |
| **缺少预取指导** | 不知道哪些数据"热"，难以预取 | ⚠️ 中 |

#### 5.3.2 **可能减少TTFT的因素**

| 因素 | 原因 | 影响程度 |
|------|------|---------|
| **管理开销减少** | 没有冷热判断和迁移开销 | ✅ 低 |
| **更大的有效存储** | 所有存储都可用于KVCache | ✅ 中 |
| **简化的代码路径** | 统一接口，减少分支判断 | ✅ 低 |

#### 5.3.3 **定量分析**

假设场景：
- GPU HBM访问延迟: 10ns
- DRAM访问延迟: 100ns
- SSD访问延迟: 10µs
- KVCache大小: 1GB

**分层架构（80%热数据在HBM）**:
```
TTFT_hierarchical = 0.8 * T_hbm + 0.15 * T_dram + 0.05 * T_ssd
                  = 0.8 * 10ns + 0.15 * 100ns + 0.05 * 10µs
                  = 8ns + 15ns + 500ns
                  = ~523ns
```

**扁平架构（均匀分布）**:
```
TTFT_flat = 0.33 * T_hbm + 0.33 * T_dram + 0.33 * T_ssd
          = 0.33 * 10ns + 0.33 * 100ns + 0.33 * 10µs
          = 3.3ns + 33ns + 3300ns
          = ~3336ns
```

**结论**: 在最坏情况下，**TTFT可能增加约6.4倍**。

### 5.4 缓解策略

#### 5.4.1 延迟优先放置策略
```cpp
config.policy = PlacementPolicy::LATENCY_FIRST;
```
优先使用低延迟存储，但不强制分层。

#### 5.4.2 智能预取
```cpp
// 基于访问模式预取，而非冷热
class FlatPrefetcher {
    void PrefetchByPattern(const std::string& prefix);
    void PrefetchByLocality(const std::string& key);
};
```

#### 5.4.3 混合模式
```cpp
// 对TTFT敏感的场景使用延迟优先
config.policy = is_prefill ? PlacementPolicy::LATENCY_FIRST 
                           : PlacementPolicy::CAPACITY_FIRST;
```

---

## 6. 实现细节

### 6.1 关键接口变更

#### 6.1.1 Put接口
```cpp
// 旧接口（分层）
int put(const std::string& key, std::span<const char> value,
        const ReplicateConfig& config);

// 新接口（扁平化）
int put_flat(const std::string& key, std::span<const char> value,
             const flat::FlatPlacementConfig& config);
```

#### 6.1.2 Get接口
```cpp
// 旧接口
int64_t get_into(const std::string& key, void* buffer, size_t size);

// 新接口（无变化，但底层实现变化）
int64_t get_into_flat(const std::string& key, void* buffer, size_t size);
```

### 6.2 数据结构变更

#### 6.2.1 元数据存储
```cpp
// 旧结构
struct ObjectMetadata {
    ReplicaType type;      // MEMORY/DISK/LOCAL_DISK
    ReplicaStatus status;
    // 热度信息
    uint64_t access_count;
    uint64_t last_access_time;
};

// 新结构
struct FlatObjectMetadata {
    flat::StorageMedium medium;  // 仅用于I/O路径选择
    std::string segment_id;
    uint64_t offset;
    size_t size;
    // 无热度信息
};
```

### 6.3 迁移流程

```
┌─────────────────────────────────────────────────────────────┐
│                    迁移步骤                                  │
├─────────────────────────────────────────────────────────────┤
│ Step 1: 配置启用扁平化模式                                     │
│         MOONCAKE_FLAT_MEMORY_ENABLED=true                   │
│                                                             │
│ Step 2: 禁用自动驱逐和迁移                                     │
│         MOONCAKE_DISABLE_AUTO_MIGRATION=true                │
│         MOONCAKE_DISABLE_EVICTION=true                      │
│                                                             │
│ Step 3: 使用新的扁平化API                                     │
│         client.EnableFlatMemoryMode()                       │
│         client.put_flat(key, value, flat_config)            │
│                                                             │
│ Step 4: 监控TTFT变化                                         │
│         如需优化，调整PlacementPolicy                         │
└─────────────────────────────────────────────────────────────┘
```

---

## 7. 性能权衡与优化建议

### 7.1 性能权衡表

| 指标 | 分层架构 | 扁平架构 | 说明 |
|------|---------|---------|------|
| TTFT | ⭐⭐⭐⭐⭐ | ⭐⭐⭐ | 分层架构通过热点优化TTFT更好 |
| 吞吐量 | ⭐⭐⭐⭐ | ⭐⭐⭐⭐⭐ | 扁平架构减少迁移开销 |
| 存储利用率 | ⭐⭐⭐ | ⭐⭐⭐⭐⭐ | 扁平架构无层级限制 |
| 实现复杂度 | ⭐⭐⭐ | ⭐⭐⭐⭐⭐ | 扁平架构更简单 |
| 管理开销 | ⭐⭐⭐ | ⭐⭐⭐⭐⭐ | 无冷热追踪和迁移 |

### 7.2 优化建议

1. **场景感知放置**
   ```cpp
   // 对延迟敏感的操作使用LATENCY_FIRST
   // 对容量敏感的操作使用CAPACITY_FIRST
   ```

2. **预取机制保留**
   - 虽然不区分冷热，但仍可基于访问模式预取

3. **监控与调优**
   - 监控不同介质的访问延迟
   - 动态调整放置策略

4. **混合模式选项**
   - 提供从扁平化到分层的渐进式配置

### 7.3 适用场景

**扁平化架构更适合**:
- 存储容量是主要瓶颈
- 访问模式均匀，无明显热点
- 需要简化运维
- 对TTFT不太敏感的批处理场景

**分层架构更适合**:
- TTFT是关键指标
- 明显的访问热点
- GPU HBM资源充足
- 实时推理场景

---

## 8. 总结

### 8.1 主要修改点

1. **新增组件**:
   - `FlatMemoryManager`: 扁平内存管理器
   - `FlatPlacementConfig`: 扁平化放置配置
   - `NoEvictionStrategy`: 无驱逐策略

2. **修改组件**:
   - `EvictionStrategy`: 添加扁平化模式支持
   - `AllocationStrategy`: 添加扁平化分配策略
   - `RealClient`: 添加扁平化API

3. **配置变更**:
   - 新增环境变量支持扁平化模式
   - 新增放置策略配置

### 8.2 TTFT影响结论

**扁平化架构会导致TTFT增长**，原因：
- 数据可能存储在高延迟介质
- 缺少热点优化机制
- 无法保证关键数据在HBM

**缓解措施**:
- 使用`LATENCY_FIRST`放置策略
- 实现智能预取机制
- 提供混合模式配置

### 8.3 实施建议

1. **渐进式迁移**: 先在测试环境验证
2. **监控关键指标**: 特别关注TTFT变化
3. **保留回退路径**: 可快速切回分层架构
4. **场景化配置**: 根据工作负载选择策略

---

## 附录

### A. 代码目录结构
```
mooncake-store/
├── include/
│   ├── flat_memory_types.h      # 新增
│   ├── flat_memory_manager.h    # 新增
│   ├── eviction_strategy.h      # 修改
│   ├── allocation_strategy.h    # 修改
│   └── real_client.h            # 修改
├── src/
│   ├── flat_memory_manager.cpp  # 新增
│   ├── real_client.cpp          # 修改
│   └── file_storage.cpp         # 修改
└── conf/
    └── flat_memory.json         # 新增配置
```

### B. 完整代码补丁

完整的代码修改可通过以下步骤获取：
1. 应用本文档中的新增文件
2. 按照修改说明更新现有文件
3. 更新CMakeLists.txt包含新文件
