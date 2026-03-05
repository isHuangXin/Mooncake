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
 * @file flat_memory_client.h
 * @brief Flat Memory System 客户端接口
 * 
 * 提供用户友好的高级 API，整合所有 Flat Memory 功能。
 */

#pragma once

#include "kvcache_flat_store.h"
#include "unified_segment_manager.h"
#include "segment_impls.h"
#include <memory>
#include <vector>
#include <string>

namespace mooncake {
namespace flat {

/**
 * @brief 节点配置
 */
struct NodeConfig {
    std::string node_id;
    std::string address;
    uint16_t port = 12345;
    
    struct {
        bool enabled = false;
        int device_id = 0;
        size_t capacity = 0;  // bytes
    } hbm;
    
    struct {
        bool enabled = true;
        size_t capacity = 0;  // bytes
    } local_dram;
    
    struct {
        bool enabled = false;
        std::string path;
        size_t capacity = 0;  // bytes
    } local_ssd;
};

/**
 * @brief 远程节点配置
 */
struct RemoteNodeConfig {
    std::string node_id;
    std::string address;
    uint16_t rdma_port = 12345;
    size_t dram_capacity = 0;
    bool has_ssd = false;
    std::string ssd_path;
    size_t ssd_capacity = 0;
    RemoteStorageType ssd_type = RemoteStorageType::DFS_3FS;
};

/**
 * @brief 集群配置
 */
struct FlatClusterConfig {
    NodeConfig local_node;
    std::vector<RemoteNodeConfig> remote_nodes;
    
    // 默认放置策略
    UnifiedPlacementPolicy default_policy = UnifiedPlacementPolicy::LATENCY_FIRST;
    
    // 启用的介质类型
    std::vector<UnifiedMediumType> enabled_mediums;
};

/**
 * @brief Flat Memory 客户端
 * 
 * 主要入口点，封装所有 Flat Memory 操作
 */
class FlatMemoryClient {
public:
    /**
     * @brief 创建客户端（从配置文件）
     */
    static std::unique_ptr<FlatMemoryClient> Create(
        const std::string& config_path);
    
    /**
     * @brief 创建客户端（从配置对象）
     */
    static std::unique_ptr<FlatMemoryClient> Create(
        const FlatClusterConfig& config);
    
    ~FlatMemoryClient();
    
    // ==================== KVCache 操作 ====================
    
    /**
     * @brief 存储 KVCache
     * 
     * @param key KVCache 键（通常格式："{request_hash}/{layer_id}"）
     * @param data 数据指针
     * @param size 数据大小
     * @param policy 可选的放置策略
     * @return 0 成功
     */
    int Put(const std::string& key, const void* data, size_t size,
            UnifiedPlacementPolicy policy = UnifiedPlacementPolicy::LATENCY_FIRST);
    
    /**
     * @brief 获取 KVCache
     */
    ssize_t Get(const std::string& key, void* buffer, size_t buffer_size);
    
    /**
     * @brief 异步获取 KVCache
     */
    int AsyncGet(const std::string& key, void* buffer, size_t buffer_size,
                 std::function<void(int status, size_t bytes)> callback);
    
    /**
     * @brief 删除 KVCache
     */
    int Remove(const std::string& key);
    
    /**
     * @brief 检查 KVCache 是否存在
     */
    bool Exists(const std::string& key);
    
    // ==================== 批量操作 ====================
    
    /**
     * @brief 批量存储
     */
    std::vector<int> BatchPut(
        const std::vector<std::tuple<std::string, const void*, size_t>>& items,
        UnifiedPlacementPolicy policy = UnifiedPlacementPolicy::LATENCY_FIRST);
    
    /**
     * @brief 批量获取
     */
    std::vector<ssize_t> BatchGet(
        const std::vector<std::tuple<std::string, void*, size_t>>& requests);
    
    // ==================== 迁移与管理 ====================
    
    /**
     * @brief 显式迁移到指定介质
     * 
     * 用途：用户知道某些 KVCache 将被频繁访问，
     *       主动迁移到更快的介质（如 HBM）
     */
    int MigrateTo(const std::string& key, UnifiedMediumType target);
    
    /**
     * @brief 批量迁移
     */
    std::vector<int> BatchMigrateTo(
        const std::vector<std::string>& keys, UnifiedMediumType target);
    
    /**
     * @brief 预加载到高速存储
     * 
     * 用途：Prefetch 场景，将预测会用到的数据提前加载
     */
    int Prefetch(const std::string& key, UnifiedMediumType target_medium);
    
    // ==================== 统计与调试 ====================
    
    /**
     * @brief 获取统计信息
     */
    struct Stats {
        // 对象统计
        size_t total_objects = 0;
        size_t total_data_size = 0;
        
        // 按介质统计
        std::unordered_map<UnifiedMediumType, size_t> objects_per_medium;
        std::unordered_map<UnifiedMediumType, size_t> bytes_per_medium;
        
        // Segment 统计
        std::unordered_map<UnifiedMediumType, size_t> segments_per_medium;
        std::unordered_map<UnifiedMediumType, size_t> capacity_per_medium;
        std::unordered_map<UnifiedMediumType, size_t> used_per_medium;
        
        // 性能统计
        uint64_t total_reads = 0;
        uint64_t total_writes = 0;
        uint64_t read_bytes = 0;
        uint64_t write_bytes = 0;
    };
    
    Stats GetStats() const;
    
    /**
     * @brief 打印统计信息
     */
    void PrintStats() const;
    
    /**
     * @brief 获取所有 KVCache 键
     */
    std::vector<std::string> GetAllKeys() const;
    
    /**
     * @brief 按介质类型获取键
     */
    std::vector<std::string> GetKeysByMedium(UnifiedMediumType medium) const;
    
    // ==================== 底层访问 ====================
    
    /**
     * @brief 获取 Segment Manager（高级用户）
     */
    std::shared_ptr<UnifiedSegmentManager> GetSegmentManager() {
        return segment_manager_;
    }
    
    /**
     * @brief 获取 KVCache Store（高级用户）
     */
    std::shared_ptr<KVCacheFlatStore> GetKVCacheStore() {
        return kvcache_store_;
    }

private:
    explicit FlatMemoryClient(const FlatClusterConfig& config);
    
    void InitializeSegments(const FlatClusterConfig& config);
    void InitializeLocalHBM(const NodeConfig::decltype(NodeConfig::hbm)& hbm_config);
    void InitializeLocalDRAM(const NodeConfig::decltype(NodeConfig::local_dram)& dram_config);
    void InitializeLocalSSD(const NodeConfig::decltype(NodeConfig::local_ssd)& ssd_config);
    void InitializeRemoteNodes(const std::vector<RemoteNodeConfig>& remote_configs);
    
    uint16_t AllocateSegmentId() { return next_segment_id_++; }
    
    FlatClusterConfig config_;
    std::shared_ptr<UnifiedSegmentManager> segment_manager_;
    std::shared_ptr<KVCacheFlatStore> kvcache_store_;
    uint16_t next_segment_id_ = 1;
    
    // 统计
    mutable std::mutex stats_mutex_;
    mutable Stats perf_stats_;
};

}  // namespace flat
}  // namespace mooncake
