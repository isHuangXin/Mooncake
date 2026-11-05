# Mooncake — FlatCake 子项目规范

## Author

Xin Huang — [@isHuangXin](https://github.com/isHuangXin)

> 本文档是 Mooncake 在 FlatCake 项目中的开发规范。上层规范：`../../../.claude/CLAUDE.md`。
> 基线 `upstream-v0.3.13`；定制分支 `upstream-v0.3.13-flatcake-b200`。
> 代码行号、数据格式与接口细节均以 v0.3.13 实现为准。
> 只迁移 Flatcake 所需提交，不改造上游分配、淘汰或传输架构。

---

## 0. 本文档范围

仅规范 Mooncake 在 FlatCake 项目中涉及的模块。通用 API 用法见 `mooncake-api` skill。

---

## 1. Mooncake 的两个角色

| 角色 | 说明 |
|------|------|
| **对比基线（主要）** | 提供论文中的 tiered HBM–DRAM–SSD 层级，对应实验 `experiment_4_with_l3_mooncake_ssd` 与 `experiment_5_with_l3_mooncake_ssd_gds`（Mooncake-GDS）。基线机制与失效原因见 [`mooncake-tiered-baseline.md`](../../../.claude/mooncake-tiered-baseline.md) |
| **RDMA TransferEngine 提供方** | FlatCake 原生 `RDMABackend` 链接 Mooncake `transfer_engine`，用于跨节点 DRAM。主仓库 `USE_RDMA=ON` 时以 CMake 子项目构建（`WITH_TE=ON`、`WITH_STORE=OFF`） |

> FlatCake 自身**不经过** MooncakeStore 存储 KV —— 它走 SGLang 的 `HiCacheStorage`
> 接口直连原生 Flat 后端。Mooncake store 路径只在基线实验中使用。

---

## 2. Flat Memory 相关的核心文件

### 2.1 存储引擎 (mooncake-store)

> 行号随上游变动，按符号名 grep，不要依赖记录的行号。

| 文件 / 符号 | 说明 |
|------|------|
| `mooncake-store/src/real_client.cpp` | RealClient 核心 — batch_put/batch_get 逻辑 |
| `RealClient::batch_get_into_internal()` | 判断数据在 DRAM 还是 SSD 并分流 — SSD 取回路径入口 |
| `batch_get_into_offload_object_internal()` | 从 SSD 读取 KVCache（RPC + TransferEngine 读回） |
| `mooncake-store/include/allocation_strategy.h` | 分配策略接口 |
| `mooncake-store/include/eviction_strategy.h` | LRU/FIFO 淘汰策略 — 基线的 SSD 落盘触发源 |
| `mooncake-store/include/file_storage.h` | SSD 文件存储接口 |

基线实验新增的遥测（见 §7）：per-key read provenance、SSD I/O 快照与 100 ms
completion bucket、`MOONCAKE_PARALLEL_READ` 开关与 O_DIRECT 对齐串行回退、
DRAM/SSD 读写带宽累计统计。这些用于与 FlatCake 对齐口径，不改变基线分层语义。

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

## 4. KVCache 数据流（基线）

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

> 这条路径上有两处构成论文 §2.2 Observation 2 的代价：SSD 命中必须先落到临时 DRAM
> buffer 再传回（host staging 争用），且 SSD 空间按淘汰增量填充、按 FIFO bucket 回收
> （同会话前缀散落在非连续 LBA）。FlatCake 的对应做法见上层 `CLAUDE.md` §2。

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
| `MOONCAKE_PARALLEL_READ` | 并行 SSD 读；关闭时走 O_DIRECT 对齐串行回退 | — |

---

## 7. 代码修改约束

- Mooncake 代码修改留在 `third_party/mooncake/`，不要混进主仓库或 SGLang 子模块
- 修改必须与顶层 `CLAUDE.md` 和 SGLang 子项目规范保持一致
- 所有 Flatcake 相关修改标注 `// FLAT_MEMORY:` 注释前缀
- 核心传输引擎（RDMA 路径）不得修改；基线侧只做遥测扩展
- **不要为了提升基线表现而改掉分层语义** —— 基线必须保持 LRU 淘汰落盘、host staging
  恢复与 FIFO bucket 回收，否则论文的对比失去意义
- C++ 代码遵循 Mooncake 现有风格（`.clang-format`）
- 该子模块的上游 PR 约定见 `AGENTS.md`（仓库根的 `CLAUDE.md` 即指向它）

---

## 8. 详细文档参考

| 文档 | 说明 |
|------|------|
| [顶层规范](../../../.claude/CLAUDE.md) | FlatCake 项目总规范 |
| [Tiered 基线说明](../../../.claude/mooncake-tiered-baseline.md) | 基线机制与失效原因（论文 §2.2） |
| [SGLang 子项目规范](../../sglang/.claude/CLAUDE.md) | SGLang 侧接入点与不变量 |
| `mooncake-api` skill | Mooncake Python API 完整参考 |
| [Mooncake 官方文档](https://kvcache-ai.github.io/Mooncake/) | 上游完整文档 |
