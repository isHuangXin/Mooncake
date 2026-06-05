# Mooncake — Flat Memory System 子项目规范

> 本文档是 Mooncake 在 KVCache Flat Memory System 项目中的开发规范。
> 上层规范: `/home/huangxin/code_list/flat-memory-system/.claude/CLAUDE.md`
> 版本: Mooncake v0.3.10

---

## 0. 本文档范围

本文档仅规范 Mooncake 在 Flat Memory System 项目中涉及的模块。Mooncake 的通用 API 用法请参考 `.claude/skills/mooncake-api/SKILL.md`。

---

## 1. Mooncake 在 Flat Memory 项目中的角色

Mooncake 是 KVCache 分布式存储引擎，在 Flat Memory 项目中承担：

| 职责 | 说明 |
|------|------|
| **KVCache 分布式存储** | 通过 MooncakeDistributedStore 提供 KVCache 的 Put/Get/Exists 操作 |
| **RDMA 数据传输** | 通过 TransferEngine 实现 GPU HBM 间的零拷贝 RDMA 传输 |
| **SSD 持久化** | DRAM 满时通过 LRU 淘汰将冷数据异步写入 SSD |
| **元数据管理** | Master 节点维护全局 KVCache 元数据索引，路由数据访问请求 |

---

## 2. Flat Memory 相关的核心文件

### 2.1 存储引擎 (mooncake-store)

| 文件 | 说明 | Flat Memory 改动点 |
|------|------|-------------------|
| `mooncake-store/src/real_client.cpp` | RealClient 核心 — 实现 batch_put/batch_get 逻辑 | Flat Memory 需要改造分配策略 |
| `mooncake-store/src/real_client.cpp:1932` | `batch_get_into_internal()` — 判断数据在 DRAM 还是 SSD 并分流 | SSD 取回路径的入口 |
| `mooncake-store/src/real_client.cpp:2724` | `batch_get_into_offload_object_internal()` — 从 SSD 读取 KVCache | RPC + TransferEngine 读回 |
| `mooncake-store/include/allocation_strategy.h` | 分配策略接口 | Flat Memory 需要替换为统一编址策略 |
| `mooncake-store/include/eviction_strategy.h` | LRU/FIFO 淘汰策略 | Flat Memory 不使用淘汰，需要绕过或禁用 |
| `mooncake-store/include/file_storage.h` | SSD 文件存储接口 | Flat Memory 可能需要替换 I/O 方式 (io_uring) |

### 2.2 传输引擎 (mooncake-transfer-engine)

| 文件 | 说明 |
|------|------|
| `mooncake-transfer-engine/include/` | TransferEngine C/C++ API — RDMA/TCP 传输 |
| `mooncake-transfer-engine/src/` | 传输引擎实现 — installTransport, registerLocalMemory, submitTransfer |

### 2.3 Master 服务

| 组件 | 说明 |
|------|------|
| Master Server | 全局元数据管理 — KVCache 位置索引 (DRAM/SSD/哪个节点) |
| HTTP Metadata Server | 元数据 HTTP 接口 |
| 租约管理 | KVCache 数据的 TTL 管理 |

---

## 3. SSD KVCache 数据结构

Mooncake 写入 SSD 的 KVCache 使用自描述格式：

```
+--------+--------+------------------------+-----------------+
| k_len  | v_len  |    index (prefix hash) |    kvcache data |
| (int)  | (int)  |       (hash value)     |   (raw bytes)   |
+--------+--------+------------------------+-----------------+
```

- `k_len`: key 的长度
- `v_len`: value 的长度
- `index`: prefix hash 值，L3 SSD 索引的关键字段
- `kvcache`: 实际的 KV 数据（裸字节）

**优势:** 索引与数据原子绑定，写入完成时索引自然就绑定好了，不需要额外的索引文件。

---

## 4. KVCache 数据流

### 4.1 Tiered Memory 基线（当前实现）

```
KVCache 写入:
  SGLang write_through -> MooncakeDistributedStore.batch_put_from()
      -> RealClient 选择目标节点 (AllocationStrategy)
      -> TransferEngine RDMA 写入目标节点 DRAM
      -> Master 记录元数据 (key -> {node, addr, size})

DRAM -> SSD 淘汰:
  DRAM 满 -> LRU 选择冷数据 -> offload_object()
      -> 将 KVCache 序列化为 [k_len, v_len, index, kvcache]
      -> POSIX write 写入 SSD 文件
      -> Master 更新元数据 (标记为 local_disk_replica)

SSD -> DRAM 取回:
  batch_get_into_internal() -> BatchQuery(keys) 查询 Master
      -> 发现 is_local_disk_replica() == true
      -> batch_get_into_offload_object_internal()
      -> RPC 请求目标节点 pread() SSD -> 临时 DRAM buffer
      -> TransferEngine 传回调用方
```

### 4.2 Flat Memory 目标

```
KVCache 写入:
  FlatMemoryManager.Put(key, data, size)
      -> PlacementStrategy 选择目标段 (DRAM/SSD, local/remote)
      -> StorageBackend.Write() 写入目标介质
      -> 统一索引更新

KVCache 读取:
  FlatMemoryManager.Get(key)
      -> 查询内存索引 (block_index_)
      -> Resolve 地址 -> 确定在哪个 StorageBackend
      -> StorageBackend.Read() 直接读取
      -> 无需 DRAM->SSD 迁移
```

---

## 5. 关键 API 参考

### 5.1 MooncakeDistributedStore (Python)

```python
from mooncake.store import MooncakeDistributedStore

store = MooncakeDistributedStore()
store.setup(hostname, metadata_server, global_segment_size,
            local_buffer_size, protocol, rdma_devices, master_addr)

# KVCache 操作
store.batch_put_from(keys, ptrs, sizes)    # 零拷贝写入
store.batch_get_into(keys, ptrs, sizes)    # 零拷贝读取（自动处理 DRAM/SSD）
store.is_exist(key)                        # 检查存在

store.close()
```

### 5.2 TransferEngine (C API)

```c
// 初始化
void* engine = createTransferEngine(local_addr, metadata_uri);
installTransport(engine, "rdma", NULL);

// 内存注册
registerLocalMemory(engine, buffer, size, "segment_name", 1);
int segment_id = openSegment(engine, remote_addr);

// 传输
BatchID batch = allocateBatchID(engine, 1);
submitTransfer(engine, batch, CYCLIC, local, remote, size, ...);
while (getTransferStatus(engine, batch) == 0) {}  // polling
freeBatchID(engine, batch);
```

---

## 6. 环境变量

| 变量 | 说明 | 默认值 |
|------|------|--------|
| `MC_METADATA_SERVER` | 元数据服务器 URL | — |
| `MC_FORCE_TCP` | 强制使用 TCP（调试用） | false |
| `MC_LOG_LEVEL` | 日志级别 (0=INFO, 1=WARN, 2=ERROR) | 0 |
| `MC_MS_FILTERS` | RDMA 设备过滤 (如 "mlx5_0") | 全部设备 |
| `MC_STORE_USE_HUGEPAGE` | 启用 Hugepage | false |
| `MC_STORE_MEMCPY` | 启用本地 memcpy 优化 | false |

---

## 7. 代码修改约束

- Mooncake 代码修改在 `third_party/mooncake/` 中进行
- 修改必须与顶层 CLAUDE.md 和 SGLang CLAUDE.md 保持一致
- 所有 Flat Memory 相关修改需标注 `// FLAT_MEMORY:` 注释前缀
- Mooncake 的核心传输引擎（RDMA 路径）不得修改，仅扩展存储层和分配策略
- C++ 代码遵循 Mooncake 现有风格（参考 `.clang-format`）

---

## 8. 详细文档参考

| 文档 | 说明 |
|------|------|
| [顶层规范](../../../.claude/CLAUDE.md) | Flat Memory System 项目总规范 |
| [Issues 跟踪](../../../.claude/issues.md) | 待解决问题清单 |
| [Mooncake API Skill](.claude/skills/mooncake-api/SKILL.md) | Mooncake Python API 完整参考 |
| [Ghost Node 分析](../../../experiments/experiment_4_single_node_Flat_Memory/ghost_node_and_ssd_kvcache_reuse_analysis.md) | SSD KVCache 取回路径分析 |
| [Mooncake 官方文档](https://kvcache-ai.github.io/Mooncake/) | Mooncake 完整文档 |
