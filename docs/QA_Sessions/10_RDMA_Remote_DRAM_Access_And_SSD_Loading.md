# Q10: RDMA 远程 DRAM 访问与 SSD 加载机制

## 问题

1. Mooncake 是怎么通过 RDMA 访问其他 Remote DRAM 的？
2. 访问本地 DRAM 是通过 PCIe 吗？
3. 如果 KVCache 在 HBM 和 DRAM 中都没有找到，需要从 SSD 中加载，那么加载的逻辑是怎样的？先加载到任一 DRAM，然后用 RDMA 去读取吗？

---

## 答案总览

| 访问类型 | 数据路径 | 传输协议 | 延迟级别 |
|---------|---------|---------|---------|
| 访问本地 DRAM | CPU ↔ DRAM | Memory Bus (DDR) | ~100ns |
| 访问远程 DRAM | NIC ↔ Remote DRAM | RDMA (InfiniBand) | ~1-10μs |
| GPU 访问 Host DRAM | GPU ↔ CPU DRAM | PCIe DMA | ~1-5μs |
| 从 SSD 加载 | SSD → Local DRAM → GPU/Remote | NVMe + RDMA | ~10-100μs |

---

## 1. RDMA 访问远程 DRAM 的机制

### 1.1 RDMA 核心原理

RDMA (Remote Direct Memory Access) 允许网卡绕过 CPU 直接读写远程主机的内存：

```
┌───────────────────────────────────────────────────────────────────────────────┐
│                        RDMA 远程内存访问流程                                     │
├───────────────────────────────────────────────────────────────────────────────┤
│                                                                               │
│   Node A (请求方)                           Node B (数据方)                     │
│   ┌─────────────────┐                       ┌─────────────────┐               │
│   │   应用程序       │                       │   应用程序       │                │
│   │ (SGLang Client) │                       │  (Store Server) │               │
│   └────────┬────────┘                       └────────┬────────┘               │
│            │                                         │                        │
│            │ 1. 注册内存                              │ 1. 注册内存              │
│            ▼                                         ▼                        │
│   ┌─────────────────┐                       ┌─────────────────┐               │
│   │ Transfer Engine │                       │ Transfer Engine │               │
│   │registerMemory() │                       │ registerMemory()│               │
│   └────────┬────────┘                       └────────┬────────┘               │
│            │                                         │                        │
│            │ 2. ibv_reg_mr()                         │ 2. ibv_reg_mr()        │
│            │    获取 lkey/rkey                        │    获取 lkey/rkey      │
│            ▼                                         ▼                        │
│   ┌─────────────────┐                       ┌─────────────────┐               │
│   │   RDMA Context  │ ◄────Handshake────►   │   RDMA Context  │               │
│   │  (QP, CQ, MR)   │   交换连接信息          │  (QP, CQ, MR)   │               │
│   └────────┬────────┘                       └────────┬────────┘               │
│            │                                         │                        │
│            │ 3. ibv_post_send()                      │                        │
│            │    IBV_WR_RDMA_READ                     │                        │
│            ▼                                         ▼                        │
│   ┌─────────────────┐                       ┌─────────────────┐               │
│   │   RDMA NIC       │ ═══════════════════► │   RDMA NIC      │               │
│   │  (Mellanox CX7)  │     RDMA READ        │  (Mellanox CX7) │               │
│   └─────────────────┘     直接访问内存        └─────────────────┘               │
│                                                      │                        │
│                                              ┌───────▼───────┐                │
│                                              │  Remote DRAM  │                │
│                                              │   (KVCache)   │                │
│                                              └───────────────┘                │
│                                                                               │
└───────────────────────────────────────────────────────────────────────────────┘
```

### 1.2 Mooncake 中的 RDMA 实现

核心代码在 [rdma_endpoint.cpp](../../mooncake-transfer-engine/src/transport/rdma_transport/rdma_endpoint.cpp)：

```cpp
// 提交 RDMA 读/写请求
int RdmaEndPoint::submitPostSend(
    std::vector<Transport::Slice *> &slice_list,
    std::vector<Transport::Slice *> &failed_slice_list) {
    
    // ... 准备工作请求 (Work Request)
    ibv_send_wr wr_list[wr_count], *bad_wr = nullptr;
    ibv_sge sge_list[wr_count];
    
    for (int i = 0; i < wr_count; ++i) {
        auto slice = slice_list[i];
        auto &sge = sge_list[i];
        
        // 设置本地内存地址和密钥
        sge.addr = (uint64_t)slice->source_addr;
        sge.length = slice->length;
        sge.lkey = slice->rdma.source_lkey;  // 本地内存注册密钥

        auto &wr = wr_list[i];
        // 操作类型：RDMA READ 或 WRITE
        wr.opcode = slice->opcode == Transport::TransferRequest::READ
                        ? IBV_WR_RDMA_READ   // 从远程读取
                        : IBV_WR_RDMA_WRITE; // 写入远程
        
        // 设置远程内存地址和密钥
        wr.wr.rdma.remote_addr = slice->rdma.dest_addr;
        wr.wr.rdma.rkey = slice->rdma.dest_rkey;  // 远程内存注册密钥
    }
    
    // 提交到网卡队列
    int rc = ibv_post_send(qp_list_[qp_index], wr_list, &bad_wr);
    // ...
}
```

### 1.3 RDMA 连接建立（Handshake）

两个节点之间的 RDMA 连接通过握手交换元数据：

```cpp
// rdma_endpoint.cpp - 主动建立连接
int RdmaEndPoint::setupConnectionsByActive() {
    HandShakeDesc local_desc, peer_desc;
    
    // 准备本地连接信息
    local_desc.local_nic_path = context_.nicPath();  // 本地网卡路径
    local_desc.peer_nic_path = peer_nic_path_;       // 对端网卡路径
    local_desc.qp_num = qpNum();                     // 队列对编号
    
    // 通过 RPC 交换连接信息
    int rc = context_.engine().sendHandshake(
        peer_server_name, local_desc, peer_desc);
    
    // 建立 QP 连接
    return doSetupConnection(nic.gid, nic.lid, peer_desc.qp_num);
}
```

交换的元数据包括：

| 元数据 | 含义 | 用途 |
|--------|------|------|
| GID | Global Identifier | 全局唯一标识符，用于跨子网路由 |
| LID | Local Identifier | 子网内本地标识符 |
| QP Num | Queue Pair Number | 队列对编号，用于连接匹配 |
| RKEY | Remote Key | 远程内存访问密钥 |

---

## 2. 本地 DRAM 访问：不是 PCIe

### 2.1 CPU 访问本地 DRAM

**不是 PCIe**，而是通过 **Memory Bus (DDR 总线)** 直接访问：

```
┌────────────────────────────────────────────────────────────────────────────────┐
│                          本地 DRAM 访问架构                                      │
├────────────────────────────────────────────────────────────────────────────────┤
│                                                                                │
│                          ┌─────────────┐                                       │
│                          │    CPU      │                                       │
│                          │ (运行应用)   │                                       │
│                          └──────┬──────┘                                       │
│                                 │                                              │
│                    ┌────────────┴────────────┐                                 │
│                    │     Memory Controller    │                                 │
│                    │       (内存控制器)        │                                 │
│                    └────────────┬────────────┘                                 │
│                                 │                                              │
│            ┌────────────────────┼────────────────────┐                         │
│            │                    │                    │                         │
│            ▼                    ▼                    ▼                         │
│   ┌─────────────────┐  ┌─────────────────┐  ┌─────────────────┐               │
│   │   DRAM Channel   │  │   DRAM Channel   │  │   DRAM Channel   │               │
│   │     DDR5-6400    │  │     DDR5-6400    │  │     DDR5-6400    │               │
│   │   (~100ns 延迟)   │  │   (~100ns 延迟)   │  │   (~100ns 延迟)   │               │
│   │   (~50GB/s 带宽)  │  │   (~50GB/s 带宽)  │  │   (~50GB/s 带宽)  │               │
│   └─────────────────┘  └─────────────────┘  └─────────────────┘               │
│                                                                                │
│   ═══════════════════════════════════════════════════════════════════════      │
│                         通过 DDR 总线直接访问                                    │
│                           不经过 PCIe ！                                        │
│                                                                                │
└────────────────────────────────────────────────────────────────────────────────┘
```

### 2.2 GPU 访问 Host DRAM：通过 PCIe

GPU 访问 Host DRAM **确实通过 PCIe**：

```
┌────────────────────────────────────────────────────────────────────────────────┐
│                       GPU 访问 Host DRAM 路径                                   │
├────────────────────────────────────────────────────────────────────────────────┤
│                                                                                │
│   ┌─────────────────┐        ┌─────────────────┐        ┌─────────────────┐    │
│   │   GPU           │        │    CPU          │        │   Host DRAM     │    │
│   │   (NVIDIA A100) │        │    (x86_64)     │        │   (KVCache L2)  │    │
│   │                 │        │                 │        │                 │    │
│   │ ┌─────────────┐ │        │                 │        │                 │    │
│   │ │  GPU HBM    │ │        │                 │        │                 │    │
│   │ │ (KVCache L1)│ │        │                 │        │                 │    │
│   │ └─────────────┘ │        │                 │        │                 │    │
│   │                 │        │                 │        │                 │    │
│   └────────┬────────┘        └────────┬────────┘        └────────┬────────┘    │
│            │                          │                          │             │
│            │      PCIe 4.0/5.0        │      DDR Bus            │             │
│            │      ~32-64 GB/s         │      ~200 GB/s          │             │
│            │      ~1-5 μs延迟          │      ~100 ns延迟         │             │
│            │                          │                          │             │
│            └──────────────────────────┴──────────────────────────┘             │
│                                                                                │
│   HiCache L2 → L1 数据加载路径:                                                 │
│   Host DRAM (L2) ──[PCIe DMA]──► GPU HBM (L1)                                  │
│                                                                                │
└────────────────────────────────────────────────────────────────────────────────┘
```

### 2.3 RDMA 访问本地 DRAM：Loopback 模式

当目标是同一节点时，Mooncake 支持 **RDMA Loopback**：

```cpp
// rdma_endpoint.cpp
int RdmaEndPoint::setupConnectionsByActive() {
    // loopback mode - 检测是否访问本地
    if (context_.nicPath() == peer_nic_path_) {
        // 目标是本地节点，使用 loopback
        auto segment_desc =
            context_.engine().meta()->getSegmentDescByID(LOCAL_SEGMENT_ID);
        if (segment_desc) {
            for (auto &nic : segment_desc->devices)
                if (nic.name == context_.deviceName())
                    return doSetupConnection(nic.gid, nic.lid, qpNum());
        }
        // ...
    }
    // 否则走正常的远程握手流程
    // ...
}
```

**Loopback 模式说明**：
- 即使目标是本地 DRAM，也可以通过 RDMA NIC 访问
- 实际上网卡会检测到是本地访问，走硬件优化路径
- 延迟比真正的远程访问低，但比直接内存访问高

---

## 3. SSD 加载逻辑：先到 DRAM，再 RDMA 或直接使用

### 3.1 数据加载流程

当 KVCache 在 HBM 和 DRAM 都找不到时：

```
┌────────────────────────────────────────────────────────────────────────────────┐
│                       SSD → DRAM → 使用 的完整流程                               │
├────────────────────────────────────────────────────────────────────────────────┤
│                                                                                │
│   Step 1: 检查 L1 (GPU HBM) - Miss                                              │
│   ┌──────────────────────────────────────────────────────────────────────┐     │
│   │   HiRadixTree.match(tokens)                                          │     │
│   │   └── L1 Miss: 数据不在 GPU HBM                                       │     │
│   └──────────────────────────────────────────────────────────────────────┘     │
│                              │                                                 │
│                              ▼                                                 │
│   Step 2: 检查 L2 (Host DRAM) - Miss                                            │
│   ┌──────────────────────────────────────────────────────────────────────┐     │
│   │   HiRadixTree.match(tokens)                                          │     │
│   │   └── L2 Miss: 数据不在本地 Host DRAM                                  │     │
│   └──────────────────────────────────────────────────────────────────────┘     │
│                              │                                                 │
│                              ▼                                                 │
│   Step 3: 查询 L3 (Mooncake Store) - 查找元数据                                  │
│   ┌──────────────────────────────────────────────────────────────────────┐     │
│   │   MooncakeStore.query(key)                                           │     │
│   │   ├── Case A: 数据在远程 DRAM → 直接 RDMA 读取                         │     │
│   │   └── Case B: 数据只在 SSD/DFS → 需要先加载到 DRAM                     │     │
│   └──────────────────────────────────────────────────────────────────────┘     │
│                              │                                                 │
│                              ▼                                                 │
│   Step 4: 从 SSD/DFS 加载到 DRAM (Case B)                                       │
│   ┌──────────────────────────────────────────────────────────────────────┐     │
│   │                                                                      │     │
│   │   ┌─────────────┐    NVMe/POSIX    ┌─────────────┐                  │     │
│   │   │   DFS/SSD   │ ─────────────────► │ Local DRAM │                  │     │
│   │   │   (3FS)     │      读取文件      │  (Buffer)  │                  │     │
│   │   └─────────────┘                   └──────┬──────┘                  │     │
│   │                                            │                         │     │
│   │                     ┌──────────────────────┼───────────────────────┐ │     │
│   │                     │                      │                       │ │     │
│   │                     ▼                      ▼                       │ │     │
│   │   如果请求方是本地:                   如果请求方是远程:               │ │     │
│   │   ┌─────────────────────┐           ┌─────────────────────┐       │ │     │
│   │   │ 直接使用 (memcpy)   │           │ RDMA READ to 请求方  │       │ │     │
│   │   │ Local DRAM → GPU    │           │ Local → Remote DRAM │       │ │     │
│   │   │ (通过 PCIe DMA)     │           │ (通过 RDMA)          │       │ │     │
│   │   └─────────────────────┘           └─────────────────────┘       │ │     │
│   │                                                                      │     │
│   └──────────────────────────────────────────────────────────────────────┘     │
│                                                                                │
└────────────────────────────────────────────────────────────────────────────────┘
```

### 3.2 Mooncake Store Get 操作的 Fallback 逻辑

根据 [mooncake-store.md](../../docs/source/design/mooncake-store.md)：

> When persistence is enabled and the requested data is not found in the distributed memory pool, `Get` will **fall back to loading the data from SSD**.

```cpp
// 概念性伪代码 - Mooncake Store Get 逻辑
tl::expected<void, ErrorCode> Client::Get(
    const std::string& object_key, 
    std::vector<Slice>& slices) {
    
    // 1. 首先尝试从分布式内存池获取
    auto replica_list = master_->GetReplicaList(object_key);
    
    for (auto& replica : replica_list) {
        if (replica.status == COMPLETE && replica.in_memory) {
            // 数据在远程 DRAM，通过 RDMA 读取
            return transfer_engine_->submitTransfer(
                batch_id, 
                {TransferRequest::READ, local_buffer, 
                 replica.segment_id, replica.offset, size}
            );
        }
    }
    
    // 2. 内存池中没有，fallback 到 SSD/DFS
    if (persistence_enabled_) {
        std::string file_path = root_fs_dir_ + "/" + cluster_id_ + "/" + object_key;
        
        // 从 DFS 读取文件到本地 buffer
        // 注意：这里读取到的是 **本地** DRAM
        int fd = open(file_path.c_str(), O_RDONLY);
        read(fd, local_buffer, size);  // 或使用 3FS USRBIO 高性能接口
        close(fd);
        
        // 数据现在在本地 DRAM，可以直接使用
        return OK;
    }
    
    return ErrorCode::NOT_FOUND;
}
```

### 3.3 SSD 加载后的数据去向

```
┌────────────────────────────────────────────────────────────────────────────────┐
│               SSD 加载后数据的两种使用场景                                        │
├────────────────────────────────────────────────────────────────────────────────┤
│                                                                                │
│  场景 A: 请求方是本地节点 (数据在同一台机器)                                       │
│  ═══════════════════════════════════════════════════════════════════════       │
│                                                                                │
│   ┌──────────────┐      ┌──────────────┐      ┌──────────────┐                │
│   │   Local SSD  │ ───► │  Local DRAM  │ ───► │   GPU HBM    │                │
│   │   (DFS/3FS)  │ NVMe │  (L2 Buffer) │ PCIe │   (L1 Use)   │                │
│   └──────────────┘      └──────────────┘      └──────────────┘                │
│                                                                                │
│   路径: SSD → Local DRAM → GPU                                                 │
│   协议: NVMe/POSIX → PCIe DMA                                                  │
│   延迟: ~10-100μs (SSD) + ~1-5μs (PCIe)                                        │
│                                                                                │
│                                                                                │
│  场景 B: 请求方是远程节点 (数据需要跨节点传输)                                      │
│  ═══════════════════════════════════════════════════════════════════════       │
│                                                                                │
│   Node A (数据所在节点)                    Node B (请求方节点)                   │
│   ┌──────────────┐                        ┌──────────────┐                    │
│   │   Local SSD  │                        │   GPU HBM    │                    │
│   │   (DFS/3FS)  │                        │   (目标)     │                    │
│   └──────┬───────┘                        └──────▲───────┘                    │
│          │ NVMe                                  │ PCIe                       │
│          ▼                                       │                            │
│   ┌──────────────┐                        ┌──────────────┐                    │
│   │  Local DRAM  │ ═══════RDMA════════►   │  Host DRAM   │                    │
│   │  (Buffer)    │      ~1-10μs           │  (L2 Buffer) │                    │
│   └──────────────┘                        └──────────────┘                    │
│                                                                                │
│   路径: SSD → Local DRAM → [RDMA] → Remote DRAM → GPU                          │
│   协议: NVMe → RDMA → PCIe DMA                                                 │
│   延迟: ~10-100μs (SSD) + ~1-10μs (RDMA) + ~1-5μs (PCIe)                       │
│                                                                                │
└────────────────────────────────────────────────────────────────────────────────┘
```

### 3.4 为什么必须先加载到 DRAM？

**关键原因**：RDMA 只能操作 **已注册的内存区域**

```
┌────────────────────────────────────────────────────────────────────────────────┐
│                      为什么 SSD 数据必须先到 DRAM                                 │
├────────────────────────────────────────────────────────────────────────────────┤
│                                                                                │
│  RDMA 的工作前提：                                                               │
│  ═══════════════════════════════════════════════════════════════════════       │
│                                                                                │
│  1. 内存必须通过 ibv_reg_mr() 注册到 RDMA 子系统                                   │
│  2. 注册后获得 lkey (本地访问密钥) 和 rkey (远程访问密钥)                           │
│  3. 注册的内存被 Pin (锁定)，不会被换出到 swap                                     │
│  4. 网卡可以通过 rkey 直接 DMA 访问这块内存                                        │
│                                                                                │
│                                                                                │
│  SSD 数据不能直接 RDMA 的原因：                                                   │
│  ═══════════════════════════════════════════════════════════════════════       │
│                                                                                │
│  ❌ SSD 数据没有物理地址 (它在存储设备上，不是内存)                                  │
│  ❌ SSD 没有 rkey (RDMA 需要内存注册密钥)                                         │
│  ❌ RDMA 网卡无法理解 NVMe 命令 (它们是不同的硬件接口)                               │
│                                                                                │
│                                                                                │
│  正确的数据流：                                                                   │
│  ═══════════════════════════════════════════════════════════════════════       │
│                                                                                │
│   ┌─────────┐      ┌─────────────┐      ┌───────────┐      ┌──────────┐       │
│   │   SSD   │      │ Registered  │      │   RDMA    │      │  Remote  │       │
│   │  (3FS)  │ ───► │    DRAM     │ ───► │   NIC     │ ═══► │   DRAM   │       │
│   │         │ NVMe │  (有 rkey)   │      │ (读 rkey) │ RDMA │ (目标)   │       │
│   └─────────┘      └─────────────┘      └───────────┘      └──────────┘       │
│                         ▲                                                      │
│                         │                                                      │
│                    必须经过这一步！                                               │
│                                                                                │
└────────────────────────────────────────────────────────────────────────────────┘
```

---

## 4. 不同场景的完整数据路径

### 4.1 最佳情况：L1 命中 (HBM)

```
请求 → L1 GPU HBM (命中) → 直接使用
延迟: ~10 ns
带宽: ~2-3 TB/s
```

### 4.2 L2 命中：Host DRAM

```
请求 → L1 Miss → L2 Host DRAM (命中) → PCIe DMA → GPU HBM
延迟: ~100 ns + ~1-5 μs
带宽: ~32-64 GB/s (PCIe 限制)
```

### 4.3 L3 命中：Remote DRAM

```
请求 → L1 Miss → L2 Miss → L3 Query
     → Remote DRAM (命中) → RDMA READ → Local DRAM → PCIe → GPU
延迟: ~1-10 μs (RDMA) + ~1-5 μs (PCIe)
带宽: ~100-200 GB/s (多 NIC 聚合)
```

### 4.4 最差情况：SSD Fallback

```
请求 → L1 Miss → L2 Miss → L3 Query
     → Memory Pool Miss → SSD Fallback
     → DFS 读取 → Local DRAM → (可选 RDMA) → GPU
延迟: ~10-100 μs (SSD) + ~1-10 μs (传输) + ~1-5 μs (PCIe)
带宽: ~7 GB/s (NVMe SSD)
```

---

## 5. 代码参考

### 5.1 Transfer Engine - RDMA Transport

```cpp
// mooncake-transfer-engine/include/transport/rdma_transport/rdma_transport.h
class RdmaTransport : public Transport {
public:
    // 注册本地内存用于 RDMA
    int registerLocalMemory(void *addr, size_t length,
                            const std::string &location, 
                            bool remote_accessible,
                            bool update_metadata) override;

    // 提交传输请求
    Status submitTransfer(BatchID batch_id,
                          const std::vector<TransferRequest> &entries) override;
};
```

### 5.2 Transfer Request 结构

```cpp
// mooncake-transfer-engine/include/transport/transport.h
struct TransferRequest {
    enum OpCode { READ, WRITE };
    
    OpCode opcode;           // READ: 从远程读取, WRITE: 写入远程
    void *source;            // 本地内存地址
    SegmentID target_id;     // 目标段 ID
    uint64_t target_offset;  // 目标偏移
    size_t length;           // 传输长度
};
```

### 5.3 DFS 持久化配置

```bash
# 启动 Master 时启用 SSD 持久化
./mooncake_master \
    --root_fs_dir=/mnt/3fs/mooncake \  # DFS 挂载目录
    --global_file_segment_size=1099511627776  # 1TB SSD 空间
```

---

## 6. 总结

| 问题 | 答案 |
|------|------|
| **RDMA 如何访问远程 DRAM？** | 通过 `ibv_post_send(IBV_WR_RDMA_READ)` 命令，网卡直接访问已注册的远程内存，绕过远程 CPU |
| **本地 DRAM 访问用 PCIe 吗？** | **不是**。CPU 访问本地 DRAM 通过 DDR Memory Bus (~100ns)。GPU 访问 Host DRAM 才通过 PCIe (~1-5μs) |
| **SSD 加载逻辑？** | **必须先加载到 DRAM**。SSD 数据通过 NVMe 读取到本地 DRAM 的已注册内存区域，然后才能通过 RDMA 传输到远程或通过 PCIe 传输到 GPU |

关键洞察：
1. **RDMA 只能操作 DRAM**，不能直接操作 SSD
2. **SSD → DRAM** 是必经之路，无论是本地使用还是远程传输
3. **PCIe** 只在 GPU ↔ Host 传输时使用，CPU ↔ DRAM 不用 PCIe
