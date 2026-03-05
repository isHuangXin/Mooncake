# Mooncake SSD 异步写入实现：io_uring

## 问题

> Mooncake 是怎么实现向 SSD 中异步写的？

---

## 回答

Mooncake 使用 **Linux io_uring** 实现 SSD 的高性能异步写入。

---

## 一、架构总览

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                         SSD 异步写入架构                                     │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│   ┌───────────────────┐                                                     │
│   │   用户请求         │  Write KVCache to SSD                              │
│   │   (BatchTransfer)  │                                                    │
│   └─────────┬─────────┘                                                     │
│             │                                                               │
│             ▼                                                               │
│   ┌───────────────────────────────────────────────────────────────────┐    │
│   │                     IOUringTransport                               │    │
│   │              (mooncake-transfer-engine/tent/)                      │    │
│   │                                                                    │    │
│   │   • allocateSubBatch()   - 分配批处理结构                          │    │
│   │   • submitTransferTasks() - 提交异步请求                           │    │
│   │   • getTransferStatus()  - 检查完成状态                            │    │
│   └─────────┬─────────────────────────────────────────────────────────┘    │
│             │                                                               │
│             │ io_uring_submit() (非阻塞)                                    │
│             ▼                                                               │
│   ┌───────────────────────────────────────────────────────────────────┐    │
│   │                     Linux Kernel                                   │    │
│   │                                                                    │    │
│   │   ┌─────────────┐              ┌─────────────┐                    │    │
│   │   │ Submission  │              │ Completion  │                    │    │
│   │   │   Queue     │  ────────►   │   Queue     │                    │    │
│   │   │   (SQ)      │   DMA完成后  │   (CQ)      │                    │    │
│   │   └─────────────┘              └─────────────┘                    │    │
│   └─────────────────────────────────────────────────────────────────────┘  │
│             │                                                               │
│             │ DMA (Direct Memory Access)                                    │
│             ▼                                                               │
│   ┌───────────────────┐                                                     │
│   │   NVMe SSD        │                                                     │
│   │   (O_DIRECT)      │                                                     │
│   └───────────────────┘                                                     │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## 二、核心组件

### 2.1 IOUringTransport 类

**文件位置**: `mooncake-transfer-engine/tent/include/tent/transport/io_uring/io_uring_transport.h`

```cpp
class IOUringTransport : public Transport {
public:
    // 分配批处理结构
    virtual Status allocateSubBatch(SubBatchRef &batch, size_t max_size);
    
    // 提交传输任务（异步）
    virtual Status submitTransferTasks(SubBatchRef batch, 
                                       const std::vector<Request> &request_list);
    
    // 检查传输状态（非阻塞）
    virtual Status getTransferStatus(SubBatchRef batch, int task_id,
                                     TransferStatus &status);
    
    virtual const char *getName() const { return "io-uring"; }
};
```

### 2.2 IOUringSubBatch 结构

```cpp
struct IOUringSubBatch : public Transport::SubBatch {
    size_t max_size;                    // 最大任务数
    std::vector<IOUringTask> task_list; // 任务列表
    struct io_uring ring;               // io_uring 环形缓冲区
};
```

### 2.3 IOUringFileContext 类

```cpp
class IOUringFileContext {
public:
    explicit IOUringFileContext(const std::string& path) {
        // 优先使用 O_DIRECT（绕过 Page Cache）
        fd_ = open(path.c_str(), O_RDWR | O_DIRECT);
        if (fd_ < 0) {
            // 回退到 Buffered I/O
            fd_ = open(path.c_str(), O_RDWR);
        }
    }
    
    int getHandle() const { return fd_; }
};
```

---

## 三、核心流程

### 3.1 初始化 io_uring 队列

```cpp
Status IOUringTransport::allocateSubBatch(SubBatchRef& batch, size_t max_size) {
    auto io_uring_batch = Slab<IOUringSubBatch>::Get().allocate();
    io_uring_batch->max_size = max_size;
    io_uring_batch->task_list.reserve(max_size);
    
    // 初始化 io_uring 环形缓冲区
    int rc = io_uring_queue_init(max_size, &io_uring_batch->ring, 0);
    if (rc)
        return Status::InternalError("io_uring_queue_init failed");
    
    batch = io_uring_batch;
    return Status::OK();
}
```

### 3.2 提交异步写请求

```cpp
Status IOUringTransport::submitTransferTasks(
    SubBatchRef batch, const std::vector<Request>& request_list) {
    
    auto io_uring_batch = dynamic_cast<IOUringSubBatch*>(batch);
    
    for (auto& request : request_list) {
        // 创建任务
        io_uring_batch->task_list.push_back(IOUringTask{});
        auto& task = io_uring_batch->task_list.back();
        task.request = request;
        task.status_word = TransferStatusEnum::PENDING;
        
        // 获取文件上下文
        IOUringFileContext* context = findFileContext(request.target_id);
        
        // 获取 SQE (Submission Queue Entry)
        struct io_uring_sqe* sqe = io_uring_get_sqe(&io_uring_batch->ring);
        
        const size_t kPageSize = 4096;
        
        // 处理 GPU 内存或未对齐的数据
        if (Platform::getLoader().getMemoryType(request.source) == MTYPE_CUDA ||
            (uint64_t)request.source % kPageSize) {
            
            // 分配对齐的中转缓冲区
            posix_memalign(&task.buffer, kPageSize, request.length);
            
            if (request.opcode == Request::WRITE) {
                // GPU→DRAM 拷贝
                Platform::getLoader().copy(task.buffer, request.source, request.length);
                // 准备写操作
                io_uring_prep_write(sqe, context->getHandle(), 
                                   task.buffer, request.length, request.target_offset);
            }
        } else {
            // DRAM 且对齐，直接使用
            if (request.opcode == Request::WRITE)
                io_uring_prep_write(sqe, context->getHandle(), 
                                   request.source, request.length, request.target_offset);
        }
        
        // 关联任务（用于完成时回调）
        sqe->user_data = (uintptr_t)&task;
    }
    
    // ★ 批量提交到内核（非阻塞，立即返回）
    int rc = io_uring_submit(&io_uring_batch->ring);
    
    return Status::OK();
}
```

### 3.3 检查完成状态（非阻塞轮询）

```cpp
Status IOUringTransport::getTransferStatus(SubBatchRef batch, int task_id,
                                           TransferStatus& status) {
    auto io_uring_batch = dynamic_cast<IOUringSubBatch*>(batch);
    auto& task = io_uring_batch->task_list[task_id];
    
    status = TransferStatus{task.status_word, task.transferred_bytes};
    
    if (task.status_word == TransferStatusEnum::PENDING) {
        struct io_uring_cqe* cqe = nullptr;
        
        // ★ 非阻塞检查完成队列
        int err = io_uring_peek_cqe(&io_uring_batch->ring, &cqe);
        
        if (err == -EAGAIN) {
            return Status::OK();  // 还没完成，继续等待
        }
        
        // 处理完成事件
        auto completed_task = (IOUringTask*)cqe->user_data;
        if (completed_task) {
            if (cqe->res < 0) {
                completed_task->status_word = TransferStatusEnum::FAILED;
            } else {
                // 如果是读操作且使用了中转缓冲区，需要拷贝回去
                if (completed_task->buffer && 
                    completed_task->request.opcode == Request::READ) {
                    Platform::getLoader().copy(completed_task->request.source,
                                               completed_task->buffer,
                                               completed_task->request.length);
                }
                completed_task->status_word = TransferStatusEnum::COMPLETED;
                completed_task->transferred_bytes = completed_task->request.length;
            }
        }
        
        // 标记 CQE 已处理
        io_uring_cqe_seen(&io_uring_batch->ring, cqe);
    }
    
    return Status::OK();
}
```

---

## 四、io_uring 工作原理

### 4.1 双环形缓冲区设计

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                     io_uring 双环形缓冲区                                    │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│   用户空间                              内核空间                             │
│   ┌─────────────────────┐              ┌─────────────────────┐             │
│   │                     │              │                     │             │
│   │   Submission Queue  │              │   io_uring 内核     │             │
│   │   ┌───┬───┬───┬───┐ │  io_uring   │   处理线程          │             │
│   │   │SQE│SQE│SQE│...│ │ ─────────►  │                     │             │
│   │   └───┴───┴───┴───┘ │  _submit()  │   ┌─────────────┐   │             │
│   │                     │              │   │  处理 I/O   │   │             │
│   │   SQ Tail ──────────┼──►           │   │  请求       │   │             │
│   │                     │              │   └──────┬──────┘   │             │
│   └─────────────────────┘              │          │          │             │
│                                        │          │ DMA      │             │
│                                        │          ▼          │             │
│   ┌─────────────────────┐              │   ┌─────────────┐   │             │
│   │                     │              │   │    SSD      │   │             │
│   │   Completion Queue  │              │   │   (NVMe)    │   │             │
│   │   ┌───┬───┬───┬───┐ │  完成通知   │   └──────┬──────┘   │             │
│   │   │CQE│CQE│CQE│...│ │ ◄─────────  │          │          │             │
│   │   └───┴───┴───┴───┘ │              │          │          │             │
│   │                     │              │          ▼          │             │
│   │   ◄─── CQ Head      │              │   写入 CQE          │             │
│   │                     │              │                     │             │
│   └─────────────────────┘              └─────────────────────┘             │
│                                                                             │
│   SQE = Submission Queue Entry (提交队列条目)                               │
│   CQE = Completion Queue Entry (完成队列条目)                               │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 4.2 异步流程时序图

```
时间 ──────────────────────────────────────────────────────────────────────────►

用户线程:
    │
    ├─[准备 SQE]──[io_uring_submit()]──[继续其他工作...]──[io_uring_peek_cqe()]─►
    │                    │                                        │
    │                    │ 立即返回                                │ 检查完成
    │                    ▼                                        ▼
    │
内核线程:
    │                    │
    │              ┌─────┴─────┐
    │              │ 收到 SQE  │
    │              │ 准备 DMA  │
    │              └─────┬─────┘
    │                    │
    │                    ▼
NVMe SSD:
    │              ┌─────────────────────────────────────┐
    │              │        DMA 传输 + Flash 写入         │
    │              └─────────────────────┬───────────────┘
    │                                    │
    │                                    ▼ 完成中断
内核线程:
    │                              ┌─────┴─────┐
    │                              │ 写入 CQE  │
    │                              │ 到完成队列│
    │                              └───────────┘
```

---

## 五、关键设计点

### 5.1 O_DIRECT 绕过 Page Cache

```cpp
fd_ = open(path.c_str(), O_RDWR | O_DIRECT);
```

| 模式 | Page Cache | 特点 |
|------|------------|------|
| 普通 I/O | 经过 | 内核会缓存，可能延迟写入 |
| **O_DIRECT** | **绕过** | 直接 DMA 到 SSD，延迟可预测 |

### 5.2 内存对齐要求

```cpp
const size_t kPageSize = 4096;

// O_DIRECT 要求缓冲区和偏移量都是 4KB 对齐
if ((uint64_t)request.source % kPageSize) {
    // 分配对齐的中转缓冲区
    posix_memalign(&task.buffer, kPageSize, request.length);
}
```

### 5.3 GPU 内存处理

```cpp
if (Platform::getLoader().getMemoryType(request.source) == MTYPE_CUDA) {
    // GPU 内存不能直接用于 io_uring
    // 需要先拷贝到对齐的 DRAM 缓冲区
    posix_memalign(&task.buffer, kPageSize, request.length);
    Platform::getLoader().copy(task.buffer, request.source, request.length);
}
```

**原因**: io_uring 只能访问 CPU 可见的内存，GPU HBM 需要先拷贝到 Host DRAM。

---

## 六、与其他异步 I/O 方案对比

| 方案 | 真异步？ | 批量提交 | 零拷贝 | 缺点 |
|------|---------|---------|--------|------|
| 同步 `write()` | ❌ | ❌ | ❌ | 阻塞线程 |
| POSIX AIO | △ | △ | ❌ | 内部用线程池模拟 |
| `libaio` | ✅ | ✅ | ✅ | 仅支持 O_DIRECT |
| **io_uring** | **✅** | **✅** | **✅** | 需要较新内核 (5.1+) |

### 为什么选择 io_uring？

1. **真正的异步**：提交后立即返回，内核异步处理
2. **高效批量**：多个请求一次系统调用提交
3. **零拷贝**：对齐数据直接 DMA
4. **统一接口**：支持文件、网络、定时器等
5. **高性能**：减少系统调用次数和上下文切换

---

## 七、使用示例

```cpp
// 1. 分配批处理
Transport::SubBatchRef batch;
io_uring_transport->allocateSubBatch(batch, 100);

// 2. 准备写请求
std::vector<Request> requests;
requests.push_back(Request{
    .opcode = Request::WRITE,
    .source = kvcache_data,           // 源数据地址
    .target_id = ssd_segment_id,      // 目标 SSD segment
    .target_offset = file_offset,     // 文件偏移
    .length = data_size               // 数据大小
});

// 3. 提交（异步，立即返回）
io_uring_transport->submitTransferTasks(batch, requests);

// 4. 做其他工作...
DoOtherWork();

// 5. 检查完成
TransferStatus status;
while (true) {
    io_uring_transport->getTransferStatus(batch, 0, status);
    if (status.status == TransferStatusEnum::COMPLETED) break;
    if (status.status == TransferStatusEnum::FAILED) HandleError();
    // 可以继续做其他事情
}

// 6. 释放批处理
io_uring_transport->freeSubBatch(batch);
```

---

## 八、总结

| 问题 | 答案 |
|------|------|
| Mooncake 用什么实现 SSD 异步写？ | **Linux io_uring** |
| 核心 API | `io_uring_submit()` + `io_uring_peek_cqe()` |
| 是否阻塞？ | **否**，提交后立即返回 |
| 文件打开模式 | **O_DIRECT**（绕过 Page Cache） |
| GPU 数据如何处理？ | 先拷贝到对齐的 Host DRAM |
| 对齐要求 | 4KB 对齐（O_DIRECT 要求） |

### 核心优势

1. **非阻塞提交**：`io_uring_submit()` 立即返回
2. **批量操作**：多个 I/O 请求一次提交
3. **零拷贝**：对齐数据直接 DMA 到 SSD
4. **高吞吐**：减少系统调用和上下文切换开销

---

## 参考文件

- `mooncake-transfer-engine/tent/include/tent/transport/io_uring/io_uring_transport.h`
- `mooncake-transfer-engine/tent/src/transport/io_uring/io_uring_transport.cpp`
