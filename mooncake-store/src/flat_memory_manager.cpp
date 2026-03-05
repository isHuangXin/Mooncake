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

#include "flat_memory_manager.h"

#include <algorithm>
#include <random>
#include <cstdlib>
#include <glog/logging.h>

#include "utils.h"

namespace mooncake {
namespace flat {

// 环境变量名称
static constexpr const char* ENV_FLAT_MEMORY_ENABLED = "MOONCAKE_FLAT_MEMORY_ENABLED";
static constexpr const char* ENV_FLAT_PLACEMENT_POLICY = "MOONCAKE_FLAT_PLACEMENT_POLICY";
static constexpr const char* ENV_DISABLE_AUTO_MIGRATION = "MOONCAKE_DISABLE_AUTO_MIGRATION";
static constexpr const char* ENV_DISABLE_EVICTION = "MOONCAKE_DISABLE_EVICTION";

FlatMemoryConfig FlatMemoryConfig::FromEnvironment() {
    FlatMemoryConfig config;
    
    // 读取是否启用扁平化模式
    const char* enabled = std::getenv(ENV_FLAT_MEMORY_ENABLED);
    if (enabled != nullptr) {
        std::string enabled_str(enabled);
        std::transform(enabled_str.begin(), enabled_str.end(), 
                       enabled_str.begin(), ::tolower);
        config.enabled = (enabled_str == "true" || enabled_str == "1");
    }
    
    // 读取放置策略
    const char* policy = std::getenv(ENV_FLAT_PLACEMENT_POLICY);
    if (policy != nullptr) {
        std::string policy_str(policy);
        std::transform(policy_str.begin(), policy_str.end(), 
                       policy_str.begin(), ::tolower);
        
        if (policy_str == "capacity_first") {
            config.default_policy = PlacementPolicy::CAPACITY_FIRST;
        } else if (policy_str == "latency_first") {
            config.default_policy = PlacementPolicy::LATENCY_FIRST;
        } else if (policy_str == "round_robin") {
            config.default_policy = PlacementPolicy::ROUND_ROBIN;
        } else if (policy_str == "random") {
            config.default_policy = PlacementPolicy::RANDOM;
        } else if (policy_str == "locality_aware") {
            config.default_policy = PlacementPolicy::LOCALITY_AWARE;
        }
    }
    
    // 读取是否禁用自动迁移
    const char* disable_migration = std::getenv(ENV_DISABLE_AUTO_MIGRATION);
    if (disable_migration != nullptr) {
        std::string disable_str(disable_migration);
        std::transform(disable_str.begin(), disable_str.end(), 
                       disable_str.begin(), ::tolower);
        config.disable_auto_migration = (disable_str == "true" || disable_str == "1");
    }
    
    // 读取是否禁用驱逐
    const char* disable_eviction = std::getenv(ENV_DISABLE_EVICTION);
    if (disable_eviction != nullptr) {
        std::string disable_str(disable_eviction);
        std::transform(disable_str.begin(), disable_str.end(), 
                       disable_str.begin(), ::tolower);
        config.disable_eviction = (disable_str == "true" || disable_str == "1");
    }
    
    return config;
}

FlatMemoryManager::FlatMemoryManager() : config_() {
    LOG(INFO) << "FlatMemoryManager created with default config";
}

FlatMemoryManager::FlatMemoryManager(const FlatMemoryConfig& config) 
    : config_(config) {
    LOG(INFO) << "FlatMemoryManager created with custom config"
              << ", enabled: " << config.enabled
              << ", policy: " << config.default_policy
              << ", disable_migration: " << config.disable_auto_migration
              << ", disable_eviction: " << config.disable_eviction;
    initialized_ = true;
}

FlatMemoryManager::~FlatMemoryManager() {
    LOG(INFO) << "FlatMemoryManager destroyed"
              << ", total_allocations: " << total_allocations_.load()
              << ", total_deallocations: " << total_deallocations_.load();
}

ErrorCode FlatMemoryManager::Initialize(const FlatMemoryConfig& config) {
    std::unique_lock lock(mutex_);
    
    if (initialized_) {
        LOG(WARNING) << "FlatMemoryManager already initialized";
        return ErrorCode::INTERNAL_ERROR;
    }
    
    config_ = config;
    initialized_ = true;
    
    LOG(INFO) << "FlatMemoryManager initialized"
              << ", policy: " << config.default_policy;
    
    return ErrorCode::OK;
}

ErrorCode FlatMemoryManager::RegisterSegment(const FlatSegmentDescriptor& descriptor) {
    std::unique_lock lock(mutex_);
    
    if (segments_.contains(descriptor.segment_id)) {
        LOG(WARNING) << "Segment already registered: " << descriptor.segment_id;
        return ErrorCode::SEGMENT_ALREADY_EXISTS;
    }
    
    segments_[descriptor.segment_id] = descriptor;
    
    LOG(INFO) << "Registered flat segment: " << descriptor.segment_id
              << ", medium: " << descriptor.medium
              << ", capacity: " << descriptor.capacity
              << ", latency_ns: " << descriptor.estimated_latency_ns;
    
    return ErrorCode::OK;
}

size_t FlatMemoryManager::RegisterSegmentBatch(
    const std::vector<FlatSegmentDescriptor>& descriptors) {
    size_t success_count = 0;
    
    for (const auto& desc : descriptors) {
        if (RegisterSegment(desc) == ErrorCode::OK) {
            ++success_count;
        }
    }
    
    return success_count;
}

ErrorCode FlatMemoryManager::UnregisterSegment(const std::string& segment_id) {
    std::unique_lock lock(mutex_);
    
    auto it = segments_.find(segment_id);
    if (it == segments_.end()) {
        LOG(WARNING) << "Segment not found: " << segment_id;
        return ErrorCode::SEGMENT_NOT_FOUND;
    }
    
    segments_.erase(it);
    allocators_.erase(segment_id);
    
    LOG(INFO) << "Unregistered segment: " << segment_id;
    
    return ErrorCode::OK;
}

tl::expected<FlatStorageLocation, ErrorCode> FlatMemoryManager::Allocate(
    size_t size,
    const FlatPlacementConfig& config) {
    
    if (size == 0) {
        return tl::make_unexpected(ErrorCode::INVALID_PARAMS);
    }
    
    // 选择存储段（基于策略，不基于冷热）
    auto segment_result = SelectSegment(size, config);
    if (!segment_result) {
        LOG(WARNING) << "Failed to select segment for size: " << size
                     << ", error: " << segment_result.error();
        return tl::make_unexpected(segment_result.error());
    }
    
    std::unique_lock lock(mutex_);
    
    const std::string& segment_id = segment_result.value();
    auto it = segments_.find(segment_id);
    if (it == segments_.end()) {
        return tl::make_unexpected(ErrorCode::SEGMENT_NOT_FOUND);
    }
    
    auto& descriptor = it->second;
    
    // 再次检查空间（可能被其他线程分配）
    if (descriptor.availableSpace() < size) {
        return tl::make_unexpected(ErrorCode::NO_AVAILABLE_HANDLE);
    }
    
    // 创建存储位置
    FlatStorageLocation location;
    location.segment_id = segment_id;
    location.medium = descriptor.medium;
    location.offset = descriptor.used;
    location.size = size;
    location.transport_endpoint = descriptor.transport_endpoint;
    
    // 更新已使用空间
    descriptor.used += size;
    
    // 更新统计
    ++total_allocations_;
    
    LOG(VLOG_IS_ON(1)) << "Allocated " << size << " bytes in segment " 
                       << segment_id << " (medium: " << descriptor.medium << ")";
    
    return location;
}

tl::expected<std::vector<FlatStorageLocation>, ErrorCode> 
FlatMemoryManager::AllocateBatch(
    const std::vector<size_t>& sizes,
    const FlatPlacementConfig& config) {
    
    std::vector<FlatStorageLocation> locations;
    locations.reserve(sizes.size());
    
    for (size_t size : sizes) {
        auto result = Allocate(size, config);
        if (!result) {
            // 回滚已分配的空间
            for (const auto& loc : locations) {
                Deallocate(loc);
            }
            return tl::make_unexpected(result.error());
        }
        locations.push_back(result.value());
    }
    
    return locations;
}

tl::expected<std::vector<FlatStorageLocation>, ErrorCode> 
FlatMemoryManager::AllocateReplicas(
    size_t size,
    size_t replica_num,
    const FlatPlacementConfig& config) {
    
    if (replica_num == 0) {
        return tl::make_unexpected(ErrorCode::INVALID_PARAMS);
    }
    
    std::vector<FlatStorageLocation> replica_locations;
    replica_locations.reserve(replica_num);
    
    std::set<std::string> used_segments;
    
    for (size_t i = 0; i < replica_num; ++i) {
        // 创建排除已使用段的配置
        FlatPlacementConfig replica_config = config;
        
        // 使用SelectSegment并排除已使用的段
        auto available = GetAvailableSegments(size);
        
        std::string selected_segment;
        for (const auto& seg : available) {
            if (used_segments.find(seg) == used_segments.end()) {
                selected_segment = seg;
                break;
            }
        }
        
        if (selected_segment.empty()) {
            // 如果无法找到新的段，采用best-effort策略
            if (replica_locations.empty()) {
                return tl::make_unexpected(ErrorCode::NO_AVAILABLE_HANDLE);
            }
            LOG(WARNING) << "Could not allocate all " << replica_num 
                         << " replicas, allocated " << replica_locations.size();
            break;
        }
        
        // 直接在选定的段中分配
        std::unique_lock lock(mutex_);
        auto& descriptor = segments_[selected_segment];
        
        FlatStorageLocation location;
        location.segment_id = selected_segment;
        location.medium = descriptor.medium;
        location.offset = descriptor.used;
        location.size = size;
        location.transport_endpoint = descriptor.transport_endpoint;
        
        descriptor.used += size;
        replica_locations.push_back(location);
        used_segments.insert(selected_segment);
        ++total_allocations_;
    }
    
    return replica_locations;
}

ErrorCode FlatMemoryManager::Deallocate(const FlatStorageLocation& location) {
    std::unique_lock lock(mutex_);
    
    auto it = segments_.find(location.segment_id);
    if (it == segments_.end()) {
        LOG(WARNING) << "Segment not found for deallocation: " 
                     << location.segment_id;
        return ErrorCode::SEGMENT_NOT_FOUND;
    }
    
    auto& descriptor = it->second;
    
    // 简化的释放逻辑（实际实现可能需要更复杂的空间管理）
    if (descriptor.used >= location.size) {
        descriptor.used -= location.size;
    } else {
        LOG(ERROR) << "Invalid deallocation: used=" << descriptor.used
                   << ", requested=" << location.size;
        return ErrorCode::INTERNAL_ERROR;
    }
    
    ++total_deallocations_;
    
    return ErrorCode::OK;
}

size_t FlatMemoryManager::DeallocateBatch(
    const std::vector<FlatStorageLocation>& locations) {
    size_t success_count = 0;
    
    for (const auto& loc : locations) {
        if (Deallocate(loc) == ErrorCode::OK) {
            ++success_count;
        }
    }
    
    return success_count;
}

std::optional<FlatSegmentDescriptor> FlatMemoryManager::GetSegment(
    const std::string& segment_id) const {
    std::shared_lock lock(mutex_);
    
    auto it = segments_.find(segment_id);
    if (it != segments_.end()) {
        return it->second;
    }
    return std::nullopt;
}

std::vector<FlatSegmentDescriptor> FlatMemoryManager::GetAllSegments() const {
    std::shared_lock lock(mutex_);
    
    std::vector<FlatSegmentDescriptor> result;
    result.reserve(segments_.size());
    
    for (const auto& [id, desc] : segments_) {
        result.push_back(desc);
    }
    
    return result;
}

std::vector<FlatSegmentDescriptor> FlatMemoryManager::GetSegmentsByMedium(
    StorageMedium medium) const {
    std::shared_lock lock(mutex_);
    
    std::vector<FlatSegmentDescriptor> result;
    
    for (const auto& [id, desc] : segments_) {
        if (desc.medium == medium) {
            result.push_back(desc);
        }
    }
    
    return result;
}

FlatCapacityStats FlatMemoryManager::GetCapacityStats() const {
    std::shared_lock lock(mutex_);
    
    FlatCapacityStats stats;
    
    for (const auto& [id, desc] : segments_) {
        stats.total_capacity += desc.capacity;
        stats.total_used += desc.used;
        stats.capacity_by_medium[desc.medium] += desc.capacity;
        stats.used_by_medium[desc.medium] += desc.used;
    }
    
    return stats;
}

bool FlatMemoryManager::HasAvailableSpace(size_t required_size) const {
    std::shared_lock lock(mutex_);
    
    for (const auto& [id, desc] : segments_) {
        if (desc.availableSpace() >= required_size) {
            return true;
        }
    }
    
    return false;
}

bool FlatMemoryManager::HasAvailableSpaceInMedium(
    StorageMedium medium, size_t required_size) const {
    std::shared_lock lock(mutex_);
    
    for (const auto& [id, desc] : segments_) {
        if (desc.medium == medium && desc.availableSpace() >= required_size) {
            return true;
        }
    }
    
    return false;
}

void FlatMemoryManager::SetDefaultPlacementPolicy(PlacementPolicy policy) {
    config_.default_policy = policy;
    LOG(INFO) << "Updated default placement policy to: " << policy;
}

size_t FlatMemoryManager::GetSegmentCount() const {
    std::shared_lock lock(mutex_);
    return segments_.size();
}

tl::expected<std::string, ErrorCode> FlatMemoryManager::SelectSegment(
    size_t required_size,
    const FlatPlacementConfig& config) {
    
    std::shared_lock lock(mutex_);
    
    // 首先尝试首选段
    if (!config.preferred_segments.empty()) {
        auto result = SelectFromPreferredSegments(
            required_size, config.preferred_segments);
        if (result) {
            return result.value();
        }
    }
    
    // 然后尝试首选介质
    if (!config.preferred_mediums.empty()) {
        auto result = SelectFromPreferredMediums(
            required_size, config.preferred_mediums);
        if (result) {
            return result.value();
        }
    }
    
    // 如果不允许使用任意介质，则失败
    if (!config.allow_any_medium) {
        return tl::make_unexpected(ErrorCode::SEGMENT_NOT_FOUND);
    }
    
    // 根据策略选择（核心变化：不基于冷热）
    std::optional<std::string> selected;
    
    PlacementPolicy policy = config.policy;
    
    switch (policy) {
        case PlacementPolicy::CAPACITY_FIRST:
            selected = SelectByCapacity(required_size);
            break;
        case PlacementPolicy::LATENCY_FIRST:
            selected = SelectByLatency(required_size);
            break;
        case PlacementPolicy::ROUND_ROBIN:
            selected = SelectByRoundRobin(required_size);
            break;
        case PlacementPolicy::RANDOM:
            selected = SelectRandom(required_size);
            break;
        case PlacementPolicy::LOCALITY_AWARE:
            selected = SelectByLocality(required_size, config);
            break;
        default:
            selected = SelectByCapacity(required_size);
    }
    
    if (selected) {
        return selected.value();
    }
    
    return tl::make_unexpected(ErrorCode::NO_AVAILABLE_HANDLE);
}

std::optional<std::string> FlatMemoryManager::SelectByCapacity(
    size_t required_size) {
    // 选择可用容量最大的段（不考虑介质类型）
    std::string best_segment;
    size_t max_available = 0;
    
    for (const auto& [id, desc] : segments_) {
        size_t available = desc.availableSpace();
        if (available >= required_size && available > max_available) {
            max_available = available;
            best_segment = id;
        }
    }
    
    if (best_segment.empty()) {
        return std::nullopt;
    }
    return best_segment;
}

std::optional<std::string> FlatMemoryManager::SelectByLatency(
    size_t required_size) {
    // 选择延迟最低的段
    std::string best_segment;
    uint64_t min_latency = UINT64_MAX;
    
    for (const auto& [id, desc] : segments_) {
        if (desc.availableSpace() >= required_size && 
            desc.estimated_latency_ns < min_latency) {
            min_latency = desc.estimated_latency_ns;
            best_segment = id;
        }
    }
    
    if (best_segment.empty()) {
        return std::nullopt;
    }
    return best_segment;
}

std::optional<std::string> FlatMemoryManager::SelectByRoundRobin(
    size_t required_size) {
    auto available = GetAvailableSegments(required_size);
    
    if (available.empty()) {
        return std::nullopt;
    }
    
    size_t index = round_robin_index_.fetch_add(1) % available.size();
    return available[index];
}

std::optional<std::string> FlatMemoryManager::SelectRandom(
    size_t required_size) {
    auto available = GetAvailableSegments(required_size);
    
    if (available.empty()) {
        return std::nullopt;
    }
    
    static std::random_device rd;
    static std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, available.size() - 1);
    
    return available[dis(gen)];
}

std::optional<std::string> FlatMemoryManager::SelectByLocality(
    size_t required_size,
    const FlatPlacementConfig& config) {
    // 位置感知选择：暂时回退到容量优先
    // TODO: 实现基于NUMA拓扑的选择
    return SelectByCapacity(required_size);
}

std::optional<std::string> FlatMemoryManager::SelectFromPreferredMediums(
    size_t required_size,
    const std::vector<StorageMedium>& preferred_mediums) {
    
    for (const auto& medium : preferred_mediums) {
        for (const auto& [id, desc] : segments_) {
            if (desc.medium == medium && 
                desc.availableSpace() >= required_size) {
                return id;
            }
        }
    }
    
    return std::nullopt;
}

std::optional<std::string> FlatMemoryManager::SelectFromPreferredSegments(
    size_t required_size,
    const std::vector<std::string>& preferred_segments) {
    
    for (const auto& preferred : preferred_segments) {
        auto it = segments_.find(preferred);
        if (it != segments_.end() && 
            it->second.availableSpace() >= required_size) {
            return preferred;
        }
    }
    
    return std::nullopt;
}

std::vector<std::string> FlatMemoryManager::GetAvailableSegments(
    size_t required_size) const {
    std::vector<std::string> available;
    
    for (const auto& [id, desc] : segments_) {
        if (desc.availableSpace() >= required_size) {
            available.push_back(id);
        }
    }
    
    return available;
}

}  // namespace flat
}  // namespace mooncake
