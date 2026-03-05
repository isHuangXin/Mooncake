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
 * @file segment_impls.h
 * @brief KVCache Flat Memory System - 各种存储介质的段实现
 * 
 * 包含：
 * - HBMSegment: GPU HBM 访问（通过 CUDA）
 * - LocalDRAMSegment: 本地 DRAM 访问（memcpy）
 * - RemoteDRAMSegment: 远程 DRAM 访问（通过 RDMA/Transfer Engine）
 * - LocalSSDSegment: 本地 SSD 访问（通过 Direct I/O）
 * - RemoteSSDSegment: 远程 SSD 访问（通过 DFS/3FS）
 */

#pragma once

#include "unified_segment.h"
#include <cstring>
#include <thread>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

// 前向声明 Transfer Engine 相关类型
namespace mooncake {
class TransferEngine;
}

namespace mooncake {
namespace flat {

// ============================================================================
// HBM Segment - GPU 高带宽内存
// ============================================================================

/**
 * @brief GPU HBM 段实现
 * 
 * 通过 CUDA API 访问 GPU 显存，支持：
 * - cudaMemcpy (同步)
 * - cudaMemcpyAsync (异步)
 */
class HBMSegment : public UnifiedSegmentBase {
public:
    HBMSegment(const UnifiedSegmentDesc& desc, void* device_ptr)
        : UnifiedSegmentBase(desc), device_ptr_(device_ptr) {
        desc_.medium = UnifiedMediumType::GPU_HBM;
        desc_.latency_ns = 10;           // ~10ns
        desc_.bandwidth_mbps = 2000000;  // ~2 TB/s
    }
    
    int Read(uint64_t offset, void* buffer, size_t length) override {
#ifdef USE_CUDA
        cudaError_t err = cudaSetDevice(desc_.gpu_id);
        if (err != cudaSuccess) return -1;
        
        err = cudaMemcpy(buffer, 
                         static_cast<char*>(device_ptr_) + offset,
                         length, 
                         cudaMemcpyDeviceToHost);
        return (err == cudaSuccess) ? 0 : -1;
#else
        // 无 CUDA 时的模拟实现（用于测试）
        memcpy(buffer, static_cast<char*>(device_ptr_) + offset, length);
        return 0;
#endif
    }
    
    int Write(uint64_t offset, const void* data, size_t length) override {
#ifdef USE_CUDA
        cudaError_t err = cudaSetDevice(desc_.gpu_id);
        if (err != cudaSuccess) return -1;
        
        err = cudaMemcpy(static_cast<char*>(device_ptr_) + offset,
                         data,
                         length,
                         cudaMemcpyHostToDevice);
        return (err == cudaSuccess) ? 0 : -1;
#else
        memcpy(static_cast<char*>(device_ptr_) + offset, data, length);
        return 0;
#endif
    }
    
    int AsyncRead(uint64_t offset, void* buffer, size_t length,
                  IOCompletionCallback callback) override {
#ifdef USE_CUDA
        cudaSetDevice(desc_.gpu_id);
        cudaStream_t stream;
        cudaStreamCreate(&stream);
        
        cudaError_t err = cudaMemcpyAsync(
            buffer,
            static_cast<char*>(device_ptr_) + offset,
            length,
            cudaMemcpyDeviceToHost,
            stream);
        
        if (err != cudaSuccess) {
            cudaStreamDestroy(stream);
            if (callback) callback(-1, 0);
            return -1;
        }
        
        // 启动等待线程
        std::thread([stream, callback, length]() {
            cudaStreamSynchronize(stream);
            cudaStreamDestroy(stream);
            if (callback) callback(0, length);
        }).detach();
        
        return 0;
#else
        // 模拟异步
        std::thread([this, offset, buffer, length, callback]() {
            int ret = Read(offset, buffer, length);
            if (callback) callback(ret, ret == 0 ? length : 0);
        }).detach();
        return 0;
#endif
    }
    
    int AsyncWrite(uint64_t offset, const void* data, size_t length,
                   IOCompletionCallback callback) override {
#ifdef USE_CUDA
        cudaSetDevice(desc_.gpu_id);
        cudaStream_t stream;
        cudaStreamCreate(&stream);
        
        cudaError_t err = cudaMemcpyAsync(
            static_cast<char*>(device_ptr_) + offset,
            data,
            length,
            cudaMemcpyHostToDevice,
            stream);
        
        if (err != cudaSuccess) {
            cudaStreamDestroy(stream);
            if (callback) callback(-1, 0);
            return -1;
        }
        
        std::thread([stream, callback, length]() {
            cudaStreamSynchronize(stream);
            cudaStreamDestroy(stream);
            if (callback) callback(0, length);
        }).detach();
        
        return 0;
#else
        std::thread([this, offset, data, length, callback]() {
            int ret = Write(offset, data, length);
            if (callback) callback(ret, ret == 0 ? length : 0);
        }).detach();
        return 0;
#endif
    }

private:
    void* device_ptr_;
};

// ============================================================================
// Local DRAM Segment - 本地主机内存
// ============================================================================

/**
 * @brief 本地 DRAM 段实现
 * 
 * 通过 memcpy 直接访问本地内存，延迟最低。
 */
class LocalDRAMSegment : public UnifiedSegmentBase {
public:
    LocalDRAMSegment(const UnifiedSegmentDesc& desc, void* base_ptr)
        : UnifiedSegmentBase(desc), base_ptr_(base_ptr) {
        desc_.medium = UnifiedMediumType::LOCAL_DRAM;
        desc_.latency_ns = 100;          // ~100ns
        desc_.bandwidth_mbps = 200000;   // ~200 GB/s (DDR5)
    }
    
    int Read(uint64_t offset, void* buffer, size_t length) override {
        if (offset + length > desc_.capacity) return -1;
        memcpy(buffer, static_cast<char*>(base_ptr_) + offset, length);
        return 0;
    }
    
    int Write(uint64_t offset, const void* data, size_t length) override {
        if (offset + length > desc_.capacity) return -1;
        memcpy(static_cast<char*>(base_ptr_) + offset, data, length);
        return 0;
    }
    
    int AsyncRead(uint64_t offset, void* buffer, size_t length,
                  IOCompletionCallback callback) override {
        // 本地内存访问很快，可以直接同步完成
        int ret = Read(offset, buffer, length);
        if (callback) callback(ret, ret == 0 ? length : 0);
        return ret;
    }
    
    int AsyncWrite(uint64_t offset, const void* data, size_t length,
                   IOCompletionCallback callback) override {
        int ret = Write(offset, data, length);
        if (callback) callback(ret, ret == 0 ? length : 0);
        return ret;
    }
    
    void* GetBasePtr() const { return base_ptr_; }

private:
    void* base_ptr_;
};

// ============================================================================
// Remote DRAM Segment - 远程主机内存 (通过 RDMA)
// ============================================================================

/**
 * @brief 远程 DRAM 段实现
 * 
 * 通过 Transfer Engine 的 RDMA 接口访问远程节点的内存。
 */
class RemoteDRAMSegment : public UnifiedSegmentBase {
public:
    RemoteDRAMSegment(const UnifiedSegmentDesc& desc,
                      std::shared_ptr<TransferEngine> engine,
                      const std::string& remote_segment_name)
        : UnifiedSegmentBase(desc),
          engine_(engine),
          remote_segment_name_(remote_segment_name) {
        desc_.medium = UnifiedMediumType::REMOTE_DRAM;
        desc_.latency_ns = 2000;         // ~2us (RDMA)
        desc_.bandwidth_mbps = 200000;   // ~200 GB/s (多网卡聚合)
    }
    
    int Read(uint64_t offset, void* buffer, size_t length) override;
    int Write(uint64_t offset, const void* data, size_t length) override;
    int AsyncRead(uint64_t offset, void* buffer, size_t length,
                  IOCompletionCallback callback) override;
    int AsyncWrite(uint64_t offset, const void* data, size_t length,
                   IOCompletionCallback callback) override;

private:
    std::shared_ptr<TransferEngine> engine_;
    std::string remote_segment_name_;
};

// ============================================================================
// Local SSD Segment - 本地 NVMe SSD
// ============================================================================

/**
 * @brief 本地 SSD 段实现
 * 
 * 通过 Direct I/O (O_DIRECT) 访问本地 NVMe SSD，绕过 OS Page Cache。
 * 
 * 注意：使用 O_DIRECT 时，buffer 必须按扇区大小对齐（通常 512 或 4096 字节）
 */
class LocalSSDSegment : public UnifiedSegmentBase {
public:
    LocalSSDSegment(const UnifiedSegmentDesc& desc)
        : UnifiedSegmentBase(desc), fd_(-1) {
        desc_.medium = UnifiedMediumType::LOCAL_SSD;
        desc_.latency_ns = 10000;        // ~10us
        desc_.bandwidth_mbps = 7000;     // ~7 GB/s (NVMe Gen4)
        
        // 打开设备或文件
        if (!desc_.nvme_device.empty()) {
            fd_ = open(desc_.nvme_device.c_str(), O_RDWR | O_DIRECT | O_CREAT, 0644);
        }
    }
    
    ~LocalSSDSegment() {
        if (fd_ >= 0) {
            close(fd_);
            fd_ = -1;
        }
    }
    
    int Read(uint64_t offset, void* buffer, size_t length) override {
        if (fd_ < 0) return -1;
        
        // 确保对齐
        size_t aligned_length = AlignUp(length, 4096);
        void* aligned_buffer = buffer;
        bool need_free = false;
        
        // 如果 buffer 未对齐，分配对齐的临时缓冲区
        if (reinterpret_cast<uintptr_t>(buffer) % 4096 != 0) {
            if (posix_memalign(&aligned_buffer, 4096, aligned_length) != 0) {
                return -1;
            }
            need_free = true;
        }
        
        ssize_t ret = pread(fd_, aligned_buffer, aligned_length, 
                            desc_.base_address + offset);
        
        if (need_free) {
            if (ret > 0) {
                memcpy(buffer, aligned_buffer, length);
            }
            free(aligned_buffer);
        }
        
        return (ret >= static_cast<ssize_t>(length)) ? 0 : -1;
    }
    
    int Write(uint64_t offset, const void* data, size_t length) override {
        if (fd_ < 0) return -1;
        
        size_t aligned_length = AlignUp(length, 4096);
        void* aligned_buffer = nullptr;
        
        // 分配对齐的缓冲区
        if (posix_memalign(&aligned_buffer, 4096, aligned_length) != 0) {
            return -1;
        }
        
        memcpy(aligned_buffer, data, length);
        // 填充剩余部分
        if (aligned_length > length) {
            memset(static_cast<char*>(aligned_buffer) + length, 0, 
                   aligned_length - length);
        }
        
        ssize_t ret = pwrite(fd_, aligned_buffer, aligned_length,
                             desc_.base_address + offset);
        
        free(aligned_buffer);
        return (ret >= static_cast<ssize_t>(length)) ? 0 : -1;
    }
    
    int AsyncRead(uint64_t offset, void* buffer, size_t length,
                  IOCompletionCallback callback) override {
        // 使用线程池模拟异步（实际生产环境应使用 io_uring）
        std::thread([this, offset, buffer, length, callback]() {
            int ret = Read(offset, buffer, length);
            if (callback) callback(ret, ret == 0 ? length : 0);
        }).detach();
        return 0;
    }
    
    int AsyncWrite(uint64_t offset, const void* data, size_t length,
                   IOCompletionCallback callback) override {
        // 复制数据以避免异步写入时数据被修改
        void* data_copy = malloc(length);
        if (!data_copy) {
            if (callback) callback(-1, 0);
            return -1;
        }
        memcpy(data_copy, data, length);
        
        std::thread([this, offset, data_copy, length, callback]() {
            int ret = Write(offset, data_copy, length);
            free(data_copy);
            if (callback) callback(ret, ret == 0 ? length : 0);
        }).detach();
        return 0;
    }

private:
    static size_t AlignUp(size_t value, size_t alignment) {
        return (value + alignment - 1) & ~(alignment - 1);
    }
    
    int fd_;
};

// ============================================================================
// Remote SSD Segment - 远程 SSD (通过 DFS/3FS)
// ============================================================================

/**
 * @brief 远程 SSD 段实现
 * 
 * 通过分布式文件系统（如 3FS）访问远程节点的 SSD 存储。
 */
class RemoteSSDSegment : public UnifiedSegmentBase {
public:
    RemoteSSDSegment(const UnifiedSegmentDesc& desc)
        : UnifiedSegmentBase(desc) {
        desc_.medium = UnifiedMediumType::REMOTE_SSD;
        desc_.latency_ns = 50000;        // ~50us (网络 + SSD)
        desc_.bandwidth_mbps = 5000;     // ~5 GB/s
        
        // 确保 DFS 路径存在
        if (!desc_.dfs_path.empty()) {
            data_file_path_ = desc_.dfs_path + "/segment_" + 
                              std::to_string(desc_.segment_id) + ".data";
        }
    }
    
    int Read(uint64_t offset, void* buffer, size_t length) override {
        if (data_file_path_.empty()) return -1;
        
        int fd = open(data_file_path_.c_str(), O_RDONLY);
        if (fd < 0) return -1;
        
        ssize_t ret = pread(fd, buffer, length, offset);
        close(fd);
        
        return (ret == static_cast<ssize_t>(length)) ? 0 : -1;
    }
    
    int Write(uint64_t offset, const void* data, size_t length) override {
        if (data_file_path_.empty()) return -1;
        
        int fd = open(data_file_path_.c_str(), O_WRONLY | O_CREAT, 0644);
        if (fd < 0) return -1;
        
        ssize_t ret = pwrite(fd, data, length, offset);
        close(fd);
        
        return (ret == static_cast<ssize_t>(length)) ? 0 : -1;
    }
    
    int AsyncRead(uint64_t offset, void* buffer, size_t length,
                  IOCompletionCallback callback) override {
        std::thread([this, offset, buffer, length, callback]() {
            int ret = Read(offset, buffer, length);
            if (callback) callback(ret, ret == 0 ? length : 0);
        }).detach();
        return 0;
    }
    
    int AsyncWrite(uint64_t offset, const void* data, size_t length,
                   IOCompletionCallback callback) override {
        void* data_copy = malloc(length);
        if (!data_copy) {
            if (callback) callback(-1, 0);
            return -1;
        }
        memcpy(data_copy, data, length);
        
        std::thread([this, offset, data_copy, length, callback]() {
            int ret = Write(offset, data_copy, length);
            free(data_copy);
            if (callback) callback(ret, ret == 0 ? length : 0);
        }).detach();
        return 0;
    }

private:
    std::string data_file_path_;
};

// ============================================================================
// 工厂函数
// ============================================================================

/**
 * @brief 创建统一段实例
 */
inline std::unique_ptr<IUnifiedSegment> CreateUnifiedSegment(
    const UnifiedSegmentDesc& desc,
    void* memory_ptr = nullptr,
    std::shared_ptr<TransferEngine> engine = nullptr) {
    
    switch (desc.medium) {
        case UnifiedMediumType::GPU_HBM:
            return std::make_unique<HBMSegment>(desc, memory_ptr);
            
        case UnifiedMediumType::LOCAL_DRAM:
            return std::make_unique<LocalDRAMSegment>(desc, memory_ptr);
            
        case UnifiedMediumType::REMOTE_DRAM:
            if (!engine) return nullptr;
            return std::make_unique<RemoteDRAMSegment>(
                desc, engine, desc.rdma_endpoint);
            
        case UnifiedMediumType::LOCAL_SSD:
            return std::make_unique<LocalSSDSegment>(desc);
            
        case UnifiedMediumType::REMOTE_SSD:
            return std::make_unique<RemoteSSDSegment>(desc);
            
        default:
            return nullptr;
    }
}

}  // namespace flat
}  // namespace mooncake
