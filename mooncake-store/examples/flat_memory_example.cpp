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
 * @file flat_memory_example.cpp
 * @brief KVCache Flat Memory System 使用示例
 * 
 * 本示例展示如何使用扁平化内存管理器替代原有的分层存储。
 */

#include <iostream>
#include <vector>
#include <string>
#include <cassert>

#include "flat_memory_manager.h"
#include "flat_memory_types.h"

using namespace mooncake::flat;

/**
 * @brief 创建模拟的存储段
 */
std::vector<FlatSegmentDescriptor> CreateMockSegments() {
    std::vector<FlatSegmentDescriptor> segments;
    
    // GPU HBM段
    FlatSegmentDescriptor hbm_seg;
    hbm_seg.segment_id = "gpu_hbm_0";
    hbm_seg.medium = StorageMedium::GPU_HBM;
    hbm_seg.base_address = 0x100000000;
    hbm_seg.capacity = 16ULL * 1024 * 1024 * 1024;  // 16GB
    hbm_seg.used = 0;
    hbm_seg.transport_endpoint = "192.168.1.1:12345";
    hbm_seg.estimated_latency_ns = 10;  // 10ns
    hbm_seg.bandwidth_gbps = 900;       // 900 GB/s
    segments.push_back(hbm_seg);
    
    // System DRAM段
    FlatSegmentDescriptor dram_seg;
    dram_seg.segment_id = "dram_0";
    dram_seg.medium = StorageMedium::SYSTEM_DRAM;
    dram_seg.base_address = 0x200000000;
    dram_seg.capacity = 128ULL * 1024 * 1024 * 1024;  // 128GB
    dram_seg.used = 0;
    dram_seg.transport_endpoint = "192.168.1.1:12346";
    dram_seg.estimated_latency_ns = 100;  // 100ns
    dram_seg.bandwidth_gbps = 100;        // 100 GB/s
    segments.push_back(dram_seg);
    
    // Local SSD段
    FlatSegmentDescriptor ssd_seg;
    ssd_seg.segment_id = "ssd_0";
    ssd_seg.medium = StorageMedium::LOCAL_SSD;
    ssd_seg.base_address = 0x300000000;
    ssd_seg.capacity = 1024ULL * 1024 * 1024 * 1024;  // 1TB
    ssd_seg.used = 0;
    ssd_seg.transport_endpoint = "192.168.1.1:12347";
    ssd_seg.estimated_latency_ns = 10000;  // 10us
    ssd_seg.bandwidth_gbps = 7;            // 7 GB/s
    segments.push_back(ssd_seg);
    
    return segments;
}

/**
 * @brief 示例1: 基础用法 - 容量优先分配
 */
void Example_BasicUsage_CapacityFirst() {
    std::cout << "\n=== Example 1: Basic Usage - Capacity First ===\n";
    
    // 创建扁平内存管理器
    FlatMemoryConfig config;
    config.enabled = true;
    config.default_policy = PlacementPolicy::CAPACITY_FIRST;
    config.disable_auto_migration = true;  // 核心：禁用自动迁移
    config.disable_eviction = true;        // 核心：禁用驱逐
    
    auto manager = FlatMemoryManagerBuilder()
        .WithConfig(config)
        .Build();
    
    // 注册存储段
    for (const auto& seg : CreateMockSegments()) {
        manager->RegisterSegment(seg);
    }
    
    // 分配KVCache空间（不指定介质类型）
    FlatPlacementConfig placement_config;
    placement_config.policy = PlacementPolicy::CAPACITY_FIRST;
    placement_config.allow_any_medium = true;  // 允许任意介质
    
    auto result = manager->Allocate(1024 * 1024 * 100, placement_config);  // 100MB
    
    if (result) {
        auto& loc = result.value();
        std::cout << "Allocated 100MB at:\n";
        std::cout << "  Segment: " << loc.segment_id << "\n";
        std::cout << "  Medium: " << loc.medium << "\n";
        std::cout << "  Offset: " << loc.offset << "\n";
        
        // 容量优先应该选择SSD（1TB最大）
        assert(loc.medium == StorageMedium::LOCAL_SSD);
    }
    
    // 打印容量统计
    auto stats = manager->GetCapacityStats();
    std::cout << "\nCapacity Stats:\n";
    std::cout << "  Total: " << stats.total_used << " / " << stats.total_capacity << "\n";
}

/**
 * @brief 示例2: 延迟优先分配（适合TTFT敏感场景）
 */
void Example_LatencyFirst() {
    std::cout << "\n=== Example 2: Latency First (TTFT Sensitive) ===\n";
    
    FlatMemoryConfig config;
    config.enabled = true;
    config.default_policy = PlacementPolicy::LATENCY_FIRST;
    config.disable_auto_migration = true;
    config.disable_eviction = true;
    
    auto manager = std::make_shared<FlatMemoryManager>(config);
    
    for (const auto& seg : CreateMockSegments()) {
        manager->RegisterSegment(seg);
    }
    
    // 延迟优先分配
    FlatPlacementConfig placement_config;
    placement_config.policy = PlacementPolicy::LATENCY_FIRST;
    
    auto result = manager->Allocate(1024 * 1024 * 100, placement_config);
    
    if (result) {
        auto& loc = result.value();
        std::cout << "Allocated 100MB at:\n";
        std::cout << "  Segment: " << loc.segment_id << "\n";
        std::cout << "  Medium: " << loc.medium << "\n";
        std::cout << "  Latency will be optimal for TTFT\n";
        
        // 延迟优先应该选择GPU HBM
        assert(loc.medium == StorageMedium::GPU_HBM);
    }
}

/**
 * @brief 示例3: 首选介质（混合策略）
 */
void Example_PreferredMediums() {
    std::cout << "\n=== Example 3: Preferred Mediums ===\n";
    
    FlatMemoryConfig config;
    config.enabled = true;
    config.disable_auto_migration = true;
    config.disable_eviction = true;
    
    auto manager = std::make_shared<FlatMemoryManager>(config);
    
    for (const auto& seg : CreateMockSegments()) {
        manager->RegisterSegment(seg);
    }
    
    // 指定首选介质（DRAM优先，然后SSD）
    FlatPlacementConfig placement_config;
    placement_config.preferred_mediums = {
        StorageMedium::SYSTEM_DRAM,
        StorageMedium::LOCAL_SSD
    };
    placement_config.allow_any_medium = true;
    
    auto result = manager->Allocate(1024 * 1024 * 100, placement_config);
    
    if (result) {
        auto& loc = result.value();
        std::cout << "Allocated 100MB at:\n";
        std::cout << "  Segment: " << loc.segment_id << "\n";
        std::cout << "  Medium: " << loc.medium << "\n";
        
        // 应该选择DRAM（首选）
        assert(loc.medium == StorageMedium::SYSTEM_DRAM);
    }
}

/**
 * @brief 示例4: 多副本分配
 */
void Example_ReplicaAllocation() {
    std::cout << "\n=== Example 4: Replica Allocation ===\n";
    
    FlatMemoryConfig config;
    config.enabled = true;
    config.disable_auto_migration = true;
    config.disable_eviction = true;
    
    auto manager = std::make_shared<FlatMemoryManager>(config);
    
    for (const auto& seg : CreateMockSegments()) {
        manager->RegisterSegment(seg);
    }
    
    // 分配3个副本
    FlatPlacementConfig placement_config;
    placement_config.replica_num = 3;
    
    auto result = manager->AllocateReplicas(1024 * 1024 * 10, 3, placement_config);
    
    if (result) {
        std::cout << "Allocated 3 replicas of 10MB:\n";
        for (size_t i = 0; i < result.value().size(); ++i) {
            auto& loc = result.value()[i];
            std::cout << "  Replica " << i << ": " 
                      << loc.segment_id << " (" << loc.medium << ")\n";
        }
        
        // 副本应该分布在不同的段
        assert(result.value().size() == 3);
    }
}

/**
 * @brief 示例5: 对比原始分层架构 vs 扁平化架构
 */
void Example_CompareArchitectures() {
    std::cout << "\n=== Example 5: Architecture Comparison ===\n";
    
    std::cout << "\n--- 原始分层架构（Mooncake默认）---\n";
    std::cout << "特点:\n";
    std::cout << "  - 热数据 -> GPU HBM\n";
    std::cout << "  - 温数据 -> System DRAM\n";
    std::cout << "  - 冷数据 -> SSD (offload)\n";
    std::cout << "  - 自动驱逐和迁移\n";
    std::cout << "  - TTFT优化：热数据访问快\n";
    
    std::cout << "\n--- 扁平化架构 ---\n";
    std::cout << "特点:\n";
    std::cout << "  - 所有存储等价对待\n";
    std::cout << "  - 基于放置策略分配，非冷热\n";
    std::cout << "  - 无自动迁移和驱逐\n";
    std::cout << "  - TTFT影响：可能增加（取决于数据位置）\n";
    std::cout << "  - 优势：简化管理、最大化存储利用\n";
    
    // 创建扁平化管理器
    FlatMemoryConfig flat_config;
    flat_config.enabled = true;
    flat_config.disable_auto_migration = true;
    flat_config.disable_eviction = true;
    
    auto flat_manager = std::make_shared<FlatMemoryManager>(flat_config);
    
    for (const auto& seg : CreateMockSegments()) {
        flat_manager->RegisterSegment(seg);
    }
    
    // 模拟分配场景
    std::cout << "\n模拟100个KVCache块分配:\n";
    
    size_t hbm_count = 0, dram_count = 0, ssd_count = 0;
    
    FlatPlacementConfig config;
    config.policy = PlacementPolicy::ROUND_ROBIN;  // 轮询分配
    
    for (int i = 0; i < 100; ++i) {
        auto result = flat_manager->Allocate(1024 * 1024, config);
        if (result) {
            switch (result.value().medium) {
                case StorageMedium::GPU_HBM: ++hbm_count; break;
                case StorageMedium::SYSTEM_DRAM: ++dram_count; break;
                case StorageMedium::LOCAL_SSD: ++ssd_count; break;
                default: break;
            }
        }
    }
    
    std::cout << "分配结果（轮询策略）:\n";
    std::cout << "  GPU HBM: " << hbm_count << " 块\n";
    std::cout << "  DRAM: " << dram_count << " 块\n";
    std::cout << "  SSD: " << ssd_count << " 块\n";
    
    std::cout << "\n在分层架构中，所有100块可能都在HBM（如果足够）。\n";
    std::cout << "在扁平化架构中（轮询），块均匀分布在所有介质。\n";
}

/**
 * @brief 示例6: TTFT缓解策略
 */
void Example_TTFTMitigation() {
    std::cout << "\n=== Example 6: TTFT Mitigation Strategies ===\n";
    
    FlatMemoryConfig config;
    config.enabled = true;
    config.disable_auto_migration = true;
    config.disable_eviction = true;
    
    auto manager = std::make_shared<FlatMemoryManager>(config);
    
    for (const auto& seg : CreateMockSegments()) {
        manager->RegisterSegment(seg);
    }
    
    std::cout << "TTFT缓解策略:\n\n";
    
    // 策略1: Prefill阶段使用延迟优先
    std::cout << "1. Prefill阶段使用LATENCY_FIRST策略:\n";
    FlatPlacementConfig prefill_config;
    prefill_config.policy = PlacementPolicy::LATENCY_FIRST;
    
    auto prefill_result = manager->Allocate(1024 * 1024 * 50, prefill_config);
    if (prefill_result) {
        std::cout << "   Prefill数据分配在: " << prefill_result.value().medium << "\n";
        std::cout << "   预期延迟: ~10ns (GPU HBM)\n";
    }
    
    // 策略2: Decode阶段可以使用容量优先
    std::cout << "\n2. Decode阶段使用CAPACITY_FIRST策略:\n";
    FlatPlacementConfig decode_config;
    decode_config.policy = PlacementPolicy::CAPACITY_FIRST;
    
    auto decode_result = manager->Allocate(1024 * 1024 * 500, decode_config);
    if (decode_result) {
        std::cout << "   Decode数据分配在: " << decode_result.value().medium << "\n";
        std::cout << "   最大化存储利用率\n";
    }
    
    // 策略3: 混合策略
    std::cout << "\n3. 混合策略（首选HBM，回退到DRAM）:\n";
    FlatPlacementConfig hybrid_config;
    hybrid_config.preferred_mediums = {
        StorageMedium::GPU_HBM,
        StorageMedium::SYSTEM_DRAM
    };
    hybrid_config.policy = PlacementPolicy::LATENCY_FIRST;
    
    auto hybrid_result = manager->Allocate(1024 * 1024 * 100, hybrid_config);
    if (hybrid_result) {
        std::cout << "   混合策略分配在: " << hybrid_result.value().medium << "\n";
    }
}

int main() {
    std::cout << "=================================================\n";
    std::cout << "   KVCache Flat Memory System Examples\n";
    std::cout << "=================================================\n";
    
    Example_BasicUsage_CapacityFirst();
    Example_LatencyFirst();
    Example_PreferredMediums();
    Example_ReplicaAllocation();
    Example_CompareArchitectures();
    Example_TTFTMitigation();
    
    std::cout << "\n=================================================\n";
    std::cout << "   All examples completed successfully!\n";
    std::cout << "=================================================\n";
    
    return 0;
}
