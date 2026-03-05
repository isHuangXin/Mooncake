# KVCache Flat Memory System 实现文档

## 概述

本文档记录了在 Mooncake 代码库中实现的 **KVCache Flat Memory System**（扁平内存系统）。

```
┌─────────────────────────────────────────────────────────────────────────────┐
│  当前 Mooncake 架构                          你的目标架构                      │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  ┌─────────────┐                            ┌─────────────────────────────┐ │
│  │   DRAM Pool │  ◄──── 主存储               │   Unified Segment Pool      │ │
│  │  (Active)   │                            │   (统一地址空间)              │ │
│  └──────┬──────┘                            │                             │ │
│         │                                   │  ┌─────┐ ┌─────┐ ┌─────┐    │ │
│         │ 备份/恢复                          │  │ HBM │ │DRAM │ │ SSD │    │ │
│         ▼                                   │  │ Seg │ │ Seg │ │ Seg │    │ │
│  ┌─────────────┐                            │  └──┬──┘ └──┬──┘ └──┬──┘    │ │
│  │  SSD/DFS    │  ◄──── 备份层               │     │       │       │       │ │
│  │ (Duplicate) │       数据重复              │     └───────┴───────┘       │ │
│  └─────────────┘                            │      统一寻址，不重复          │ │
│                                             └─────────────────────────────┘ │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 核心设计理念

**与原 Mooncake Store 的关键区别：**

| 特性 | 原 Mooncake Store | Flat Memory System |
|------|-------------------|-------------------|
| SSD 角色 | 备份层（数据重复存储） | 独立存储位置（数据不重复） |
| 冷热迁移 | 自动基于访问频率 | 显式迁移（用户控制） |
| 地址空间 | 分层管理 | **统一 64-bit 地址空间** |
| 数据副本 | DRAM+SSD 必然重复 | 每个对象只有**一个**位置 |

## 架构图

```
┌─────────────────────────────────────────────────────────────────┐
│                    FlatMemoryClient (用户接口)                   │
│ Put() / Get() / AsyncGet() / Remove() / MigrateTo() / Prefetch()│
└─────────────────────────────────────────────────────────────────┘
                                    │
                                    ▼
┌─────────────────────────────────────────────────────────────────┐
│                   KVCacheFlatStore (对象存储)                    │
│        管理 KVCacheObjectMeta，每个对象只有一个 location            │
└─────────────────────────────────────────────────────────────────┘
                                    │
                                    ▼
┌─────────────────────────────────────────────────────────────────┐
│              UnifiedSegmentManager (Segment 管理器)              │
│   Allocate() / Deallocate() / Read() / Write() / 放置策略        │
└─────────────────────────────────────────────────────────────────┘
                                    │
        ┌───────────────────────────┼───────────────────────────────────┐
        │               │               │               │               │
        ▼               ▼               ▼               ▼               ▼
┌───────────┐   ┌───────────┐   ┌───────────┐   ┌───────────┐   ┌───────────┐
│ HBMSegment│   │LocalDRAM  │   │RemoteDRAM │   │ LocalSSD  │   │ RemoteSSD │
│   (GPU)   │   │ Segment   │   │ Segment   │   │ Segment   │   │ Segment   │
│ ~10ns     │   │ ~100ns    │   │ ~2μs      │   │ ~10μs     │   │ ~50μs     │
│ CUDA API  │   │ memcpy    │   │ RDMA      │   │ DirectIO  │   │ 3FS/NFS   │
└───────────┘   └───────────┘   └───────────┘   └───────────┘   └───────────┘
```

## 统一地址格式

```
UnifiedAddress: 64-bit
┌──────────────────┬──────────────────────────────────────────────┐
│  Segment ID (16) │              Offset (48)                     │
└──────────────────┴──────────────────────────────────────────────┘
      65536 个          每个 Segment 最大 256TB
       Segments
```

## 文件结构

```
mooncake-store/
├── include/
│   ├── unified_segment.h          # 核心接口定义
│   ├── segment_impls.h            # 5 种 Segment 实现
│   ├── unified_segment_manager.h  # Segment 管理器
│   ├── kvcache_flat_store.h       # KVCache 对象存储
│   └── flat_memory_client.h       # 用户客户端
├── src/
│   ├── unified_segment.cpp        # RemoteDRAM/SSD 实现
│   └── flat_memory_client.cpp     # Client 实现
└── examples/
    └── flat_memory_usage_example.cpp  # 使用示例
```

## 核心组件详解

### 1. UnifiedMediumType（存储介质类型）

```cpp
enum class UnifiedMediumType : uint8_t {
    GPU_HBM      = 0,  // GPU 高带宽内存，最快，容量最小
    LOCAL_DRAM   = 1,  // 本地 DDR 内存
    REMOTE_DRAM  = 2,  // 远程节点 DRAM（通过 RDMA）
    LOCAL_SSD    = 3,  // 本地 NVMe SSD
    REMOTE_SSD   = 4,  // 远程 SSD（通过 3FS/NFS）
};
```

**延迟层次（典型值）：**

| 介质 | 延迟 | 带宽 | 容量（典型） |
|------|------|------|-------------|
| GPU_HBM | ~10ns | 1-3 TB/s | 16-80 GB |
| LOCAL_DRAM | ~100ns | 100-400 GB/s | 256-1024 GB |
| REMOTE_DRAM | ~2μs | 100-400 Gbps | TB 级 |
| LOCAL_SSD | ~10μs | 5-10 GB/s | 1-8 TB |
| REMOTE_SSD | ~50μs | 1-5 GB/s | PB 级 |

### 2. IUnifiedSegment（统一 Segment 接口）

```cpp
class IUnifiedSegment {
public:
    // 基本属性
    virtual uint16_t GetSegmentId() const = 0;
    virtual UnifiedMediumType GetMediumType() const = 0;
    virtual size_t GetCapacity() const = 0;
    
    // 同步读写
    virtual int Read(uint64_t offset, void* buffer, size_t size) = 0;
    virtual int Write(uint64_t offset, const void* data, size_t size) = 0;
    
    // 异步读写
    virtual int AsyncRead(uint64_t offset, void* buffer, size_t size,
                         Callback callback) = 0;
    virtual int AsyncWrite(uint64_t offset, const void* data, size_t size,
                          Callback callback) = 0;
    
    // 批量 I/O
    virtual int BatchIO(const std::vector<IORequest>& requests) = 0;
};
```

### 3. 具体 Segment 实现

#### HBMSegment（GPU 内存）
```cpp
class HBMSegment : public UnifiedSegmentBase {
    void* gpu_memory_;
    int device_id_;
    
    int Read(uint64_t offset, void* buffer, size_t size) override {
        return cudaMemcpy(buffer, 
                         static_cast<const char*>(gpu_memory_) + offset,
                         size, cudaMemcpyDeviceToHost);
    }
    
    int Write(uint64_t offset, const void* data, size_t size) override {
        return cudaMemcpy(static_cast<char*>(gpu_memory_) + offset,
                         data, size, cudaMemcpyHostToDevice);
    }
};
```

#### LocalDRAMSegment（本地内存）
```cpp
class LocalDRAMSegment : public UnifiedSegmentBase {
    void* memory_;  // aligned_alloc 分配
    
    int Read(uint64_t offset, void* buffer, size_t size) override {
        memcpy(buffer, static_cast<const char*>(memory_) + offset, size);
        return 0;
    }
};
```

#### RemoteDRAMSegment（RDMA 远程内存）
```cpp
class RemoteDRAMSegment : public UnifiedSegmentBase {
    std::shared_ptr<TransferEngine> transfer_engine_;
    
    int Read(uint64_t offset, void* buffer, size_t size) override {
        // 使用 RDMA READ 操作
        TransferRequest req;
        req.type = TransferType::READ;
        req.local_addr = buffer;
        req.remote_addr = remote_base_addr_ + offset;
        req.size = size;
        
        return transfer_engine_->Submit(req)->Wait();
    }
};
```

#### LocalSSDSegment（本地 SSD）
```cpp
class LocalSSDSegment : public UnifiedSegmentBase {
    int fd_;  // O_DIRECT 打开的文件描述符
    
    int Read(uint64_t offset, void* buffer, size_t size) override {
        ssize_t ret = pread(fd_, buffer, size, offset);
        return (ret == static_cast<ssize_t>(size)) ? 0 : -1;
    }
};
```

### 4. UnifiedSegmentManager

管理所有 Segment，提供统一的分配和访问接口：

```cpp
class UnifiedSegmentManager {
public:
    // 注册/注销 Segment
    void RegisterSegment(std::unique_ptr<IUnifiedSegment> segment);
    void UnregisterSegment(uint16_t segment_id);
    
    // 分配空间（根据策略选择 Segment）
    std::optional<UnifiedAllocationResult> Allocate(
        const UnifiedAllocationRequest& request);
    
    // 通过统一地址访问
    int Read(UnifiedAddress addr, void* buffer, size_t size);
    int Write(UnifiedAddress addr, const void* data, size_t size);
};
```

**放置策略：**

| 策略 | 描述 |
|------|------|
| LATENCY_FIRST | 优先低延迟介质（HBM > LocalDRAM > RemoteDRAM > ...） |
| CAPACITY_FIRST | 优先大容量介质（RemoteSSD > LocalSSD > ...） |
| ROUND_ROBIN | 轮询分配 |
| LOCALITY_AWARE | 优先本地介质 |

### 5. KVCacheFlatStore

KVCache 对象级别的存储：

```cpp
struct KVCacheObjectMeta {
    std::string key;
    UnifiedAddress location;  // **唯一**位置，不重复存储
    size_t size;
    UnifiedMediumType medium;
    // 统计信息（不用于冷热判断）
    uint64_t create_time;
    uint64_t last_access_time;
};

class KVCacheFlatStore {
public:
    int Put(const std::string& key, const void* data, size_t size,
            const KVCachePutConfig& config);
    
    ssize_t Get(const std::string& key, void* buffer, size_t size);
    
    int Remove(const std::string& key);
    
    // 显式迁移（不是自动冷热迁移）
    int MigrateTo(const std::string& key, UnifiedMediumType target);
};
```

### 6. FlatMemoryClient

用户友好的高级接口：

```cpp
auto client = FlatMemoryClient::Create(config);

// 存储 KVCache（系统自动选择位置）
client->Put("request_123/layer_0", data, size);

// 读取 KVCache
client->Get("request_123/layer_0", buffer, size);

// 显式迁移到 HBM（用户主动控制）
client->MigrateTo("request_123/layer_0", UnifiedMediumType::GPU_HBM);

// Prefetch（预测性加载）
client->Prefetch("request_456/layer_0", UnifiedMediumType::LOCAL_DRAM);
```

## 使用示例

### 基本使用

```cpp
#include "flat_memory_client.h"
using namespace mooncake::flat;

// 1. 配置
FlatClusterConfig config;
config.local_node.local_dram.enabled = true;
config.local_node.local_dram.capacity = 16ULL * 1024 * 1024 * 1024;  // 16GB

// 2. 创建客户端
auto client = FlatMemoryClient::Create(config);

// 3. 存储 KVCache
std::vector<float> kv_data(1024 * 1024);  // 4MB
client->Put("req_abc/layer_0", kv_data.data(), kv_data.size() * sizeof(float));

// 4. 读取 KVCache
std::vector<float> buffer(1024 * 1024);
client->Get("req_abc/layer_0", buffer.data(), buffer.size() * sizeof(float));
```

### 配置文件

```json
{
  "local_node": {
    "node_id": "inference_node_0",
    "address": "192.168.1.100",
    "port": 12345,
    "hbm": {
      "enabled": true,
      "device_id": 0,
      "capacity": 4294967296
    },
    "local_dram": {
      "enabled": true,
      "capacity": 34359738368
    },
    "local_ssd": {
      "enabled": true,
      "path": "/mnt/nvme0/kvcache",
      "capacity": 107374182400
    }
  },
  "remote_nodes": [
    {
      "node_id": "storage_node_0",
      "address": "192.168.1.200",
      "rdma_port": 12345,
      "dram_capacity": 68719476736,
      "has_ssd": true,
      "ssd_path": "/mnt/3fs/kvcache",
      "ssd_capacity": 1099511627776,
      "ssd_type": "3fs"
    }
  ],
  "default_policy": "latency_first"
}
```

## 与原 Mooncake 的集成

### 不改变原有逻辑

Flat Memory System 作为**独立模块**，不影响原有 Mooncake Store：

```
mooncake-store/
├── include/
│   ├── common.h              # 原有
│   ├── client.h              # 原有 Mooncake Client
│   ├── types.h               # 原有
│   │
│   ├── unified_segment.h     # 新增 - Flat Memory
│   ├── segment_impls.h       # 新增
│   ├── unified_segment_manager.h  # 新增
│   ├── kvcache_flat_store.h  # 新增
│   └── flat_memory_client.h  # 新增
```

### 可复用 Transfer Engine

Flat Memory System 复用了原有 Mooncake 的 Transfer Engine 进行 RDMA 通信：

```cpp
// RemoteDRAMSegment 使用 Transfer Engine
#include "transfer_engine.h"

class RemoteDRAMSegment {
    std::shared_ptr<TransferEngine> transfer_engine_;
    
    int Read(...) {
        // 复用 Transfer Engine 的 RDMA 能力
        return transfer_engine_->Submit(req)->Wait();
    }
};
```

## 关键设计决策

### 1. 为什么不自动冷热迁移？

原 Mooncake Store 的冷热迁移设计：
- 热数据在 DRAM，冷数据在 SSD
- SSD 是 DRAM 的备份，数据重复存储

Flat Memory System 的设计：
- **每个数据只有一个位置**
- 用户根据业务逻辑显式控制位置
- 避免数据重复带来的存储浪费

**适用场景对比：**

| 场景 | 原 Mooncake | Flat Memory |
|------|------------|-------------|
| 随机访问模式 | ✓ 自动缓存热点 | △ 需要预测 |
| 可预测访问模式 | △ 可能浪费缓存 | ✓ 精准放置 |
| 存储成本敏感 | △ 数据重复 | ✓ 无重复 |
| 延迟敏感（推理） | ✓ | ✓ |

### 2. 为什么使用统一地址空间？

好处：
1. **简化编程模型**：无需关心数据在哪
2. **透明迁移**：迁移只改地址，不改上层代码
3. **支持异构存储**：HBM/DRAM/SSD 统一管理

### 3. SSD 加载为什么必须经过 DRAM？

```
SSD → DRAM → RDMA NIC → 网络 → Remote
     ↑
   必须经过
```

原因：
- RDMA 需要**注册内存**（pinned memory）
- SSD 数据不在 pinned memory 中
- 必须先加载到 DRAM 再发送

## 性能优化建议

### 1. Prefetch 策略

```cpp
// 推理开始前，预加载预测会用到的 KVCache
for (auto& predicted_key : predicted_keys) {
    client->Prefetch(predicted_key, UnifiedMediumType::LOCAL_DRAM);
}
```

### 2. 批量操作

```cpp
// 批量存储比单个存储更高效
std::vector<std::tuple<std::string, const void*, size_t>> items;
for (int layer = 0; layer < 32; ++layer) {
    items.emplace_back(
        "req_123/layer_" + std::to_string(layer),
        layer_data[layer],
        layer_size);
}
client->BatchPut(items);
```

### 3. 异步操作

```cpp
// 异步读取，不阻塞主线程
client->AsyncGet(key, buffer, size, [](int status, size_t bytes) {
    if (status == 0) {
        // 处理数据
    }
});
// 继续做其他事情...
```

## 总结

KVCache Flat Memory System 提供了：

1. **统一地址空间**：HBM/DRAM/SSD 透明管理
2. **无数据重复**：每个 KVCache 只有一个存储位置
3. **显式控制**：用户根据业务逻辑决定数据位置
4. **高级 API**：简单易用的 Put/Get/MigrateTo/Prefetch
5. **与现有系统兼容**：复用 Transfer Engine，不影响原有模块

---

*文档版本: 1.0*
*创建日期: 2024*
*作者: Mooncake Team*
