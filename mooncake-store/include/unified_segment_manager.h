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
 * @file unified_segment_manager.h
 * @brief KVCache Flat Memory System - 统一段管理器
 * 
 * 管理所有类型的存储段，提供统一的空间分配和数据访问接口。
 * 这是 Flat Memory System 的核心组件。
 */

#pragma once

#include "unified_segment.h"
#include "segment_impls.h"
#include <unordered_map>
#include <shared_mutex>
#include <vector>
#include <random>
#include <algorithm>

namespace mooncake {
namespace flat {

/**
 * @brief 统一放置策略
 */
enum class UnifiedPlacementPolicy {
    LATENCY_FIRST,      // 延迟优先 (HBM → Local DRAM → Remote DRAM → SSD)
    CAPACITY_FIRST,     // 容量优先 (SSD → DRAM → HBM)
    ROUND_ROBIN,        // 轮询
    LOCALITY_AWARE,     // 位置感知 (优先本地)
    RANDOM              // 随机
};

/**
 * @brief 分配请求
 */
struct UnifiedAllocationRequest {
    size_t size;
    UnifiedPlacementPolicy policy = UnifiedPlacementPolicy::LATENCY_FIRST;
    std::vector<UnifiedMediumType> preferred_mediums;  // 首选介质
    bool allow_remote = true;
    uint32_t max_latency_ns = UINT32_MAX;  // 延迟约束
};

/**
 * @brief 分配结果
 */
struct UnifiedAllocationResult {
    uint16_t segment_id;
    uint64_t offset;
    size_t size;
    UnifiedMediumType medium;
    
    UnifiedAddress ToAddress() const {
        return UnifiedAddress(segment_id, offset);
    }
};

/**
 * @brief 统一段管理器 - Flat Memory System 核心
 * 
 * 职责：
 * 1. 管理所有类型的存储段（HBM/DRAM/SSD，本地/远程）
 * 2. 提供统一的空间分配接口
 * 3. 提供统一的数据读写接口
 * 4. 实现各种放置策略
 */
class UnifiedSegmentManager {
public:
    UnifiedSegmentManager() = default;
    ~UnifiedSegmentManager() = default;
    
    // 禁用拷贝
    UnifiedSegmentManager(const UnifiedSegmentManager&) = delete;
    UnifiedSegmentManager& operator=(const UnifiedSegmentManager&) = delete;
    
    // ==================== 段注册 ====================
    
    /**
     * @brief 注册段
     * @param segment 段实例
     * @return 分配的段 ID
     */
    uint16_t RegisterSegment(std::unique_ptr<IUnifiedSegment> segment) {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        
        uint16_t id = next_segment_id_++;
        
        auto medium = segment->GetMedium();
        segment_map_[id] = std::move(segment);
        medium_index_[medium].push_back(id);
        
        return id;
    }
    
    /**
     * @brief 通过描述符注册段
     */
    uint16_t RegisterSegmentFromDesc(const UnifiedSegmentDesc& desc,
                                     void* memory_ptr = nullptr,
                                     std::shared_ptr<TransferEngine> engine = nullptr) {
        auto segment = CreateUnifiedSegment(desc, memory_ptr, engine);
        if (!segment) return 0;
        return RegisterSegment(std::move(segment));
    }
    
    /**
     * @brief 注销段
     */
    bool UnregisterSegment(uint16_t segment_id) {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        
        auto it = segment_map_.find(segment_id);
        if (it == segment_map_.end()) return false;
        
        auto medium = it->second->GetMedium();
        segment_map_.erase(it);
        
        // 从介质索引中移除
        auto& vec = medium_index_[medium];
        vec.erase(std::remove(vec.begin(), vec.end(), segment_id), vec.end());
        
        return true;
    }
    
    // ==================== 空间分配 ====================
    
    /**
     * @brief 分配空间
     * @param req 分配请求
     * @return 分配结果，或 nullopt 如果失败
     */
    std::optional<UnifiedAllocationResult> Allocate(
        const UnifiedAllocationRequest& req) {
        
        std::unique_lock<std::shared_mutex> lock(mutex_);
        
        // 获取候选段
        auto candidates = GetCandidatesByPolicy(req);
        
        for (uint16_t seg_id : candidates) {
            auto it = segment_map_.find(seg_id);
            if (it == segment_map_.end()) continue;
            
            auto& segment = it->second;
            
            // 检查延迟约束
            if (req.max_latency_ns < segment->GetDesc().latency_ns) {
                continue;
            }
            
            // 检查本地性约束
            if (!req.allow_remote && segment->GetDesc().IsRemote()) {
                continue;
            }
            
            // 尝试分配
            uint64_t offset = segment->AllocateSpace(req.size);
            if (offset != UINT64_MAX) {
                return UnifiedAllocationResult{
                    .segment_id = seg_id,
                    .offset = offset,
                    .size = req.size,
                    .medium = segment->GetMedium()
                };
            }
        }
        
        return std::nullopt;  // 分配失败
    }
    
    /**
     * @brief 释放空间
     */
    void Deallocate(const UnifiedAddress& addr, size_t size) {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        
        auto it = segment_map_.find(addr.segment_id);
        if (it != segment_map_.end()) {
            it->second->DeallocateSpace(addr.offset, size);
        }
    }
    
    // ==================== 数据访问 ====================
    
    /**
     * @brief 同步读取
     */
    int Read(const UnifiedAddress& addr, void* buffer, size_t length) {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        
        auto it = segment_map_.find(addr.segment_id);
        if (it == segment_map_.end()) return -1;
        
        return it->second->Read(addr.offset, buffer, length);
    }
    
    /**
     * @brief 同步写入
     */
    int Write(const UnifiedAddress& addr, const void* data, size_t length) {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        
        auto it = segment_map_.find(addr.segment_id);
        if (it == segment_map_.end()) return -1;
        
        return it->second->Write(addr.offset, data, length);
    }
    
    /**
     * @brief 异步读取
     */
    int AsyncRead(const UnifiedAddress& addr, void* buffer, size_t length,
                  IOCompletionCallback callback) {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        
        auto it = segment_map_.find(addr.segment_id);
        if (it == segment_map_.end()) {
            if (callback) callback(-1, 0);
            return -1;
        }
        
        return it->second->AsyncRead(addr.offset, buffer, length, callback);
    }
    
    /**
     * @brief 异步写入
     */
    int AsyncWrite(const UnifiedAddress& addr, const void* data, size_t length,
                   IOCompletionCallback callback) {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        
        auto it = segment_map_.find(addr.segment_id);
        if (it == segment_map_.end()) {
            if (callback) callback(-1, 0);
            return -1;
        }
        
        return it->second->AsyncWrite(addr.offset, data, length, callback);
    }
    
    // ==================== 查询接口 ====================
    
    /**
     * @brief 获取段信息
     */
    std::optional<UnifiedSegmentDesc> GetSegmentDesc(uint16_t segment_id) const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        
        auto it = segment_map_.find(segment_id);
        if (it == segment_map_.end()) return std::nullopt;
        
        return it->second->GetDesc();
    }
    
    /**
     * @brief 获取所有段
     */
    std::vector<UnifiedSegmentDesc> GetAllSegments() const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        
        std::vector<UnifiedSegmentDesc> result;
        result.reserve(segment_map_.size());
        
        for (const auto& [id, seg] : segment_map_) {
            result.push_back(seg->GetDesc());
        }
        return result;
    }
    
    /**
     * @brief 按介质类型获取段
     */
    std::vector<UnifiedSegmentDesc> GetSegmentsByMedium(
        UnifiedMediumType medium) const {
        
        std::shared_lock<std::shared_mutex> lock(mutex_);
        
        std::vector<UnifiedSegmentDesc> result;
        
        auto it = medium_index_.find(medium);
        if (it == medium_index_.end()) return result;
        
        for (uint16_t id : it->second) {
            auto seg_it = segment_map_.find(id);
            if (seg_it != segment_map_.end()) {
                result.push_back(seg_it->second->GetDesc());
            }
        }
        
        return result;
    }
    
    /**
     * @brief 获取总容量统计
     */
    struct CapacityStats {
        size_t total_capacity = 0;
        size_t total_used = 0;
        std::unordered_map<UnifiedMediumType, size_t> capacity_by_medium;
        std::unordered_map<UnifiedMediumType, size_t> used_by_medium;
    };
    
    CapacityStats GetCapacityStats() const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        
        CapacityStats stats;
        
        for (const auto& [id, seg] : segment_map_) {
            auto cap = seg->GetCapacity();
            auto used = seg->GetUsedSpace();
            auto medium = seg->GetMedium();
            
            stats.total_capacity += cap;
            stats.total_used += used;
            stats.capacity_by_medium[medium] += cap;
            stats.used_by_medium[medium] += used;
        }
        
        return stats;
    }
    
    /**
     * @brief 获取段数量
     */
    size_t GetSegmentCount() const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return segment_map_.size();
    }

private:
    std::vector<uint16_t> GetCandidatesByPolicy(
        const UnifiedAllocationRequest& req) {
        
        std::vector<uint16_t> candidates;
        
        // 如果指定了首选介质
        if (!req.preferred_mediums.empty()) {
            for (auto medium : req.preferred_mediums) {
                auto it = medium_index_.find(medium);
                if (it != medium_index_.end()) {
                    for (uint16_t id : it->second) {
                        candidates.push_back(id);
                    }
                }
            }
            // 如果首选介质有候选，直接返回
            if (!candidates.empty()) return candidates;
        }
        
        // 按策略排序
        std::vector<UnifiedMediumType> order;
        
        switch (req.policy) {
            case UnifiedPlacementPolicy::LATENCY_FIRST:
                order = {
                    UnifiedMediumType::GPU_HBM,
                    UnifiedMediumType::LOCAL_DRAM,
                    UnifiedMediumType::REMOTE_DRAM,
                    UnifiedMediumType::LOCAL_SSD,
                    UnifiedMediumType::REMOTE_SSD
                };
                break;
                
            case UnifiedPlacementPolicy::CAPACITY_FIRST:
                order = {
                    UnifiedMediumType::REMOTE_SSD,
                    UnifiedMediumType::LOCAL_SSD,
                    UnifiedMediumType::REMOTE_DRAM,
                    UnifiedMediumType::LOCAL_DRAM,
                    UnifiedMediumType::GPU_HBM
                };
                break;
                
            case UnifiedPlacementPolicy::LOCALITY_AWARE:
                order = {
                    UnifiedMediumType::GPU_HBM,
                    UnifiedMediumType::LOCAL_DRAM,
                    UnifiedMediumType::LOCAL_SSD,
                    UnifiedMediumType::REMOTE_DRAM,
                    UnifiedMediumType::REMOTE_SSD
                };
                break;
                
            case UnifiedPlacementPolicy::ROUND_ROBIN: {
                // 轮询所有段
                for (const auto& [id, seg] : segment_map_) {
                    candidates.push_back(id);
                }
                // 从上次位置开始
                size_t start = round_robin_index_.fetch_add(1) % 
                               candidates.size();
                std::rotate(candidates.begin(), 
                            candidates.begin() + start,
                            candidates.end());
                return candidates;
            }
                
            case UnifiedPlacementPolicy::RANDOM: {
                for (const auto& [id, seg] : segment_map_) {
                    candidates.push_back(id);
                }
                std::random_device rd;
                std::mt19937 g(rd());
                std::shuffle(candidates.begin(), candidates.end(), g);
                return candidates;
            }
        }
        
        // 按顺序添加段
        for (auto medium : order) {
            auto it = medium_index_.find(medium);
            if (it != medium_index_.end()) {
                for (uint16_t id : it->second) {
                    candidates.push_back(id);
                }
            }
        }
        
        return candidates;
    }
    
    mutable std::shared_mutex mutex_;
    uint16_t next_segment_id_ = 1;
    
    std::unordered_map<uint16_t, std::unique_ptr<IUnifiedSegment>> segment_map_;
    std::unordered_map<UnifiedMediumType, std::vector<uint16_t>> medium_index_;
    
    std::atomic<size_t> round_robin_index_{0};
};

}  // namespace flat
}  // namespace mooncake
