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

#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <variant>
#include <ostream>
#include <unordered_map>

namespace mooncake {
namespace flat {

/**
 * @brief 存储介质类型（扁平化，不区分层级）
 * 
 * 在Flat Memory System中，所有存储介质被视为同一层级，
 * 此枚举仅用于标识介质类型，不用于决定数据冷热。
 */
enum class StorageMedium {
    GPU_HBM,        // GPU高带宽内存 (~纳秒级访问)
    SYSTEM_DRAM,    // 系统内存 (~10-100纳秒)
    LOCAL_SSD,      // 本地SSD (~微秒级)
    REMOTE_STORAGE  // 远程存储 (~微秒级)
};

/**
 * @brief Stream operator for StorageMedium
 */
inline std::ostream& operator<<(std::ostream& os,
                                const StorageMedium& medium) noexcept {
    static const std::unordered_map<StorageMedium, const char*> medium_strings{
        {StorageMedium::GPU_HBM, "GPU_HBM"},
        {StorageMedium::SYSTEM_DRAM, "SYSTEM_DRAM"},
        {StorageMedium::LOCAL_SSD, "LOCAL_SSD"},
        {StorageMedium::REMOTE_STORAGE, "REMOTE_STORAGE"}
    };
    
    auto it = medium_strings.find(medium);
    os << (it != medium_strings.end() ? it->second : "UNKNOWN");
    return os;
}

/**
 * @brief 放置策略类型
 * 
 * 定义KVCache数据如何被放置到存储介质中。
 * 注意：这些策略不基于数据冷热，而是基于系统需求。
 */
enum class PlacementPolicy {
    CAPACITY_FIRST,     // 容量优先：优先使用可用容量最大的存储
    LATENCY_FIRST,      // 延迟优先：优先使用访问延迟最低的存储
    ROUND_ROBIN,        // 轮询：在所有可用存储间均匀分布
    RANDOM,             // 随机：随机选择可用存储
    LOCALITY_AWARE      // 位置感知：根据数据亲和性和NUMA拓扑放置
};

/**
 * @brief Stream operator for PlacementPolicy
 */
inline std::ostream& operator<<(std::ostream& os,
                                const PlacementPolicy& policy) noexcept {
    static const std::unordered_map<PlacementPolicy, const char*> policy_strings{
        {PlacementPolicy::CAPACITY_FIRST, "CAPACITY_FIRST"},
        {PlacementPolicy::LATENCY_FIRST, "LATENCY_FIRST"},
        {PlacementPolicy::ROUND_ROBIN, "ROUND_ROBIN"},
        {PlacementPolicy::RANDOM, "RANDOM"},
        {PlacementPolicy::LOCALITY_AWARE, "LOCALITY_AWARE"}
    };
    
    auto it = policy_strings.find(policy);
    os << (it != policy_strings.end() ? it->second : "UNKNOWN");
    return os;
}

/**
 * @brief 扁平化存储段描述符
 * 
 * 描述一个存储段的完整信息，包括物理特性和当前状态。
 * 所有存储段被统一管理，不区分层级。
 */
struct FlatSegmentDescriptor {
    std::string segment_id;          // 唯一标识符
    StorageMedium medium;            // 存储介质类型
    uint64_t base_address;           // 基地址
    size_t capacity;                 // 总容量（字节）
    size_t used;                     // 已使用容量（字节）
    std::string transport_endpoint;  // Transfer Engine端点
    
    // 介质特性（用于策略决策，但不用于层级划分）
    uint64_t estimated_latency_ns;   // 估计访问延迟（纳秒）
    uint64_t bandwidth_gbps;         // 带宽（Gbps）
    
    /**
     * @brief 检查段是否有可用空间
     */
    bool isAvailable() const { return (capacity > used); }
    
    /**
     * @brief 获取可用空间
     */
    size_t availableSpace() const { 
        return capacity > used ? capacity - used : 0; 
    }
    
    /**
     * @brief 获取使用率
     */
    double utilizationRatio() const {
        return capacity > 0 ? static_cast<double>(used) / capacity : 0.0;
    }
};

/**
 * @brief 存储位置（扁平化）
 * 
 * 表示数据在扁平化存储系统中的具体位置。
 * 不包含冷热信息，仅包含访问数据所需的信息。
 */
struct FlatStorageLocation {
    std::string segment_id;          // 所属段ID
    StorageMedium medium;            // 存储介质类型
    uint64_t offset;                 // 段内偏移
    size_t size;                     // 数据大小
    std::string transport_endpoint;  // Transfer Engine端点
    
    /**
     * @brief 获取绝对地址（如果适用）
     */
    uint64_t absoluteAddress(uint64_t segment_base) const {
        return segment_base + offset;
    }
};

/**
 * @brief 放置配置（扁平化版本）
 * 
 * 替代原有的ReplicateConfig，移除了所有与冷热相关的配置。
 * 
 * 与原始ReplicateConfig的区别：
 * - 移除: with_soft_pin（冷热相关）
 * - 新增: policy（放置策略）
 * - 新增: preferred_mediums（首选介质）
 * - 新增: allow_any_medium（是否允许任意介质）
 */
struct FlatPlacementConfig {
    PlacementPolicy policy = PlacementPolicy::CAPACITY_FIRST;
    size_t replica_num = 1;
    std::vector<StorageMedium> preferred_mediums;   // 首选存储介质
    std::vector<std::string> preferred_segments;    // 首选存储段
    bool allow_any_medium = true;                   // 是否允许使用任意介质
    bool prefer_alloc_in_same_node = false;         // 是否优先同节点分配
    
    // 以下字段在扁平化架构中被移除或废弃：
    // bool with_soft_pin = false;    // 移除：不再区分冷热
    // 数据不会因为"冷"而被迁移
    
    friend std::ostream& operator<<(std::ostream& os,
                                    const FlatPlacementConfig& config) noexcept {
        os << "FlatPlacementConfig: { policy: " << config.policy
           << ", replica_num: " << config.replica_num
           << ", preferred_mediums: [";
        for (size_t i = 0; i < config.preferred_mediums.size(); ++i) {
            os << config.preferred_mediums[i];
            if (i < config.preferred_mediums.size() - 1) os << ", ";
        }
        os << "], preferred_segments: [";
        for (size_t i = 0; i < config.preferred_segments.size(); ++i) {
            os << config.preferred_segments[i];
            if (i < config.preferred_segments.size() - 1) os << ", ";
        }
        os << "], allow_any_medium: " << config.allow_any_medium
           << ", prefer_alloc_in_same_node: " << config.prefer_alloc_in_same_node
           << " }";
        return os;
    }
};

/**
 * @brief 扁平化对象元数据
 * 
 * 存储对象的元数据，不包含任何冷热信息。
 */
struct FlatObjectMetadata {
    std::string key;
    std::vector<FlatStorageLocation> replica_locations;
    size_t total_size;
    uint64_t create_timestamp;
    
    // 不再跟踪以下信息（冷热相关）：
    // uint64_t access_count;
    // uint64_t last_access_time;
    // bool is_hot;
};

/**
 * @brief 容量统计信息
 */
struct FlatCapacityStats {
    size_t total_capacity = 0;
    size_t total_used = 0;
    std::unordered_map<StorageMedium, size_t> capacity_by_medium;
    std::unordered_map<StorageMedium, size_t> used_by_medium;
    
    double totalUtilization() const {
        return total_capacity > 0 
            ? static_cast<double>(total_used) / total_capacity 
            : 0.0;
    }
    
    double utilizationByMedium(StorageMedium medium) const {
        auto cap_it = capacity_by_medium.find(medium);
        auto used_it = used_by_medium.find(medium);
        if (cap_it == capacity_by_medium.end() || 
            used_it == used_by_medium.end() ||
            cap_it->second == 0) {
            return 0.0;
        }
        return static_cast<double>(used_it->second) / cap_it->second;
    }
};

/**
 * @brief Flat Memory系统配置
 */
struct FlatMemoryConfig {
    bool enabled = false;                           // 是否启用扁平化模式
    PlacementPolicy default_policy = 
        PlacementPolicy::CAPACITY_FIRST;            // 默认放置策略
    bool disable_auto_migration = true;             // 禁用自动迁移（核心改变）
    bool disable_eviction = true;                   // 禁用基于冷热的驱逐
    std::vector<StorageMedium> available_mediums;   // 可用存储介质列表
    
    // 策略相关配置
    uint64_t latency_threshold_ns = 1000;           // 延迟优先策略的阈值
    double capacity_balance_factor = 0.8;           // 容量均衡因子
    
    /**
     * @brief 从环境变量加载配置
     */
    static FlatMemoryConfig FromEnvironment();
};

}  // namespace flat
}  // namespace mooncake
