// Copyright 2024 KVCache.AI
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/**
 * @file unified_segment.h
 * @brief KVCache Flat Memory System - 统一段抽象接口
 * 
 * 定义统一的段访问接口，使得 HBM、DRAM（本地/远程）、SSD（本地/远程）
 * 可以通过相同的接口访问。
 */

#pragma once

#include <cstdint>
#include <string>
#include <memory>
#include <functional>
#include <atomic>
#include <vector>

namespace mooncake {
namespace flat {

/**
 * @brief 扩展的存储介质类型，区分本地和远程
 */
enum class UnifiedMediumType {
    GPU_HBM,           // 本地 GPU HBM
    LOCAL_DRAM,        // 本地 Host DRAM
    REMOTE_DRAM,       // 远程 DRAM (通过 RDMA)
    LOCAL_SSD,         // 本地 NVMe SSD
    REMOTE_SSD,        // 远程 SSD (通过 DFS/3FS)
};

inline const char* MediumTypeToString(UnifiedMediumType type) {
    switch (type) {
        case UnifiedMediumType::GPU_HBM: return "GPU_HBM";
        case UnifiedMediumType::LOCAL_DRAM: return "LOCAL_DRAM";
        case UnifiedMediumType::REMOTE_DRAM: return "REMOTE_DRAM";
        case UnifiedMediumType::LOCAL_SSD: return "LOCAL_SSD";
        case UnifiedMediumType::REMOTE_SSD: return "REMOTE_SSD";
        default: return "UNKNOWN";
    }
}

/**
 * @brief 统一地址结构
 * 
 * 64位地址格式：[16位段ID][48位段内偏移]
 * 支持最多 65536 个段，每段最大 256TB
 */
struct UnifiedAddress {
    uint16_t segment_id;
    uint64_t offset;
    
    UnifiedAddress() : segment_id(0), offset(0) {}
    UnifiedAddress(uint16_t sid, uint64_t off) : segment_id(sid), offset(off) {}
    
    // 转换为 64 位线性地址
    uint64_t ToLinear() const {
        return (static_cast<uint64_t>(segment_id) << 48) | 
               (offset & 0xFFFFFFFFFFFFULL);
    }
    
    // 从线性地址解析
    static UnifiedAddress FromLinear(uint64_t addr) {
        UnifiedAddress ua;
        ua.segment_id = static_cast<uint16_t>(addr >> 48);
        ua.offset = addr & 0xFFFFFFFFFFFFULL;
        return ua;
    }
    
    bool operator==(const UnifiedAddress& other) const {
        return segment_id == other.segment_id && offset == other.offset;
    }
    
    bool IsValid() const { return segment_id != 0 || offset != 0; }
};

/**
 * @brief 统一段描述符
 */
struct UnifiedSegmentDesc {
    uint16_t segment_id = 0;
    std::string segment_name;
    UnifiedMediumType medium = UnifiedMediumType::LOCAL_DRAM;
    std::string node_id;              // 所在节点 ID
    uint64_t base_address = 0;        // 物理基地址（介质内）
    size_t capacity = 0;
    std::atomic<size_t> used{0};
    
    // 性能特征
    uint32_t latency_ns = 100;        // 预估延迟 (ns)
    uint32_t bandwidth_mbps = 100000; // 预估带宽 (MB/s)
    
    // 访问配置
    std::string rdma_endpoint;        // RDMA 端点（用于远程 DRAM）
    std::string nvme_device;          // NVMe 设备路径（本地 SSD）
    std::string dfs_path;             // DFS 路径（远程 SSD）
    int gpu_id = -1;                  // GPU ID（用于 HBM）
    
    bool IsLocal() const {
        return medium == UnifiedMediumType::GPU_HBM || 
               medium == UnifiedMediumType::LOCAL_DRAM ||
               medium == UnifiedMediumType::LOCAL_SSD;
    }
    
    bool IsRemote() const { return !IsLocal(); }
    
    size_t AvailableSpace() const {
        size_t current_used = used.load(std::memory_order_relaxed);
        return capacity > current_used ? capacity - current_used : 0;
    }
};

/**
 * @brief I/O 完成回调
 */
using IOCompletionCallback = std::function<void(int status, size_t bytes_transferred)>;

/**
 * @brief 统一段访问接口 - 核心抽象
 * 
 * 所有类型的存储介质都实现这个接口，提供统一的读写操作。
 */
class IUnifiedSegment {
public:
    virtual ~IUnifiedSegment() = default;
    
    // ==================== 同步 I/O ====================
    
    /**
     * @brief 同步读取
     * @param offset 段内偏移
     * @param buffer 目标缓冲区（必须已分配）
     * @param length 读取长度
     * @return 0 成功，<0 失败
     */
    virtual int Read(uint64_t offset, void* buffer, size_t length) = 0;
    
    /**
     * @brief 同步写入
     * @param offset 段内偏移
     * @param data 源数据
     * @param length 写入长度
     * @return 0 成功，<0 失败
     */
    virtual int Write(uint64_t offset, const void* data, size_t length) = 0;
    
    // ==================== 异步 I/O ====================
    
    /**
     * @brief 异步读取
     * @param offset 段内偏移
     * @param buffer 目标缓冲区
     * @param length 读取长度
     * @param callback 完成回调
     * @return 0 成功提交，<0 失败
     */
    virtual int AsyncRead(uint64_t offset, void* buffer, size_t length,
                          IOCompletionCallback callback) = 0;
    
    /**
     * @brief 异步写入
     * @param offset 段内偏移
     * @param data 源数据
     * @param length 写入长度
     * @param callback 完成回调
     * @return 0 成功提交，<0 失败
     */
    virtual int AsyncWrite(uint64_t offset, const void* data, size_t length,
                           IOCompletionCallback callback) = 0;
    
    // ==================== 批量 I/O ====================
    
    struct IORequest {
        enum OpType { READ, WRITE };
        OpType op;
        uint64_t offset;
        void* buffer;          // READ: 目标缓冲区, WRITE: const 源数据
        size_t length;
    };
    
    /**
     * @brief 批量 I/O 操作
     * @param requests I/O 请求列表
     * @param callback 所有操作完成后的回调
     * @return 0 成功提交，<0 失败
     */
    virtual int BatchIO(const std::vector<IORequest>& requests,
                        IOCompletionCallback callback) = 0;
    
    // ==================== 段信息 ====================
    
    virtual const UnifiedSegmentDesc& GetDesc() const = 0;
    virtual UnifiedMediumType GetMedium() const = 0;
    virtual uint16_t GetSegmentId() const = 0;
    virtual size_t GetCapacity() const = 0;
    virtual size_t GetUsedSpace() const = 0;
    virtual size_t GetAvailableSpace() const = 0;
    
    // ==================== 空间管理 ====================
    
    /**
     * @brief 分配空间
     * @param size 请求大小
     * @return 分配的偏移，UINT64_MAX 表示失败
     */
    virtual uint64_t AllocateSpace(size_t size) = 0;
    
    /**
     * @brief 释放空间
     * @param offset 偏移
     * @param size 大小
     */
    virtual void DeallocateSpace(uint64_t offset, size_t size) = 0;
};

/**
 * @brief 统一段基类 - 提供通用实现
 */
class UnifiedSegmentBase : public IUnifiedSegment {
public:
    explicit UnifiedSegmentBase(const UnifiedSegmentDesc& desc) : desc_(desc) {}
    
    const UnifiedSegmentDesc& GetDesc() const override { return desc_; }
    UnifiedMediumType GetMedium() const override { return desc_.medium; }
    uint16_t GetSegmentId() const override { return desc_.segment_id; }
    size_t GetCapacity() const override { return desc_.capacity; }
    size_t GetUsedSpace() const override { 
        return desc_.used.load(std::memory_order_relaxed); 
    }
    size_t GetAvailableSpace() const override { return desc_.AvailableSpace(); }
    
    // 简单的顺序分配器（可被子类覆盖）
    uint64_t AllocateSpace(size_t size) override {
        std::lock_guard<std::mutex> lock(alloc_mutex_);
        size_t current_used = desc_.used.load(std::memory_order_relaxed);
        if (current_used + size > desc_.capacity) {
            return UINT64_MAX;  // 空间不足
        }
        desc_.used.fetch_add(size, std::memory_order_relaxed);
        return current_used;
    }
    
    void DeallocateSpace(uint64_t offset, size_t size) override {
        // 简化实现：只减少使用量，不回收具体位置
        // 实际生产环境需要更复杂的空闲空间管理
        desc_.used.fetch_sub(size, std::memory_order_relaxed);
    }
    
    // 默认的批量 I/O 实现（顺序执行）
    int BatchIO(const std::vector<IORequest>& requests,
                IOCompletionCallback callback) override {
        size_t total_bytes = 0;
        for (const auto& req : requests) {
            int ret = 0;
            if (req.op == IORequest::READ) {
                ret = Read(req.offset, req.buffer, req.length);
            } else {
                ret = Write(req.offset, req.buffer, req.length);
            }
            if (ret != 0) {
                if (callback) callback(ret, total_bytes);
                return ret;
            }
            total_bytes += req.length;
        }
        if (callback) callback(0, total_bytes);
        return 0;
    }

protected:
    UnifiedSegmentDesc desc_;
    std::mutex alloc_mutex_;
};

}  // namespace flat
}  // namespace mooncake
