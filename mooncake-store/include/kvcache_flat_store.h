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
 * @file kvcache_flat_store.h
 * @brief KVCache Flat Memory Store - 统一 KVCache 存储接口
 * 
 * 提供 KVCache 对象级别的存储操作，基于 UnifiedSegmentManager。
 * 
 * 核心特点：
 * 1. 数据不重复存储：每个 KVCache 只有一个位置
 * 2. 统一地址空间：通过 UnifiedAddress 访问所有数据
 * 3. 无冷热区分：基于放置策略决定存储位置
 */

#pragma once

#include "unified_segment_manager.h"
#include <unordered_map>
#include <shared_mutex>
#include <chrono>

namespace mooncake {
namespace flat {

/**
 * @brief KVCache 对象元数据
 * 
 * 关键设计：location 是唯一的，数据不重复存储
 */
struct KVCacheObjectMeta {
    std::string key;                 // 对象键
    UnifiedAddress location;         // **唯一**存储位置
    size_t size;                     // 数据大小
    UnifiedMediumType medium;        // 存储介质（冗余，方便查询）
    
    // 元数据（不用于冷热判断，仅用于统计）
    uint64_t create_time;
    uint64_t last_access_time;
    uint32_t access_count;
    
    // 可选：多副本支持（用于容错，不是因为冷热）
    std::vector<UnifiedAddress> replica_locations;
};

/**
 * @brief Put 配置
 */
struct KVCachePutConfig {
    UnifiedPlacementPolicy policy = UnifiedPlacementPolicy::LATENCY_FIRST;
    std::vector<UnifiedMediumType> preferred_mediums;
    bool allow_remote = true;
    uint32_t max_latency_ns = UINT32_MAX;
    size_t replica_count = 1;  // 副本数（用于容错，不是冷热）
};

/**
 * @brief KVCache Flat Store - 核心存储类
 */
class KVCacheFlatStore {
public:
    explicit KVCacheFlatStore(
        std::shared_ptr<UnifiedSegmentManager> segment_manager)
        : segment_manager_(segment_manager) {}
    
    ~KVCacheFlatStore() = default;
    
    // ==================== 基本操作 ====================
    
    /**
     * @brief 存储 KVCache 对象
     * 
     * 与原 Mooncake 的区别：
     * - 数据只存储一份（除非显式配置副本）
     * - 存储位置由放置策略决定，不是冷热
     * 
     * @param key 对象键
     * @param data 数据指针
     * @param size 数据大小
     * @param config 放置配置
     * @return 0 成功，<0 失败
     */
    int Put(const std::string& key, const void* data, size_t size,
            const KVCachePutConfig& config = KVCachePutConfig{}) {
        
        // 检查是否已存在
        {
            std::shared_lock<std::shared_mutex> lock(meta_mutex_);
            if (object_map_.find(key) != object_map_.end()) {
                return -1;  // 已存在，使用 Update 或先 Remove
            }
        }
        
        // 构建分配请求
        UnifiedAllocationRequest alloc_req;
        alloc_req.size = size;
        alloc_req.policy = config.policy;
        alloc_req.preferred_mediums = config.preferred_mediums;
        alloc_req.allow_remote = config.allow_remote;
        alloc_req.max_latency_ns = config.max_latency_ns;
        
        // 分配空间
        auto result = segment_manager_->Allocate(alloc_req);
        if (!result) {
            return -2;  // 空间不足
        }
        
        // 写入数据
        int ret = segment_manager_->Write(result->ToAddress(), data, size);
        if (ret != 0) {
            segment_manager_->Deallocate(result->ToAddress(), size);
            return -3;  // 写入失败
        }
        
        // 注册元数据
        KVCacheObjectMeta meta;
        meta.key = key;
        meta.location = result->ToAddress();
        meta.size = size;
        meta.medium = result->medium;
        meta.create_time = GetCurrentTimeMs();
        meta.last_access_time = meta.create_time;
        meta.access_count = 0;
        
        // 处理副本（可选）
        if (config.replica_count > 1) {
            for (size_t i = 1; i < config.replica_count; ++i) {
                auto replica_result = segment_manager_->Allocate(alloc_req);
                if (replica_result) {
                    segment_manager_->Write(replica_result->ToAddress(), 
                                            data, size);
                    meta.replica_locations.push_back(replica_result->ToAddress());
                }
            }
        }
        
        {
            std::unique_lock<std::shared_mutex> lock(meta_mutex_);
            object_map_[key] = meta;
        }
        
        return 0;
    }
    
    /**
     * @brief 获取 KVCache 对象
     * 
     * @param key 对象键
     * @param buffer 目标缓冲区
     * @param buffer_size 缓冲区大小
     * @return 实际读取的字节数，<0 失败
     */
    ssize_t Get(const std::string& key, void* buffer, size_t buffer_size) {
        KVCacheObjectMeta meta;
        
        // 查找元数据
        {
            std::shared_lock<std::shared_mutex> lock(meta_mutex_);
            auto it = object_map_.find(key);
            if (it == object_map_.end()) {
                return -1;  // 不存在
            }
            meta = it->second;
        }
        
        if (buffer_size < meta.size) {
            return -2;  // 缓冲区太小
        }
        
        // 从唯一位置读取
        int ret = segment_manager_->Read(meta.location, buffer, meta.size);
        if (ret != 0) {
            return -3;  // 读取失败
        }
        
        // 更新访问统计（可选，不用于冷热判断）
        {
            std::unique_lock<std::shared_mutex> lock(meta_mutex_);
            auto it = object_map_.find(key);
            if (it != object_map_.end()) {
                it->second.last_access_time = GetCurrentTimeMs();
                it->second.access_count++;
            }
        }
        
        return static_cast<ssize_t>(meta.size);
    }
    
    /**
     * @brief 异步获取 KVCache 对象
     */
    int AsyncGet(const std::string& key, void* buffer, size_t buffer_size,
                 std::function<void(int status, size_t bytes)> callback) {
        KVCacheObjectMeta meta;
        
        {
            std::shared_lock<std::shared_mutex> lock(meta_mutex_);
            auto it = object_map_.find(key);
            if (it == object_map_.end()) {
                if (callback) callback(-1, 0);
                return -1;
            }
            meta = it->second;
        }
        
        if (buffer_size < meta.size) {
            if (callback) callback(-2, 0);
            return -2;
        }
        
        return segment_manager_->AsyncRead(meta.location, buffer, meta.size,
            [this, key, callback](int status, size_t bytes) {
                if (status == 0) {
                    // 更新访问统计
                    std::unique_lock<std::shared_mutex> lock(meta_mutex_);
                    auto it = object_map_.find(key);
                    if (it != object_map_.end()) {
                        it->second.last_access_time = GetCurrentTimeMs();
                        it->second.access_count++;
                    }
                }
                if (callback) callback(status, bytes);
            });
    }
    
    /**
     * @brief 删除 KVCache 对象
     */
    int Remove(const std::string& key) {
        KVCacheObjectMeta meta;
        
        {
            std::unique_lock<std::shared_mutex> lock(meta_mutex_);
            auto it = object_map_.find(key);
            if (it == object_map_.end()) {
                return -1;  // 不存在
            }
            meta = it->second;
            object_map_.erase(it);
        }
        
        // 释放主存储位置
        segment_manager_->Deallocate(meta.location, meta.size);
        
        // 释放副本位置
        for (const auto& replica : meta.replica_locations) {
            segment_manager_->Deallocate(replica, meta.size);
        }
        
        return 0;
    }
    
    /**
     * @brief 检查对象是否存在
     */
    bool Exists(const std::string& key) const {
        std::shared_lock<std::shared_mutex> lock(meta_mutex_);
        return object_map_.find(key) != object_map_.end();
    }
    
    /**
     * @brief 获取对象元数据
     */
    std::optional<KVCacheObjectMeta> GetMeta(const std::string& key) const {
        std::shared_lock<std::shared_mutex> lock(meta_mutex_);
        auto it = object_map_.find(key);
        if (it == object_map_.end()) return std::nullopt;
        return it->second;
    }
    
    // ==================== 批量操作 ====================
    
    /**
     * @brief 批量存储
     */
    std::vector<int> BatchPut(
        const std::vector<std::pair<std::string, std::pair<const void*, size_t>>>& items,
        const KVCachePutConfig& config = KVCachePutConfig{}) {
        
        std::vector<int> results;
        results.reserve(items.size());
        
        for (const auto& [key, data_pair] : items) {
            results.push_back(Put(key, data_pair.first, data_pair.second, config));
        }
        
        return results;
    }
    
    /**
     * @brief 批量获取
     */
    std::vector<ssize_t> BatchGet(
        const std::vector<std::tuple<std::string, void*, size_t>>& requests) {
        
        std::vector<ssize_t> results;
        results.reserve(requests.size());
        
        for (const auto& [key, buffer, size] : requests) {
            results.push_back(Get(key, buffer, size));
        }
        
        return results;
    }
    
    // ==================== 高级操作 ====================
    
    /**
     * @brief 迁移对象到指定介质
     * 
     * 注意：这是显式迁移，不是基于冷热的自动迁移
     */
    int MigrateTo(const std::string& key, UnifiedMediumType target_medium) {
        KVCacheObjectMeta meta;
        std::vector<char> buffer;
        
        // 获取元数据和数据
        {
            std::shared_lock<std::shared_mutex> lock(meta_mutex_);
            auto it = object_map_.find(key);
            if (it == object_map_.end()) return -1;
            meta = it->second;
        }
        
        // 如果已经在目标介质，无需迁移
        if (meta.medium == target_medium) return 0;
        
        // 读取数据
        buffer.resize(meta.size);
        int ret = segment_manager_->Read(meta.location, buffer.data(), meta.size);
        if (ret != 0) return -2;
        
        // 在目标介质分配新空间
        UnifiedAllocationRequest alloc_req;
        alloc_req.size = meta.size;
        alloc_req.preferred_mediums = {target_medium};
        
        auto result = segment_manager_->Allocate(alloc_req);
        if (!result) return -3;
        
        // 写入新位置
        ret = segment_manager_->Write(result->ToAddress(), buffer.data(), meta.size);
        if (ret != 0) {
            segment_manager_->Deallocate(result->ToAddress(), meta.size);
            return -4;
        }
        
        // 更新元数据
        UnifiedAddress old_location = meta.location;
        {
            std::unique_lock<std::shared_mutex> lock(meta_mutex_);
            auto it = object_map_.find(key);
            if (it != object_map_.end()) {
                it->second.location = result->ToAddress();
                it->second.medium = result->medium;
            }
        }
        
        // 释放旧空间
        segment_manager_->Deallocate(old_location, meta.size);
        
        return 0;
    }
    
    /**
     * @brief 获取所有对象键
     */
    std::vector<std::string> GetAllKeys() const {
        std::shared_lock<std::shared_mutex> lock(meta_mutex_);
        
        std::vector<std::string> keys;
        keys.reserve(object_map_.size());
        
        for (const auto& [key, meta] : object_map_) {
            keys.push_back(key);
        }
        
        return keys;
    }
    
    /**
     * @brief 按介质类型获取对象
     */
    std::vector<std::string> GetKeysByMedium(UnifiedMediumType medium) const {
        std::shared_lock<std::shared_mutex> lock(meta_mutex_);
        
        std::vector<std::string> keys;
        
        for (const auto& [key, meta] : object_map_) {
            if (meta.medium == medium) {
                keys.push_back(key);
            }
        }
        
        return keys;
    }
    
    /**
     * @brief 获取存储统计
     */
    struct StorageStats {
        size_t object_count = 0;
        size_t total_data_size = 0;
        std::unordered_map<UnifiedMediumType, size_t> count_by_medium;
        std::unordered_map<UnifiedMediumType, size_t> size_by_medium;
    };
    
    StorageStats GetStats() const {
        std::shared_lock<std::shared_mutex> lock(meta_mutex_);
        
        StorageStats stats;
        stats.object_count = object_map_.size();
        
        for (const auto& [key, meta] : object_map_) {
            stats.total_data_size += meta.size;
            stats.count_by_medium[meta.medium]++;
            stats.size_by_medium[meta.medium] += meta.size;
        }
        
        return stats;
    }

private:
    static uint64_t GetCurrentTimeMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
    }
    
    std::shared_ptr<UnifiedSegmentManager> segment_manager_;
    
    mutable std::shared_mutex meta_mutex_;
    std::unordered_map<std::string, KVCacheObjectMeta> object_map_;
};

}  // namespace flat
}  // namespace mooncake
