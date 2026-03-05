# Mooncake 技术问答集

本文件夹包含关于 Mooncake 架构和实现的技术问答记录。

## 目录

| 文件 | 主题 | 关键词 |
|------|------|--------|
| [01_KVCache_Hot_Cold_Data_Sensing.md](./01_KVCache_Hot_Cold_Data_Sensing.md) | KVCache 冷热数据感知机制 | Soft Pin, Lease, Eviction, LRU |
| [02_KVCache_Tiered_Storage_Architecture.md](./02_KVCache_Tiered_Storage_Architecture.md) | KVCache 分级存储架构 | GPU HBM, CPU DRAM, SSD, L1/L2/L3 |
| [03_Distributed_DRAM_SSD_Implementation.md](./03_Distributed_DRAM_SSD_Implementation.md) | 分布式 DRAM 和 SSD 实现 | Transfer Engine, RDMA, 3FS, DFS |
| [04_KVCache_Flat_Memory_System_Implementation.md](./04_KVCache_Flat_Memory_System_Implementation.md) | Flat Memory 系统实现 | 架构修改, TTFT 影响, 代码方案 |
| [05_KVCache_Flat_Memory_System_Summary.md](./05_KVCache_Flat_Memory_System_Summary.md) | Flat Memory 实现总结 | 文件清单, 快速开始, 缓解策略 |
| [06_Transfer_Engine_Segment_BatchTransfer.md](./06_Transfer_Engine_Segment_BatchTransfer.md) | Transfer Engine 核心抽象 | Segment, BatchTransfer, RDMA, 统一接口 |
| [07_Mooncake_Core_Modules.md](./07_Mooncake_Core_Modules.md) | Mooncake 核心模块解析 | transfer-engine, store, integration, 架构 |
| [08_SSD_Role_Persistence_vs_Capacity_Extension.md](./08_SSD_Role_Persistence_vs_Capacity_Extension.md) | SSD 角色分析 | 持久化层, 备份, 容量扩展, Offload |
| [09_TTFT_Benchmark_Data_Source_Analysis.md](./09_TTFT_Benchmark_Data_Source_Analysis.md) | TTFT 测试数据来源分析 | 调度算法, 缓存命中, HBM, DRAM |
| [10_RDMA_Remote_DRAM_Access_And_SSD_Loading.md](./10_RDMA_Remote_DRAM_Access_And_SSD_Loading.md) | RDMA 远程访问与 SSD 加载 | RDMA, PCIe, Memory Bus, Fallback |

## 问答摘要

### Q1: Mooncake 如何感知 KVCache 的冷热？

**核心机制**：
- **Soft Pin**: 热数据标记，TTL 30分钟
- **Lease**: 读写保护，TTL 5秒
- **Eviction**: LRU/FIFO 策略，95% 水位触发

### Q2: KVCache 分级存储的数据放置策略？

**三级存储**：
- **L1 GPU HBM**: 正在计算的热数据
- **L2 Host DRAM**: 本地温数据缓存
- **L3 Mooncake Store**: 分布式冷数据池（DRAM + DFS）

**SSD 答案**: 是多节点共享的分布式文件系统 (DFS)，不是单节点 SSD

### Q3: 分布式 DRAM 和 SSD 的实现方式？

**分布式 DRAM**:
- 通过 Transfer Engine + RDMA 实现
- 每个节点注册本地内存段
- Master 统一管理全局资源

**分布式 SSD**:
- 通过分布式文件系统 (DFS) 实现
- **推荐**: DeepSeek 的 3FS
- 所有节点挂载相同路径

### Q4 & Q5: KVCache Flat Memory System 实现

**核心思想**:
- 将 GPU HBM、DRAM、SSD 视为同一层级
- 不再区分冷热数据，用户显式指定存储位置
- 移除自动 Eviction 和 Soft Pin 机制

**TTFT 影响**:
- 分层架构: ~500ns (80%热数据在HBM)
- 扁平架构: ~3.3μs (数据均匀分布)
- 增长约 6 倍

**缓解策略**:
- 使用 LATENCY_FIRST 放置策略
- 混合策略：Flat Memory + 智能预取

### Q6: Transfer Engine 的 Segment 和 BatchTransfer 是什么？

**Segment (存储抽象)**:
- 代表一段可被远程读写的连续地址空间
- 可以是 DRAM、VRAM (GPU HBM)、NVMe SSD
- 通过 `registerLocalMemory()` 创建，`openSegment()` 访问

**BatchTransfer (传输抽象)**:
- 封装批量数据传输操作
- 支持 READ/WRITE 两种方向
- 支持非连续数据块的批量传输

**核心 API**:
```cpp
// Segment
engine->registerLocalMemory(addr, size, location);
auto segment_id = engine->openSegment("remote_node");

// BatchTransfer
auto batch_id = engine->allocateBatchID(batch_size);
engine->submitTransfer(batch_id, requests);
engine->getTransferStatus(batch_id, task_id, status);
engine->freeBatchID(batch_id);
```

**屏蔽的底层细节**:
- RDMA QP 元数据交换
- Memory Registration 和 RKEY 管理
- 多 NIC 选择和拓扑感知
- 协议差异 (RDMA/TCP/NVLink)

### Q7: Mooncake 最核心的是哪几个模块？

**四大核心模块**:
1. **mooncake-transfer-engine** ⭐⭐⭐⭐⭐ - 最底层最核心，高性能 RDMA 传输
2. **mooncake-store** ⭐⭐⭐⭐ - KVCache 分布式存储引擎
3. **mooncake-integration** ⭐⭐⭐ - vLLM/SGLang 集成层
4. **mooncake-common** ⭐⭐ - 公共基础设施

**依赖关系**: `vLLM/SGLang → integration → store → transfer-engine → common`

### Q8: SSD 在 Mooncake 中是持久化层还是容量扩展？

**答案**: SSD 主要作为**持久化/备份层**，而非 DRAM 的容量扩展。

**关键点**:
- SSD 存储的数据与 DRAM 中**重复**（是备份副本）
- 不是"DRAM 放不下的数据放 SSD"
- 用途：故障恢复、跨实例共享、长期持久化
- Prefetch 主要从 Remote DRAM 读取，而非 SSD

**设计理由**: LLM 推理对延迟敏感，SSD 的 ~10μs 延迟无法满足 prefill 需求

### Q9: 调度算法 TTFT 对比测试的数据来源？

**答案**: TTFT 测试主要反映从 **HBM 和 DRAM** 读取 KVCache 的性能，而非 SSD。

**测试结果**:
- 全局缓存感知: 3.07s（最优）
- 本地缓存感知: 3.58s
- 负载均衡: 5.27s
- 随机调度: 19.65s

**差异原因**: 缓存命中率不同导致 DRAM Prefetch 量不同，而非 SSD 读取差异

### Q10: RDMA 如何访问远程 DRAM？SSD 加载逻辑是什么？

**RDMA 访问远程 DRAM**:
- 通过 `ibv_post_send(IBV_WR_RDMA_READ)` 命令
- 网卡直接 DMA 访问已注册的远程内存，绕过远程 CPU
- 内存必须先通过 `ibv_reg_mr()` 注册获得 rkey

**本地 DRAM 访问**:
- **CPU 访问本地 DRAM**: 通过 DDR Memory Bus (~100ns)，**不是 PCIe**
- **GPU 访问 Host DRAM**: 通过 PCIe DMA (~1-5μs)

**SSD 加载逻辑**:
- **必须先加载到 DRAM**，因为 RDMA 只能操作内存，不能直接操作 SSD
- 路径: `SSD → Local DRAM (注册内存) → [RDMA] → Remote DRAM → GPU`

---

*最后更新: 2026-02-27*
