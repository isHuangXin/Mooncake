# KVCache Flat Memory System 实现指南

## 简介

本文档详细说明如何基于Mooncake实现KVCache Flat Memory System。这个系统的核心思想是将原有的分层存储架构（GPU HBM → DRAM → SSD）转变为扁平化存储架构，不再区分数据冷热。

## 目录

1. [架构对比](#1-架构对比)
2. [核心修改点](#2-核心修改点)
3. [新增代码文件说明](#3-新增代码文件说明)
4. [修改现有代码](#4-修改现有代码)
5. [TTFT影响分析](#5-ttft影响分析)
6. [部署与配置](#6-部署与配置)
7. [最佳实践](#7-最佳实践)

---

## 1. 架构对比

### 1.1 原始Mooncake分层架构

```
┌────────────────────────────────────────────────┐
│                   应用层                        │
└────────────────────────────────────────────────┘
                        │
                        ▼
┌────────────────────────────────────────────────┐
│            Mooncake Store Client               │
│  ┌──────────────────────────────────────────┐  │
│  │         热度追踪 & 驱逐策略                 │  │
│  │   (LRU/FIFO Eviction Strategy)           │  │
│  └──────────────────────────────────────────┘  │
└────────────────────────────────────────────────┘
         │              │              │
         ▼              ▼              ▼
┌──────────────┐ ┌──────────────┐ ┌──────────────┐
│   GPU HBM    │ │ System DRAM  │ │  Local SSD   │
│   (热数据)    │ │  (温数据)     │ │   (冷数据)    │
│   ~10ns      │ │  ~100ns      │ │   ~10μs      │
└──────────────┘ └──────────────┘ └──────────────┘
```

**特点：**
- 热数据优先存放在GPU HBM
- 冷数据自动offload到SSD
- 基于访问频率的驱逐策略
- TTFT优化好（热数据快速访问）

### 1.2 新的Flat Memory架构

```
┌────────────────────────────────────────────────┐
│                   应用层                        │
└────────────────────────────────────────────────┘
                        │
                        ▼
┌────────────────────────────────────────────────┐
│           KVCache Prefix Parser                │
│   (解析KVCache键，确定存储位置)                    │
└────────────────────────────────────────────────┘
                        │
                        ▼
┌────────────────────────────────────────────────┐
│          Flat Memory Interface                 │
│  ┌──────────────────────────────────────────┐  │
│  │      FlatMemoryManager                   │  │
│  │   - 无冷热区分                             │  │
│  │   - 基于放置策略分配                        │  │
│  │   - 统一地址空间                           │  │
│  └──────────────────────────────────────────┘  │
└────────────────────────────────────────────────┘
         │              │              │
         ▼              ▼              ▼
┌────────────────────────────────────────────────┐
│       Flat Memory Management System            │
│  ┌────────────┐ ┌────────────┐ ┌────────────┐  │
│  │  GPU HBM   │ │   DRAM     │ │    SSD     │  │
│  │  (等价)     │ │  (等价)    │ │   (等价)    │  │
│  └────────────┘ └────────────┘ └────────────┘  │
└────────────────────────────────────────────────┘
```

**特点：**
- 所有存储介质等价对待
- 不追踪数据冷热
- 基于放置策略（容量/延迟/轮询/随机）分配
- 简化管理，最大化存储利用率

---

## 2. 核心修改点

### 2.1 需要新增的文件

| 文件路径 | 说明 |
|---------|------|
| `include/flat_memory_types.h` | 扁平化存储类型定义 |
| `include/flat_memory_manager.h` | 扁平内存管理器接口 |
| `src/flat_memory_manager.cpp` | 扁平内存管理器实现 |
| `examples/flat_memory_example.cpp` | 使用示例 |

### 2.2 需要修改的文件

| 文件路径 | 修改内容 |
|---------|---------|
| `include/eviction_strategy.h` | 添加NoEvictionStrategy |
| `include/allocation_strategy.h` | 添加FlatAllocationStrategy |
| `include/real_client.h` | 添加扁平化API |
| `src/real_client.cpp` | 实现扁平化API |
| `src/file_storage.cpp` | 禁用自动offload |
| `CMakeLists.txt` | 添加新文件编译 |

---

## 3. 新增代码文件说明

### 3.1 `flat_memory_types.h`

这是类型定义文件，定义了扁平化存储系统的核心类型：

```cpp
// 存储介质类型（扁平化，不区分层级）
enum class StorageMedium {
    GPU_HBM,        // GPU高带宽内存
    SYSTEM_DRAM,    // 系统内存
    LOCAL_SSD,      // 本地SSD
    REMOTE_STORAGE  // 远程存储
};

// 放置策略类型（替代冷热分层）
enum class PlacementPolicy {
    CAPACITY_FIRST,     // 容量优先
    LATENCY_FIRST,      // 延迟优先
    ROUND_ROBIN,        // 轮询
    RANDOM,             // 随机
    LOCALITY_AWARE      // 位置感知
};

// 放置配置（替代ReplicateConfig）
struct FlatPlacementConfig {
    PlacementPolicy policy;
    size_t replica_num;
    std::vector<StorageMedium> preferred_mediums;
    bool allow_any_medium;
    // 移除: with_soft_pin（冷热相关）
};
```

### 3.2 `flat_memory_manager.h`

扁平内存管理器是核心组件：

```cpp
class FlatMemoryManager {
public:
    // 注册存储段（不区分类型）
    ErrorCode RegisterSegment(const FlatSegmentDescriptor& descriptor);
    
    // 分配空间（基于策略，非冷热）
    tl::expected<FlatStorageLocation, ErrorCode> Allocate(
        size_t size,
        const FlatPlacementConfig& config);
    
    // 获取容量统计
    FlatCapacityStats GetCapacityStats() const;
    
private:
    // 策略选择算法
    std::optional<std::string> SelectByCapacity(size_t size);
    std::optional<std::string> SelectByLatency(size_t size);
    std::optional<std::string> SelectByRoundRobin(size_t size);
    std::optional<std::string> SelectRandom(size_t size);
};
```

---

## 4. 修改现有代码

### 4.1 禁用驱逐策略

在 `eviction_strategy.h` 中添加：

```cpp
// 扁平化系统不需要基于冷热的驱逐
class NoEvictionStrategy : public EvictionStrategy {
public:
    virtual ErrorCode AddKey(const std::string& key) override {
        // 仅记录，不参与驱逐
        return ErrorCode::OK;
    }
    
    virtual ErrorCode UpdateKey(const std::string& key) override {
        // 不追踪访问热度
        return ErrorCode::OK;
    }
    
    virtual std::string EvictKey(void) override {
        // 不主动驱逐
        return "";
    }
};
```

### 4.2 添加扁平化分配策略

在 `allocation_strategy.h` 中添加：

```cpp
class FlatAllocationStrategy : public AllocationStrategy {
public:
    explicit FlatAllocationStrategy(
        std::shared_ptr<flat::FlatMemoryManager> manager)
        : flat_manager_(manager) {}
    
    tl::expected<std::vector<Replica>, ErrorCode> Allocate(
        const AllocatorManager& allocator_manager,
        const size_t slice_length,
        const size_t replica_num,
        const std::vector<std::string>& preferred_segments,
        const std::set<std::string>& excluded_segments) override {
        
        // 使用扁平化管理器，不考虑冷热
        flat::FlatPlacementConfig config;
        config.replica_num = replica_num;
        config.preferred_segments = preferred_segments;
        
        auto locations = flat_manager_->AllocateReplicas(
            slice_length, replica_num, config);
        
        // 转换并返回
        // ...
    }
    
private:
    std::shared_ptr<flat::FlatMemoryManager> flat_manager_;
};
```

### 4.3 修改RealClient

在 `real_client.h` 中添加：

```cpp
class RealClient : public PyClient {
public:
    // 启用扁平化模式
    void EnableFlatMemoryMode();
    
    // 扁平化Put
    int put_flat(const std::string& key, 
                 std::span<const char> value,
                 const flat::FlatPlacementConfig& config);
    
    // 扁平化Get
    int64_t get_flat(const std::string& key, 
                     void* buffer, 
                     size_t size);
    
private:
    std::shared_ptr<flat::FlatMemoryManager> flat_memory_manager_;
    bool use_flat_memory_ = false;
};
```

### 4.4 禁用自动Offload

在 `file_storage.cpp` 的 `Heartbeat()` 方法中：

```cpp
tl::expected<void, ErrorCode> FileStorage::Heartbeat() {
    // 检查是否使用扁平化模式
    if (IsFlatMemoryMode()) {
        // 扁平化模式：不进行自动offload
        // 仅同步元数据
        return SyncMetadata();
    }
    
    // 原有分层模式逻辑...
    auto objects_to_offload = client_->GetObjectsToOffload();
    // ...
}
```

---

## 5. TTFT影响分析

### 5.1 TTFT概念

TTFT (Time To First Token) = 从请求到生成第一个token的时间

```
请求到达 → 预填充(Prefill) → 第一个Token
        |<───── TTFT ─────>|
```

### 5.2 分层架构的TTFT优势

原始Mooncake通过以下方式优化TTFT：

1. **热数据在HBM**：高频KVCache保持在GPU，访问延迟~10ns
2. **预取机制**：从L3预取数据到L1
3. **智能放置**：关键数据优先放高速存储

### 5.3 扁平化架构对TTFT的影响

#### 可能增加TTFT的因素

| 因素 | 说明 | 影响 |
|------|------|------|
| 数据位置不确定 | KVCache可能在SSD | 延迟从10ns→10μs |
| 无热点优化 | 不将热数据放HBM | 访问延迟增加 |
| I/O路径变长 | 可能跨多介质 | 增加读取时间 |

#### 定量估算

假设：
- HBM延迟: 10ns
- DRAM延迟: 100ns  
- SSD延迟: 10μs

**分层架构（80%在HBM）**：
```
TTFT ≈ 0.8×10ns + 0.15×100ns + 0.05×10μs ≈ 523ns
```

**扁平架构（均匀分布）**：
```
TTFT ≈ 0.33×10ns + 0.33×100ns + 0.33×10μs ≈ 3.3μs
```

**结论：最坏情况下TTFT可能增加约6倍**

### 5.4 缓解策略

#### 策略1：延迟优先放置

```cpp
FlatPlacementConfig config;
config.policy = PlacementPolicy::LATENCY_FIRST;
```

优先使用低延迟存储，但不强制分层。

#### 策略2：Prefill阶段特殊处理

```cpp
// Prefill阶段使用延迟优先
if (is_prefill_phase) {
    config.policy = PlacementPolicy::LATENCY_FIRST;
    config.preferred_mediums = {StorageMedium::GPU_HBM};
}
```

#### 策略3：混合模式

```cpp
// 关键数据使用首选介质
config.preferred_mediums = {
    StorageMedium::GPU_HBM,
    StorageMedium::SYSTEM_DRAM
};
config.allow_any_medium = true;  // 回退到其他介质
```

---

## 6. 部署与配置

### 6.1 环境变量

```bash
# 启用扁平化模式
export MOONCAKE_FLAT_MEMORY_ENABLED=true

# 设置放置策略
export MOONCAKE_FLAT_PLACEMENT_POLICY=capacity_first
# 可选值: capacity_first, latency_first, round_robin, random

# 禁用自动迁移（核心）
export MOONCAKE_DISABLE_AUTO_MIGRATION=true

# 禁用驱逐（核心）
export MOONCAKE_DISABLE_EVICTION=true
```

### 6.2 代码配置

```cpp
// 创建扁平化配置
flat::FlatMemoryConfig config;
config.enabled = true;
config.default_policy = flat::PlacementPolicy::CAPACITY_FIRST;
config.disable_auto_migration = true;
config.disable_eviction = true;

// 创建管理器
auto manager = std::make_shared<flat::FlatMemoryManager>(config);

// 注册存储段
manager->RegisterSegment(hbm_segment);
manager->RegisterSegment(dram_segment);
manager->RegisterSegment(ssd_segment);

// 使用
auto location = manager->Allocate(size, placement_config);
```

### 6.3 与SGLang HiCache集成

```python
# 在SGLang配置中启用扁平化模式
sglang_config = {
    "hicache_backend": "mooncake",
    "mooncake_flat_memory": True,  # 启用扁平化
    "mooncake_placement_policy": "latency_first",  # TTFT敏感场景
}
```

---

## 7. 最佳实践

### 7.1 场景选择

**适合扁平化架构的场景：**
- 存储容量是主要瓶颈
- 访问模式均匀，无明显热点
- 批处理推理（TTFT不敏感）
- 需要简化运维

**应继续使用分层架构的场景：**
- 实时推理（TTFT敏感）
- 明显的访问热点
- GPU HBM资源充足

### 7.2 性能调优

```cpp
// 1. 对TTFT敏感的操作使用延迟优先
FlatPlacementConfig prefill_config;
prefill_config.policy = PlacementPolicy::LATENCY_FIRST;

// 2. 批量数据使用容量优先
FlatPlacementConfig batch_config;
batch_config.policy = PlacementPolicy::CAPACITY_FIRST;

// 3. 多副本确保可靠性
FlatPlacementConfig reliable_config;
reliable_config.replica_num = 3;
```

### 7.3 监控建议

```cpp
// 获取容量统计
auto stats = manager->GetCapacityStats();
LOG(INFO) << "Total utilization: " << stats.totalUtilization();

// 按介质查看
for (auto& [medium, used] : stats.used_by_medium) {
    LOG(INFO) << medium << ": " << used << " / " 
              << stats.capacity_by_medium[medium];
}
```

---

## 总结

KVCache Flat Memory System通过以下核心改变实现：

1. **移除冷热区分**：所有存储介质等价对待
2. **禁用自动迁移**：数据不会因为"冷"而被移动
3. **禁用驱逐策略**：不基于访问频率驱逐
4. **统一放置策略**：容量优先/延迟优先/轮询/随机

**TTFT会增加**，但可以通过：
- 使用LATENCY_FIRST策略缓解
- Prefill阶段特殊处理
- 混合模式配置

新增文件位置：
- `mooncake-store/include/flat_memory_types.h`
- `mooncake-store/include/flat_memory_manager.h`
- `mooncake-store/src/flat_memory_manager.cpp`
- `mooncake-store/examples/flat_memory_example.cpp`


```
┌─────────────────────────────────────────────────────────────────────────────┐
│                          依赖关系                                            │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│   vLLM / SGLang                                                             │
│        │                                                                    │
│        ▼                                                                    │
│   mooncake-integration  ─────► 调用 mooncake-store 的 Python API             │
│        │                                                                    │
│        ▼                                                                    │
│   mooncake-store  ──────────► 调用 transfer-engine 进行数据传输                │
│        │                                                                    │
│        ▼                                                                    │
│   mooncake-transfer-engine ─► 实际执行 RDMA/TCP 传输                          │
│        │                                                                    │
│        ▼                                                                    │
│   mooncake-common  ─────────► 提供基础工具类                                   │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```
