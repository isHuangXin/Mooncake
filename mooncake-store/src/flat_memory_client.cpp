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
 * @file flat_memory_client.cpp
 * @brief Flat Memory Client 实现
 */

#include "flat_memory_client.h"
#include <fstream>
#include <iostream>
#include <sstream>
#include <nlohmann/json.hpp>

namespace mooncake {
namespace flat {

using json = nlohmann::json;

// ==================== 静态工厂方法 ====================

std::unique_ptr<FlatMemoryClient> FlatMemoryClient::Create(
    const std::string& config_path) {
    
    std::ifstream file(config_path);
    if (!file.is_open()) {
        std::cerr << "Failed to open config file: " << config_path << std::endl;
        return nullptr;
    }
    
    json config_json;
    try {
        file >> config_json;
    } catch (const json::parse_error& e) {
        std::cerr << "Failed to parse config file: " << e.what() << std::endl;
        return nullptr;
    }
    
    FlatClusterConfig config;
    
    // 解析本地节点配置
    if (config_json.contains("local_node")) {
        auto& local = config_json["local_node"];
        config.local_node.node_id = local.value("node_id", "local");
        config.local_node.address = local.value("address", "127.0.0.1");
        config.local_node.port = local.value("port", 12345);
        
        if (local.contains("hbm")) {
            config.local_node.hbm.enabled = local["hbm"].value("enabled", false);
            config.local_node.hbm.device_id = local["hbm"].value("device_id", 0);
            config.local_node.hbm.capacity = local["hbm"].value("capacity", 0ULL);
        }
        
        if (local.contains("local_dram")) {
            config.local_node.local_dram.enabled = local["local_dram"].value("enabled", true);
            config.local_node.local_dram.capacity = local["local_dram"].value("capacity", 0ULL);
        }
        
        if (local.contains("local_ssd")) {
            config.local_node.local_ssd.enabled = local["local_ssd"].value("enabled", false);
            config.local_node.local_ssd.path = local["local_ssd"].value("path", "");
            config.local_node.local_ssd.capacity = local["local_ssd"].value("capacity", 0ULL);
        }
    }
    
    // 解析远程节点配置
    if (config_json.contains("remote_nodes")) {
        for (const auto& remote : config_json["remote_nodes"]) {
            RemoteNodeConfig remote_config;
            remote_config.node_id = remote.value("node_id", "");
            remote_config.address = remote.value("address", "");
            remote_config.rdma_port = remote.value("rdma_port", 12345);
            remote_config.dram_capacity = remote.value("dram_capacity", 0ULL);
            remote_config.has_ssd = remote.value("has_ssd", false);
            remote_config.ssd_path = remote.value("ssd_path", "");
            remote_config.ssd_capacity = remote.value("ssd_capacity", 0ULL);
            
            if (remote.contains("ssd_type")) {
                std::string type_str = remote["ssd_type"];
                if (type_str == "3fs") {
                    remote_config.ssd_type = RemoteStorageType::DFS_3FS;
                } else if (type_str == "nfs") {
                    remote_config.ssd_type = RemoteStorageType::NFS;
                } else if (type_str == "s3") {
                    remote_config.ssd_type = RemoteStorageType::S3;
                }
            }
            
            config.remote_nodes.push_back(remote_config);
        }
    }
    
    // 解析默认策略
    if (config_json.contains("default_policy")) {
        std::string policy_str = config_json["default_policy"];
        if (policy_str == "latency_first") {
            config.default_policy = UnifiedPlacementPolicy::LATENCY_FIRST;
        } else if (policy_str == "capacity_first") {
            config.default_policy = UnifiedPlacementPolicy::CAPACITY_FIRST;
        } else if (policy_str == "locality_aware") {
            config.default_policy = UnifiedPlacementPolicy::LOCALITY_AWARE;
        }
    }
    
    return Create(config);
}

std::unique_ptr<FlatMemoryClient> FlatMemoryClient::Create(
    const FlatClusterConfig& config) {
    
    return std::unique_ptr<FlatMemoryClient>(new FlatMemoryClient(config));
}

// ==================== 构造与析构 ====================

FlatMemoryClient::FlatMemoryClient(const FlatClusterConfig& config)
    : config_(config) {
    
    // 创建 Segment Manager
    segment_manager_ = std::make_shared<UnifiedSegmentManager>();
    
    // 初始化所有 Segments
    InitializeSegments(config);
    
    // 创建 KVCache Store
    kvcache_store_ = std::make_shared<KVCacheFlatStore>(segment_manager_);
    
    std::cout << "FlatMemoryClient initialized with:" << std::endl;
    PrintStats();
}

FlatMemoryClient::~FlatMemoryClient() {
    // 清理顺序：Store -> SegmentManager
    kvcache_store_.reset();
    segment_manager_.reset();
}

// ==================== 初始化方法 ====================

void FlatMemoryClient::InitializeSegments(const FlatClusterConfig& config) {
    // 1. 初始化本地 HBM
    if (config.local_node.hbm.enabled && config.local_node.hbm.capacity > 0) {
        InitializeLocalHBM(config.local_node.hbm);
    }
    
    // 2. 初始化本地 DRAM
    if (config.local_node.local_dram.enabled && 
        config.local_node.local_dram.capacity > 0) {
        InitializeLocalDRAM(config.local_node.local_dram);
    }
    
    // 3. 初始化本地 SSD
    if (config.local_node.local_ssd.enabled && 
        config.local_node.local_ssd.capacity > 0) {
        InitializeLocalSSD(config.local_node.local_ssd);
    }
    
    // 4. 初始化远程节点
    if (!config.remote_nodes.empty()) {
        InitializeRemoteNodes(config.remote_nodes);
    }
}

void FlatMemoryClient::InitializeLocalHBM(
    const NodeConfig::decltype(NodeConfig::hbm)& hbm_config) {
    
    SegmentCreateConfig seg_config;
    seg_config.segment_id = AllocateSegmentId();
    seg_config.medium = UnifiedMediumType::GPU_HBM;
    seg_config.capacity = hbm_config.capacity;
    seg_config.gpu_device_id = hbm_config.device_id;
    
    auto segment = CreateUnifiedSegment(seg_config);
    if (segment) {
        segment_manager_->RegisterSegment(std::move(segment));
        std::cout << "Registered HBM segment: " 
                  << hbm_config.capacity / (1024*1024) << " MB on GPU " 
                  << hbm_config.device_id << std::endl;
    }
}

void FlatMemoryClient::InitializeLocalDRAM(
    const NodeConfig::decltype(NodeConfig::local_dram)& dram_config) {
    
    SegmentCreateConfig seg_config;
    seg_config.segment_id = AllocateSegmentId();
    seg_config.medium = UnifiedMediumType::LOCAL_DRAM;
    seg_config.capacity = dram_config.capacity;
    
    auto segment = CreateUnifiedSegment(seg_config);
    if (segment) {
        segment_manager_->RegisterSegment(std::move(segment));
        std::cout << "Registered Local DRAM segment: " 
                  << dram_config.capacity / (1024*1024) << " MB" << std::endl;
    }
}

void FlatMemoryClient::InitializeLocalSSD(
    const NodeConfig::decltype(NodeConfig::local_ssd)& ssd_config) {
    
    SegmentCreateConfig seg_config;
    seg_config.segment_id = AllocateSegmentId();
    seg_config.medium = UnifiedMediumType::LOCAL_SSD;
    seg_config.capacity = ssd_config.capacity;
    seg_config.ssd_path = ssd_config.path;
    
    auto segment = CreateUnifiedSegment(seg_config);
    if (segment) {
        segment_manager_->RegisterSegment(std::move(segment));
        std::cout << "Registered Local SSD segment: " 
                  << ssd_config.capacity / (1024*1024) << " MB at " 
                  << ssd_config.path << std::endl;
    }
}

void FlatMemoryClient::InitializeRemoteNodes(
    const std::vector<RemoteNodeConfig>& remote_configs) {
    
    for (const auto& remote : remote_configs) {
        // 注册远程 DRAM
        if (remote.dram_capacity > 0) {
            SegmentCreateConfig seg_config;
            seg_config.segment_id = AllocateSegmentId();
            seg_config.medium = UnifiedMediumType::REMOTE_DRAM;
            seg_config.capacity = remote.dram_capacity;
            seg_config.remote_address = remote.address;
            seg_config.rdma_port = remote.rdma_port;
            
            auto segment = CreateUnifiedSegment(seg_config);
            if (segment) {
                segment_manager_->RegisterSegment(std::move(segment));
                std::cout << "Registered Remote DRAM segment: " 
                          << remote.dram_capacity / (1024*1024) << " MB at " 
                          << remote.address << std::endl;
            }
        }
        
        // 注册远程 SSD
        if (remote.has_ssd && remote.ssd_capacity > 0) {
            SegmentCreateConfig seg_config;
            seg_config.segment_id = AllocateSegmentId();
            seg_config.medium = UnifiedMediumType::REMOTE_SSD;
            seg_config.capacity = remote.ssd_capacity;
            seg_config.remote_path = remote.ssd_path;
            seg_config.remote_storage_type = remote.ssd_type;
            
            auto segment = CreateUnifiedSegment(seg_config);
            if (segment) {
                segment_manager_->RegisterSegment(std::move(segment));
                std::cout << "Registered Remote SSD segment: " 
                          << remote.ssd_capacity / (1024*1024) << " MB at " 
                          << remote.ssd_path << std::endl;
            }
        }
    }
}

// ==================== KVCache 操作 ====================

int FlatMemoryClient::Put(const std::string& key, const void* data, size_t size,
                          UnifiedPlacementPolicy policy) {
    
    KVCachePutConfig put_config;
    put_config.policy = policy;
    put_config.allow_remote = true;
    
    int ret = kvcache_store_->Put(key, data, size, put_config);
    
    if (ret == 0) {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        perf_stats_.total_writes++;
        perf_stats_.write_bytes += size;
    }
    
    return ret;
}

ssize_t FlatMemoryClient::Get(const std::string& key, void* buffer, 
                               size_t buffer_size) {
    
    ssize_t result = kvcache_store_->Get(key, buffer, buffer_size);
    
    if (result > 0) {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        perf_stats_.total_reads++;
        perf_stats_.read_bytes += result;
    }
    
    return result;
}

int FlatMemoryClient::AsyncGet(const std::string& key, void* buffer, 
                                size_t buffer_size,
                                std::function<void(int status, size_t bytes)> callback) {
    
    return kvcache_store_->AsyncGet(key, buffer, buffer_size, 
        [this, callback](int status, size_t bytes) {
            if (status == 0 && bytes > 0) {
                std::lock_guard<std::mutex> lock(stats_mutex_);
                perf_stats_.total_reads++;
                perf_stats_.read_bytes += bytes;
            }
            if (callback) callback(status, bytes);
        });
}

int FlatMemoryClient::Remove(const std::string& key) {
    return kvcache_store_->Remove(key);
}

bool FlatMemoryClient::Exists(const std::string& key) {
    return kvcache_store_->Exists(key);
}

// ==================== 批量操作 ====================

std::vector<int> FlatMemoryClient::BatchPut(
    const std::vector<std::tuple<std::string, const void*, size_t>>& items,
    UnifiedPlacementPolicy policy) {
    
    std::vector<int> results;
    results.reserve(items.size());
    
    for (const auto& [key, data, size] : items) {
        results.push_back(Put(key, data, size, policy));
    }
    
    return results;
}

std::vector<ssize_t> FlatMemoryClient::BatchGet(
    const std::vector<std::tuple<std::string, void*, size_t>>& requests) {
    
    std::vector<ssize_t> results;
    results.reserve(requests.size());
    
    for (const auto& [key, buffer, size] : requests) {
        results.push_back(Get(key, buffer, size));
    }
    
    return results;
}

// ==================== 迁移与管理 ====================

int FlatMemoryClient::MigrateTo(const std::string& key, 
                                 UnifiedMediumType target) {
    return kvcache_store_->MigrateTo(key, target);
}

std::vector<int> FlatMemoryClient::BatchMigrateTo(
    const std::vector<std::string>& keys, UnifiedMediumType target) {
    
    std::vector<int> results;
    results.reserve(keys.size());
    
    for (const auto& key : keys) {
        results.push_back(MigrateTo(key, target));
    }
    
    return results;
}

int FlatMemoryClient::Prefetch(const std::string& key, 
                                UnifiedMediumType target_medium) {
    // Prefetch 本质上就是迁移到目标介质
    return MigrateTo(key, target_medium);
}

// ==================== 统计与调试 ====================

FlatMemoryClient::Stats FlatMemoryClient::GetStats() const {
    Stats stats;
    
    // 从 KVCache Store 获取对象统计
    auto store_stats = kvcache_store_->GetStats();
    stats.total_objects = store_stats.object_count;
    stats.total_data_size = store_stats.total_data_size;
    stats.objects_per_medium = store_stats.count_by_medium;
    stats.bytes_per_medium = store_stats.size_by_medium;
    
    // 从 Segment Manager 获取容量统计
    auto seg_info = segment_manager_->GetAllSegmentInfo();
    for (const auto& info : seg_info) {
        stats.segments_per_medium[info.medium]++;
        stats.capacity_per_medium[info.medium] += info.total_capacity;
        stats.used_per_medium[info.medium] += info.used_capacity;
    }
    
    // 性能统计
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats.total_reads = perf_stats_.total_reads;
        stats.total_writes = perf_stats_.total_writes;
        stats.read_bytes = perf_stats_.read_bytes;
        stats.write_bytes = perf_stats_.write_bytes;
    }
    
    return stats;
}

void FlatMemoryClient::PrintStats() const {
    auto stats = GetStats();
    
    std::cout << "\n=== Flat Memory Client Stats ===" << std::endl;
    std::cout << "Total Objects: " << stats.total_objects << std::endl;
    std::cout << "Total Data Size: " << stats.total_data_size / (1024*1024) 
              << " MB" << std::endl;
    
    std::cout << "\n--- By Medium ---" << std::endl;
    
    auto medium_name = [](UnifiedMediumType m) -> std::string {
        switch (m) {
            case UnifiedMediumType::GPU_HBM: return "GPU_HBM";
            case UnifiedMediumType::LOCAL_DRAM: return "LOCAL_DRAM";
            case UnifiedMediumType::REMOTE_DRAM: return "REMOTE_DRAM";
            case UnifiedMediumType::LOCAL_SSD: return "LOCAL_SSD";
            case UnifiedMediumType::REMOTE_SSD: return "REMOTE_SSD";
            default: return "UNKNOWN";
        }
    };
    
    for (const auto& [medium, count] : stats.segments_per_medium) {
        std::cout << medium_name(medium) << ":" << std::endl;
        std::cout << "  Segments: " << count << std::endl;
        std::cout << "  Capacity: " << stats.capacity_per_medium.at(medium) / (1024*1024) 
                  << " MB" << std::endl;
        std::cout << "  Used: " << stats.used_per_medium.at(medium) / (1024*1024) 
                  << " MB" << std::endl;
        if (stats.objects_per_medium.count(medium)) {
            std::cout << "  Objects: " << stats.objects_per_medium.at(medium) << std::endl;
        }
    }
    
    std::cout << "\n--- Performance ---" << std::endl;
    std::cout << "Total Reads: " << stats.total_reads << std::endl;
    std::cout << "Total Writes: " << stats.total_writes << std::endl;
    std::cout << "Read Bytes: " << stats.read_bytes / (1024*1024) << " MB" << std::endl;
    std::cout << "Write Bytes: " << stats.write_bytes / (1024*1024) << " MB" << std::endl;
    std::cout << "================================\n" << std::endl;
}

std::vector<std::string> FlatMemoryClient::GetAllKeys() const {
    return kvcache_store_->GetAllKeys();
}

std::vector<std::string> FlatMemoryClient::GetKeysByMedium(
    UnifiedMediumType medium) const {
    return kvcache_store_->GetKeysByMedium(medium);
}

}  // namespace flat
}  // namespace mooncake
