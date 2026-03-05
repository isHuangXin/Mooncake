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

#include <memory>
#include <unordered_map>
#include <vector>
#include <shared_mutex>
#include <atomic>
#include <optional>

#include "flat_memory_types.h"
#include "types.h"
#include "allocator.h"

namespace mooncake {
namespace flat {

/**
 * @brief 扁平内存管理器 - KVCache Flat Memory System的核心组件
 * 
 * 这是实现扁平化存储的核心类，它将所有存储介质（GPU HBM、DRAM、SSD）
 * 视为同一层级的存储池，不再区分冷热数据。
 * 
 * 核心设计原则：
 * 1. 存储位置透明化：应用层不需要知道数据存储在哪个介质
 * 2. 无冷热区分：所有数据被同等对待，不基于访问频率迁移
 * 3. 统一地址空间：通过统一接口访问所有存储
 * 4. 基于策略的放置：可配置的放置策略（容量优先、延迟优先等）
 * 
 * 与原始Mooncake的区别：
 * - 原始架构：分层存储，热数据在HBM，冷数据offload到SSD
 * - 扁平架构：所有存储等价，基于放置策略而非冷热决定存储位置
 */
class FlatMemoryManager : public std::enable_shared_from_this<FlatMemoryManager> {
public:
    FlatMemoryManager();
    explicit FlatMemoryManager(const FlatMemoryConfig& config);
    ~FlatMemoryManager();
    
    // 禁用拷贝
    FlatMemoryManager(const FlatMemoryManager&) = delete;
    FlatMemoryManager& operator=(const FlatMemoryManager&) = delete;
    
    /**
     * @brief 初始化管理器
     * @param config 扁平化内存配置
     * @return 成功返回OK，否则返回错误码
     */
    ErrorCode Initialize(const FlatMemoryConfig& config);
    
    /**
     * @brief 注册存储段（不区分类型，统一管理）
     * 
     * 在扁平化架构中，所有类型的存储段都通过此接口注册，
     * 不会因为存储介质类型而有不同的处理。
     * 
     * @param descriptor 段描述符
     * @return 成功返回OK，如果段已存在返回SEGMENT_ALREADY_EXISTS
     */
    ErrorCode RegisterSegment(const FlatSegmentDescriptor& descriptor);
    
    /**
     * @brief 批量注册存储段
     * @param descriptors 段描述符列表
     * @return 成功注册的数量
     */
    size_t RegisterSegmentBatch(const std::vector<FlatSegmentDescriptor>& descriptors);
    
    /**
     * @brief 注销存储段
     * @param segment_id 段ID
     * @return 成功返回OK，如果段不存在返回SEGMENT_NOT_FOUND
     */
    ErrorCode UnregisterSegment(const std::string& segment_id);
    
    /**
     * @brief 分配存储空间（基于放置策略，不基于冷热）
     * 
     * 这是与原始Mooncake最大的区别：
     * - 原始架构：热数据优先分配到HBM，冷数据分配到SSD
     * - 扁平架构：基于PlacementPolicy决定分配位置，不考虑数据冷热
     * 
     * @param size 请求的大小
     * @param config 放置配置
     * @return 分配的存储位置，或错误码
     */
    tl::expected<FlatStorageLocation, ErrorCode> Allocate(
        size_t size,
        const FlatPlacementConfig& config = FlatPlacementConfig{});
    
    /**
     * @brief 批量分配存储空间
     * 
     * @param sizes 请求的大小列表
     * @param config 放置配置（所有请求使用相同配置）
     * @return 分配的存储位置列表，或错误码
     */
    tl::expected<std::vector<FlatStorageLocation>, ErrorCode> AllocateBatch(
        const std::vector<size_t>& sizes,
        const FlatPlacementConfig& config = FlatPlacementConfig{});
    
    /**
     * @brief 为多副本分配空间
     * 
     * 确保副本分布在不同的段上以保证冗余性。
     * 
     * @param size 每个副本的大小
     * @param replica_num 副本数量
     * @param config 放置配置
     * @return 副本位置列表，或错误码
     */
    tl::expected<std::vector<FlatStorageLocation>, ErrorCode> AllocateReplicas(
        size_t size,
        size_t replica_num,
        const FlatPlacementConfig& config = FlatPlacementConfig{});
    
    /**
     * @brief 释放存储空间
     * @param location 存储位置
     * @return 成功返回OK，否则返回错误码
     */
    ErrorCode Deallocate(const FlatStorageLocation& location);
    
    /**
     * @brief 批量释放存储空间
     * @param locations 存储位置列表
     * @return 成功释放的数量
     */
    size_t DeallocateBatch(const std::vector<FlatStorageLocation>& locations);
    
    /**
     * @brief 获取指定段的信息
     * @param segment_id 段ID
     * @return 段描述符（如果存在）
     */
    std::optional<FlatSegmentDescriptor> GetSegment(const std::string& segment_id) const;
    
    /**
     * @brief 获取所有可用段（不分层）
     * @return 所有段的描述符列表
     */
    std::vector<FlatSegmentDescriptor> GetAllSegments() const;
    
    /**
     * @brief 按存储介质类型获取段
     * @param medium 存储介质类型
     * @return 指定类型的段列表
     */
    std::vector<FlatSegmentDescriptor> GetSegmentsByMedium(StorageMedium medium) const;
    
    /**
     * @brief 获取容量统计信息（跨所有介质）
     * @return 容量统计
     */
    FlatCapacityStats GetCapacityStats() const;
    
    /**
     * @brief 检查是否有足够空间
     * @param required_size 需要的空间大小
     * @return 是否有足够空间
     */
    bool HasAvailableSpace(size_t required_size) const;
    
    /**
     * @brief 检查指定介质是否有足够空间
     * @param medium 存储介质类型
     * @param required_size 需要的空间大小
     * @return 是否有足够空间
     */
    bool HasAvailableSpaceInMedium(StorageMedium medium, size_t required_size) const;
    
    /**
     * @brief 获取当前配置
     */
    const FlatMemoryConfig& GetConfig() const { return config_; }
    
    /**
     * @brief 更新放置策略
     * @param policy 新的放置策略
     */
    void SetDefaultPlacementPolicy(PlacementPolicy policy);
    
    /**
     * @brief 获取段数量
     */
    size_t GetSegmentCount() const;

private:
    /**
     * @brief 基于策略选择存储段
     * 
     * 核心方法：根据配置的放置策略选择合适的存储段。
     * 不考虑数据冷热，仅考虑：
     * - 可用空间
     * - 放置策略（容量/延迟/轮询/随机）
     * - 首选介质/段配置
     */
    tl::expected<std::string, ErrorCode> SelectSegment(
        size_t required_size,
        const FlatPlacementConfig& config);
    
    /**
     * @brief 容量优先选择算法
     * 选择可用容量最大的段，不考虑介质类型
     */
    std::optional<std::string> SelectByCapacity(size_t required_size);
    
    /**
     * @brief 延迟优先选择算法
     * 选择估计延迟最低的段
     */
    std::optional<std::string> SelectByLatency(size_t required_size);
    
    /**
     * @brief 轮询选择算法
     * 在所有可用段间轮询
     */
    std::optional<std::string> SelectByRoundRobin(size_t required_size);
    
    /**
     * @brief 随机选择算法
     */
    std::optional<std::string> SelectRandom(size_t required_size);
    
    /**
     * @brief 位置感知选择算法
     * 考虑NUMA拓扑和数据亲和性
     */
    std::optional<std::string> SelectByLocality(
        size_t required_size,
        const FlatPlacementConfig& config);
    
    /**
     * @brief 从首选介质中选择
     */
    std::optional<std::string> SelectFromPreferredMediums(
        size_t required_size,
        const std::vector<StorageMedium>& preferred_mediums);
    
    /**
     * @brief 从首选段中选择
     */
    std::optional<std::string> SelectFromPreferredSegments(
        size_t required_size,
        const std::vector<std::string>& preferred_segments);
    
    /**
     * @brief 获取满足空间要求的段列表
     */
    std::vector<std::string> GetAvailableSegments(size_t required_size) const;

private:
    mutable std::shared_mutex mutex_;
    
    // 配置
    FlatMemoryConfig config_;
    
    // 段管理（不分层，统一管理）
    std::unordered_map<std::string, FlatSegmentDescriptor> segments_;
    
    // 段到分配器的映射（如果需要精细化分配）
    std::unordered_map<std::string, std::shared_ptr<BufferAllocatorBase>> allocators_;
    
    // 轮询索引
    std::atomic<size_t> round_robin_index_{0};
    
    // 统计信息
    std::atomic<size_t> total_allocations_{0};
    std::atomic<size_t> total_deallocations_{0};
    
    // 初始化标志
    std::atomic<bool> initialized_{false};
};

/**
 * @brief 扁平内存管理器的Builder模式
 * 
 * 方便创建和配置FlatMemoryManager
 */
class FlatMemoryManagerBuilder {
public:
    FlatMemoryManagerBuilder& WithConfig(const FlatMemoryConfig& config) {
        config_ = config;
        return *this;
    }
    
    FlatMemoryManagerBuilder& WithPlacementPolicy(PlacementPolicy policy) {
        config_.default_policy = policy;
        return *this;
    }
    
    FlatMemoryManagerBuilder& DisableAutoMigration(bool disable = true) {
        config_.disable_auto_migration = disable;
        return *this;
    }
    
    FlatMemoryManagerBuilder& DisableEviction(bool disable = true) {
        config_.disable_eviction = disable;
        return *this;
    }
    
    FlatMemoryManagerBuilder& AddMedium(StorageMedium medium) {
        config_.available_mediums.push_back(medium);
        return *this;
    }
    
    std::shared_ptr<FlatMemoryManager> Build() {
        auto manager = std::make_shared<FlatMemoryManager>(config_);
        return manager;
    }

private:
    FlatMemoryConfig config_;
};

}  // namespace flat
}  // namespace mooncake
