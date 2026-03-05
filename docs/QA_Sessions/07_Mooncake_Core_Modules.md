# Mooncake 核心模块解析

## 问题

> Mooncake 这个代码库，最核心的是哪几个模块？

---

## 回答

Mooncake 代码库最核心的是以下 **4 个模块**：

### 整体架构概览

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                        Mooncake 核心模块                                     │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│                    ┌─────────────────────────────┐                          │
│                    │   mooncake-integration      │  ← 与推理框架集成         │
│                    │   (vLLM/SGLang 集成)        │                          │
│                    └──────────────┬──────────────┘                          │
│                                   │                                         │
│                                   ▼                                         │
│                    ┌─────────────────────────────┐                          │
│                    │     mooncake-store          │  ← KVCache 存储引擎       │
│                    │   (分布式 KV 存储)          │                          │
│                    └──────────────┬──────────────┘                          │
│                                   │                                         │
│                                   ▼                                         │
│                    ┌─────────────────────────────┐                          │
│                    │  mooncake-transfer-engine   │  ← 高性能数据传输         │
│                    │   (RDMA/TCP/NVLink)         │                          │
│                    └──────────────┬──────────────┘                          │
│                                   │                                         │
│                                   ▼                                         │
│                    ┌─────────────────────────────┐                          │
│                    │     mooncake-common         │  ← 公共基础设施           │
│                    │   (工具类/配置/日志)        │                          │
│                    └─────────────────────────────┘                          │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## 一、mooncake-transfer-engine（最底层、最核心）

### 1.1 作用

**高性能零拷贝数据传输引擎** —— 整个 Mooncake 系统的基础。

### 1.2 核心功能

| 功能 | 说明 |
|------|------|
| **多协议支持** | RDMA、TCP、NVLink、NVMe-oF、CXL |
| **Segment 抽象** | 统一的远程内存访问接口 |
| **BatchTransfer** | 批量异步数据传输 |
| **拓扑感知** | 自动选择最优网卡路径 |
| **元数据管理** | 通过 etcd 自动交换连接信息 |

### 1.3 目录结构

```
mooncake-transfer-engine/
├── include/
│   ├── transfer_engine.h      # 主接口
│   ├── transfer_metadata.h    # 元数据管理 (Segment 信息)
│   ├── transport/
│   │   └── transport.h        # 传输抽象层
│   └── topology.h             # 拓扑管理
├── src/
│   ├── transport/
│   │   ├── rdma_transport/    # RDMA 实现 (核心)
│   │   ├── tcp_transport/     # TCP 实现
│   │   ├── nvlink_transport/  # NVLink 实现
│   │   └── nvmeof_transport/  # NVMe-oF 实现
│   ├── transfer_engine.cpp    # 引擎实现
│   └── transfer_metadata.cpp  # 元数据实现
└── tent/                      # TENT 新一代架构
    ├── include/tent/
    │   ├── transfer_engine.h
    │   └── runtime/
    │       ├── segment.h      # Segment 定义
    │       └── segment_manager.h
    └── src/
```

### 1.4 核心 API

```cpp
class TransferEngine {
    // 初始化
    int init(const std::string& metadata_server, 
             const std::string& local_server_name);
    
    // Segment 操作
    int registerLocalMemory(void* addr, size_t length, 
                            const std::string& location);
    SegmentHandle openSegment(const std::string& segment_name);
    
    // 批量传输
    BatchID allocateBatchID(size_t batch_size);
    Status submitTransfer(BatchID batch_id, 
                          const std::vector<TransferRequest>& entries);
    Status getTransferStatus(BatchID batch_id, size_t task_id, 
                             TransferStatus& status);
    Status freeBatchID(BatchID batch_id);
};
```

---

## 二、mooncake-store（KVCache 存储核心）

### 2.1 作用

**分布式 KVCache 存储引擎** —— 提供 KVCache 的存储、复制、驱逐等语义。

### 2.2 核心功能

| 功能 | 说明 |
|------|------|
| **分布式存储** | 跨节点 KVCache 共享 |
| **冷热分层** | Soft Pin (热数据保护)、Lease (读写保护) |
| **自动驱逐** | LRU/FIFO 策略，95% 水位触发 |
| **Master-Worker** | 集中式元数据管理 |
| **多副本** | 数据可靠性保障 |

### 2.3 目录结构

```
mooncake-store/
├── include/
│   ├── client.h               # 客户端接口 (用户使用)
│   ├── master_service.h       # Master 服务定义
│   ├── eviction_strategy.h    # 驱逐策略 (LRU/FIFO)
│   ├── allocation_strategy.h  # 分配策略
│   ├── segment.h              # 存储段管理
│   ├── replica.h              # 副本配置
│   └── types.h                # 类型定义 (TTL 常量等)
├── src/
│   ├── master_service.cpp     # Master 实现 (驱逐逻辑)
│   ├── real_client.cpp        # 客户端实现
│   ├── file_storage.cpp       # 文件存储 (SSD/DFS)
│   └── segment.cpp            # Segment 管理
└── python/
    └── mooncake_store.py      # Python 绑定
```

### 2.4 核心 API

```cpp
class MooncakeDistributedStore {
    // 初始化
    int setup(const std::string& local_hostname,
              const std::string& metadata_server,
              size_t global_segment_size);
    
    // KV 操作
    int put(const std::string& key, const void* value, size_t size,
            const ReplicateConfig& config);
    int get(const std::string& key, void* buffer, size_t* size);
    int remove(const std::string& key);
    
    // 查询
    bool isExist(const std::string& key);
    std::vector<std::string> getReplicaList(const std::string& key);
};
```

---

## 三、mooncake-integration（推理框架集成）

### 3.1 作用

**与 vLLM、SGLang 等推理框架的集成层** —— 将 Mooncake 能力暴露给上层应用。

### 3.2 核心功能

| 功能 | 说明 |
|------|------|
| **HiCache** | GPU HBM → Host DRAM → Mooncake Store 三级缓存 |
| **Prefix Caching** | 前缀复用优化，减少重复计算 |
| **PD 分离** | Prefill-Decode 分离架构支持 |
| **KVCache 管理** | 自动管理 KVCache 生命周期 |

### 3.3 目录结构

```
mooncake-integration/
├── vllm/                      # vLLM 集成
│   ├── mooncake_vllm_adapter.py
│   └── kvcache_connector.py
├── sglang/                    # SGLang 集成
│   ├── hicache_storage.py     # HiCache 实现 (核心)
│   └── prefix_cache.py
└── common/
    ├── kvcache_manager.py     # KVCache 管理器
    └── transfer_helper.py     # 传输辅助
```

### 3.4 HiCache 三级缓存架构

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                        HiCache 三级缓存                                      │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│   L1: GPU HBM (纳秒级)                                                      │
│   ├── 存储正在计算的热 KVCache                                              │
│   └── 容量有限，需要驱逐到 L2                                               │
│                        │                                                    │
│                        ▼ evict                                              │
│   L2: Host DRAM (10-100ns)                                                  │
│   ├── 本地 CPU 内存缓存                                                     │
│   └── 作为 L1 和 L3 之间的缓冲                                              │
│                        │                                                    │
│                        ▼ write-back                                         │
│   L3: Mooncake Store (微秒级)                                               │
│   ├── 分布式 DRAM 池 (RDMA 访问)                                            │
│   └── 分布式 SSD/DFS (持久化)                                               │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## 四、mooncake-common（公共基础）

### 4.1 作用

**公共工具和基础设施** —— 为其他模块提供通用能力。

### 4.2 目录结构

```
mooncake-common/
├── include/
│   ├── base/
│   │   ├── status.h           # 错误状态码
│   │   └── logging.h          # 日志工具
│   ├── config/
│   │   └── config.h           # 配置管理
│   └── utils/
│       ├── memory_pool.h      # 内存池
│       └── timer.h            # 计时器
└── src/
    └── ...
```

---

## 五、模块依赖关系

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                          依赖关系图                                          │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│   ┌─────────────────┐                                                       │
│   │  vLLM / SGLang  │  ← 推理框架 (用户直接使用)                            │
│   └────────┬────────┘                                                       │
│            │ 调用                                                           │
│            ▼                                                                │
│   ┌─────────────────────────┐                                               │
│   │  mooncake-integration   │  ← Python 集成层                              │
│   │  (HiCache, PD分离)      │                                               │
│   └────────┬────────────────┘                                               │
│            │ 调用                                                           │
│            ▼                                                                │
│   ┌─────────────────────────┐                                               │
│   │    mooncake-store       │  ← KVCache 存储语义                           │
│   │  (分布式KV, 冷热分层)    │                                               │
│   └────────┬────────────────┘                                               │
│            │ 调用                                                           │
│            ▼                                                                │
│   ┌─────────────────────────┐                                               │
│   │ mooncake-transfer-engine│  ← 数据传输 (最核心)                          │
│   │  (RDMA, Segment)        │                                               │
│   └────────┬────────────────┘                                               │
│            │ 依赖                                                           │
│            ▼                                                                │
│   ┌─────────────────────────┐                                               │
│   │    mooncake-common      │  ← 基础工具                                   │
│   └─────────────────────────┘                                               │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## 六、总结

| 模块 | 核心价值 | 关键文件 | 重要程度 |
|------|---------|---------|---------|
| **transfer-engine** | 高性能 RDMA 传输 | `transfer_engine.h`, `transport.h` | ⭐⭐⭐⭐⭐ |
| **store** | KVCache 分布式存储 | `master_service.cpp`, `client.h` | ⭐⭐⭐⭐ |
| **integration** | 推理框架适配 | `hicache_storage.py` | ⭐⭐⭐ |
| **common** | 基础设施 | `status.h`, `config.h` | ⭐⭐ |

### 核心结论

1. **最核心的是 `mooncake-transfer-engine`**
   - 提供跨节点高性能数据传输能力
   - 是整个系统的基础设施
   - 支持 RDMA、TCP、NVLink 等多种协议

2. **`mooncake-store` 是业务核心**
   - 在 transfer-engine 之上构建 KVCache 语义
   - 实现冷热分层、自动驱逐等关键功能

3. **`mooncake-integration` 是用户入口**
   - 与 vLLM/SGLang 集成
   - 提供 HiCache 三级缓存

4. **阅读代码建议顺序**
   ```
   transfer-engine (理解传输) → store (理解存储) → integration (理解应用)
   ```
