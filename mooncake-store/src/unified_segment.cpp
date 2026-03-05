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
 * @file unified_segment.cpp
 * @brief UnifiedSegment 实现
 * 
 * 实现 RemoteDRAMSegment 和 RemoteSSDSegment 的具体功能
 */

#include "unified_segment.h"
#include "segment_impls.h"

// 如果启用了 Transfer Engine
#ifdef MOONCAKE_TRANSFER_ENGINE
#include "transfer_engine.h"
#endif

#include <cstring>
#include <fstream>
#include <future>
#include <iostream>
#include <thread>

namespace mooncake {
namespace flat {

// ==================== RemoteDRAMSegment 实现 ====================

RemoteDRAMSegment::RemoteDRAMSegment(
    uint16_t segment_id, size_t capacity,
    const std::string& remote_address, uint16_t rdma_port)
    : UnifiedSegmentBase(segment_id, UnifiedMediumType::REMOTE_DRAM, capacity)
    , remote_address_(remote_address)
    , rdma_port_(rdma_port)
{
    InitializeConnection();
}

RemoteDRAMSegment::~RemoteDRAMSegment() {
    CleanupConnection();
}

int RemoteDRAMSegment::Read(uint64_t offset, void* buffer, size_t size) {
    if (offset + size > capacity_) {
        return -1;  // 越界
    }
    
#ifdef MOONCAKE_TRANSFER_ENGINE
    // 使用 Transfer Engine 进行 RDMA 读取
    if (transfer_engine_) {
        TransferRequest req;
        req.type = TransferType::READ;
        req.local_addr = buffer;
        req.remote_addr = remote_base_addr_ + offset;
        req.size = size;
        req.target_id = remote_segment_id_;
        
        auto result = transfer_engine_->Submit(req);
        if (!result) {
            return -2;  // 提交失败
        }
        
        // 等待完成
        return result->Wait();
    }
#endif
    
    // Fallback: 模拟 RDMA 读取（用于测试）
    // 实际部署时必须使用 Transfer Engine
    std::cerr << "Warning: RemoteDRAMSegment using simulated read" << std::endl;
    std::this_thread::sleep_for(std::chrono::microseconds(2));  // 模拟 ~2μs 延迟
    
    // 在测试模式下，可能有本地备份
    if (test_local_buffer_) {
        memcpy(buffer, static_cast<const char*>(test_local_buffer_) + offset, size);
        return 0;
    }
    
    return -3;  // 无有效连接
}

int RemoteDRAMSegment::Write(uint64_t offset, const void* data, size_t size) {
    if (offset + size > capacity_) {
        return -1;
    }
    
#ifdef MOONCAKE_TRANSFER_ENGINE
    if (transfer_engine_) {
        TransferRequest req;
        req.type = TransferType::WRITE;
        req.local_addr = const_cast<void*>(data);
        req.remote_addr = remote_base_addr_ + offset;
        req.size = size;
        req.target_id = remote_segment_id_;
        
        auto result = transfer_engine_->Submit(req);
        if (!result) {
            return -2;
        }
        
        return result->Wait();
    }
#endif
    
    // Fallback
    std::cerr << "Warning: RemoteDRAMSegment using simulated write" << std::endl;
    std::this_thread::sleep_for(std::chrono::microseconds(2));
    
    if (test_local_buffer_) {
        memcpy(static_cast<char*>(test_local_buffer_) + offset, data, size);
        return 0;
    }
    
    return -3;
}

int RemoteDRAMSegment::AsyncRead(
    uint64_t offset, void* buffer, size_t size,
    std::function<void(int status, size_t bytes)> callback) {
    
    if (offset + size > capacity_) {
        if (callback) callback(-1, 0);
        return -1;
    }
    
#ifdef MOONCAKE_TRANSFER_ENGINE
    if (transfer_engine_) {
        TransferRequest req;
        req.type = TransferType::READ;
        req.local_addr = buffer;
        req.remote_addr = remote_base_addr_ + offset;
        req.size = size;
        req.target_id = remote_segment_id_;
        req.callback = [callback, size](int status) {
            if (callback) callback(status, status == 0 ? size : 0);
        };
        
        auto result = transfer_engine_->SubmitAsync(req);
        return result ? 0 : -2;
    }
#endif
    
    // Fallback: 使用线程模拟异步
    std::thread([this, offset, buffer, size, callback]() {
        int status = Read(offset, buffer, size);
        if (callback) callback(status, status == 0 ? size : 0);
    }).detach();
    
    return 0;
}

int RemoteDRAMSegment::AsyncWrite(
    uint64_t offset, const void* data, size_t size,
    std::function<void(int status, size_t bytes)> callback) {
    
    if (offset + size > capacity_) {
        if (callback) callback(-1, 0);
        return -1;
    }
    
#ifdef MOONCAKE_TRANSFER_ENGINE
    if (transfer_engine_) {
        TransferRequest req;
        req.type = TransferType::WRITE;
        req.local_addr = const_cast<void*>(data);
        req.remote_addr = remote_base_addr_ + offset;
        req.size = size;
        req.target_id = remote_segment_id_;
        req.callback = [callback, size](int status) {
            if (callback) callback(status, status == 0 ? size : 0);
        };
        
        auto result = transfer_engine_->SubmitAsync(req);
        return result ? 0 : -2;
    }
#endif
    
    // Fallback
    std::thread([this, offset, data, size, callback]() {
        int status = Write(offset, data, size);
        if (callback) callback(status, status == 0 ? size : 0);
    }).detach();
    
    return 0;
}

void RemoteDRAMSegment::InitializeConnection() {
#ifdef MOONCAKE_TRANSFER_ENGINE
    // 初始化 Transfer Engine 连接
    TransferEngineConfig config;
    config.device_name = "mlx5_0";  // 默认 RDMA 设备
    config.port = rdma_port_;
    
    transfer_engine_ = std::make_shared<TransferEngine>(config);
    
    // 连接到远程节点
    int ret = transfer_engine_->Connect(remote_address_, rdma_port_);
    if (ret != 0) {
        std::cerr << "Failed to connect to remote node: " 
                  << remote_address_ << ":" << rdma_port_ << std::endl;
        transfer_engine_.reset();
        return;
    }
    
    // 注册内存区域
    remote_segment_id_ = transfer_engine_->RegisterRemoteMemory(
        remote_address_, capacity_);
    
    if (remote_segment_id_ == 0) {
        std::cerr << "Failed to register remote memory" << std::endl;
        transfer_engine_.reset();
        return;
    }
    
    remote_base_addr_ = transfer_engine_->GetRemoteBaseAddress(remote_segment_id_);
    
    std::cout << "RemoteDRAMSegment connected to " << remote_address_ 
              << " segment=" << remote_segment_id_ << std::endl;
#else
    std::cerr << "Transfer Engine not available, using simulated mode" << std::endl;
    
    // 测试模式：分配本地缓冲区模拟远程内存
    test_local_buffer_ = aligned_alloc(4096, capacity_);
    if (test_local_buffer_) {
        memset(test_local_buffer_, 0, capacity_);
    }
#endif
}

void RemoteDRAMSegment::CleanupConnection() {
#ifdef MOONCAKE_TRANSFER_ENGINE
    if (transfer_engine_) {
        transfer_engine_->UnregisterRemoteMemory(remote_segment_id_);
        transfer_engine_->Disconnect(remote_address_);
        transfer_engine_.reset();
    }
#endif
    
    if (test_local_buffer_) {
        free(test_local_buffer_);
        test_local_buffer_ = nullptr;
    }
}

// ==================== RemoteSSDSegment 实现 ====================

RemoteSSDSegment::RemoteSSDSegment(
    uint16_t segment_id, size_t capacity,
    const std::string& remote_path, RemoteStorageType storage_type)
    : UnifiedSegmentBase(segment_id, UnifiedMediumType::REMOTE_SSD, capacity)
    , remote_path_(remote_path)
    , storage_type_(storage_type)
{
    // 验证远程存储可用性
    if (!ValidateConnection()) {
        std::cerr << "Warning: Remote storage not accessible: " 
                  << remote_path_ << std::endl;
    }
}

RemoteSSDSegment::~RemoteSSDSegment() {
    // 清理临时文件等
}

int RemoteSSDSegment::Read(uint64_t offset, void* buffer, size_t size) {
    if (offset + size > capacity_) {
        return -1;
    }
    
    switch (storage_type_) {
        case RemoteStorageType::DFS_3FS:
            return ReadFrom3FS(offset, buffer, size);
        case RemoteStorageType::NFS:
            return ReadFromNFS(offset, buffer, size);
        case RemoteStorageType::S3:
            return ReadFromS3(offset, buffer, size);
        default:
            return -2;
    }
}

int RemoteSSDSegment::Write(uint64_t offset, const void* data, size_t size) {
    if (offset + size > capacity_) {
        return -1;
    }
    
    switch (storage_type_) {
        case RemoteStorageType::DFS_3FS:
            return WriteTo3FS(offset, data, size);
        case RemoteStorageType::NFS:
            return WriteToNFS(offset, data, size);
        case RemoteStorageType::S3:
            return WriteToS3(offset, data, size);
        default:
            return -2;
    }
}

int RemoteSSDSegment::AsyncRead(
    uint64_t offset, void* buffer, size_t size,
    std::function<void(int status, size_t bytes)> callback) {
    
    // 使用线程池进行异步读取
    std::thread([this, offset, buffer, size, callback]() {
        int status = Read(offset, buffer, size);
        if (callback) callback(status, status == 0 ? size : 0);
    }).detach();
    
    return 0;
}

int RemoteSSDSegment::AsyncWrite(
    uint64_t offset, const void* data, size_t size,
    std::function<void(int status, size_t bytes)> callback) {
    
    std::thread([this, offset, data, size, callback]() {
        int status = Write(offset, data, size);
        if (callback) callback(status, status == 0 ? size : 0);
    }).detach();
    
    return 0;
}

int RemoteSSDSegment::ReadFrom3FS(uint64_t offset, void* buffer, size_t size) {
    // 3FS 读取实现
    // 3FS 是 ByteDance 开发的高性能分布式文件系统
    
    std::string full_path = remote_path_;
    
    // 使用 posix 接口访问 3FS 挂载点
    std::ifstream file(full_path, std::ios::binary);
    if (!file.is_open()) {
        return -1;
    }
    
    file.seekg(offset);
    if (!file.read(static_cast<char*>(buffer), size)) {
        return -2;
    }
    
    return 0;
}

int RemoteSSDSegment::WriteTo3FS(uint64_t offset, const void* data, size_t size) {
    std::string full_path = remote_path_;
    
    std::fstream file(full_path, std::ios::binary | std::ios::in | std::ios::out);
    if (!file.is_open()) {
        // 尝试创建文件
        std::ofstream new_file(full_path, std::ios::binary);
        if (!new_file.is_open()) {
            return -1;
        }
        new_file.close();
        file.open(full_path, std::ios::binary | std::ios::in | std::ios::out);
    }
    
    file.seekp(offset);
    if (!file.write(static_cast<const char*>(data), size)) {
        return -2;
    }
    
    return 0;
}

int RemoteSSDSegment::ReadFromNFS(uint64_t offset, void* buffer, size_t size) {
    // NFS 挂载后使用标准文件操作
    return ReadFrom3FS(offset, buffer, size);  // 相同实现
}

int RemoteSSDSegment::WriteToNFS(uint64_t offset, const void* data, size_t size) {
    return WriteTo3FS(offset, data, size);
}

int RemoteSSDSegment::ReadFromS3(uint64_t offset, void* buffer, size_t size) {
    // S3 读取需要 AWS SDK
    // 这里提供简化实现，实际需要集成 aws-sdk-cpp
    
#ifdef AWS_SDK_AVAILABLE
    // 使用 AWS SDK 进行 S3 范围读取
    Aws::S3::S3Client s3_client;
    
    Aws::S3::Model::GetObjectRequest request;
    request.SetBucket(ExtractBucket(remote_path_));
    request.SetKey(ExtractKey(remote_path_));
    
    std::stringstream range_str;
    range_str << "bytes=" << offset << "-" << (offset + size - 1);
    request.SetRange(range_str.str());
    
    auto outcome = s3_client.GetObject(request);
    if (!outcome.IsSuccess()) {
        return -1;
    }
    
    auto& stream = outcome.GetResult().GetBody();
    stream.read(static_cast<char*>(buffer), size);
    
    return 0;
#else
    std::cerr << "S3 support not available" << std::endl;
    return -1;
#endif
}

int RemoteSSDSegment::WriteToS3(uint64_t offset, const void* data, size_t size) {
#ifdef AWS_SDK_AVAILABLE
    // S3 分块上传实现
    // 实际实现需要处理 multipart upload
    return -1;  // TODO: 实现 S3 写入
#else
    std::cerr << "S3 support not available" << std::endl;
    return -1;
#endif
}

bool RemoteSSDSegment::ValidateConnection() {
    switch (storage_type_) {
        case RemoteStorageType::DFS_3FS:
        case RemoteStorageType::NFS:
            // 检查挂载点是否可访问
            return access(remote_path_.c_str(), R_OK | W_OK) == 0;
        case RemoteStorageType::S3:
            // S3 验证需要尝试 HeadObject
            return true;  // 假设配置正确
        default:
            return false;
    }
}

// ==================== Segment 工厂函数实现 ====================

std::unique_ptr<IUnifiedSegment> CreateUnifiedSegment(
    const SegmentCreateConfig& config) {
    
    switch (config.medium) {
        case UnifiedMediumType::GPU_HBM:
            return std::make_unique<HBMSegment>(
                config.segment_id, config.capacity, config.gpu_device_id);
            
        case UnifiedMediumType::LOCAL_DRAM:
            return std::make_unique<LocalDRAMSegment>(
                config.segment_id, config.capacity);
            
        case UnifiedMediumType::REMOTE_DRAM:
            return std::make_unique<RemoteDRAMSegment>(
                config.segment_id, config.capacity, 
                config.remote_address, config.rdma_port);
            
        case UnifiedMediumType::LOCAL_SSD:
            return std::make_unique<LocalSSDSegment>(
                config.segment_id, config.capacity, config.ssd_path);
            
        case UnifiedMediumType::REMOTE_SSD:
            return std::make_unique<RemoteSSDSegment>(
                config.segment_id, config.capacity, 
                config.remote_path, config.remote_storage_type);
            
        default:
            return nullptr;
    }
}

}  // namespace flat
}  // namespace mooncake
