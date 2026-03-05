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
 * @file flat_memory_usage_example.cpp
 * @brief Flat Memory System 使用示例
 * 
 * 展示如何使用 KVCache Flat Memory System
 */

#include "flat_memory_client.h"
#include <iostream>
#include <vector>
#include <random>
#include <chrono>

using namespace mooncake::flat;

/**
 * @brief 示例 1: 基本使用
 */
void BasicUsageExample() {
    std::cout << "\n=== Example 1: Basic Usage ===" << std::endl;
    
    // 1. 配置集群
    FlatClusterConfig config;
    
    // 本地节点配置
    config.local_node.node_id = "node0";
    config.local_node.address = "192.168.1.100";
    config.local_node.port = 12345;
    
    // 启用本地 DRAM（8GB）
    config.local_node.local_dram.enabled = true;
    config.local_node.local_dram.capacity = 8ULL * 1024 * 1024 * 1024;
    
    // 启用本地 SSD（100GB）
    config.local_node.local_ssd.enabled = true;
    config.local_node.local_ssd.path = "/mnt/nvme0/flat_memory";
    config.local_node.local_ssd.capacity = 100ULL * 1024 * 1024 * 1024;
    
    // 添加远程节点
    RemoteNodeConfig remote;
    remote.node_id = "node1";
    remote.address = "192.168.1.101";
    remote.rdma_port = 12345;
    remote.dram_capacity = 8ULL * 1024 * 1024 * 1024;
    remote.has_ssd = true;
    remote.ssd_path = "/mnt/3fs/kvcache";
    remote.ssd_capacity = 100ULL * 1024 * 1024 * 1024;
    remote.ssd_type = RemoteStorageType::DFS_3FS;
    
    config.remote_nodes.push_back(remote);
    
    // 默认策略：延迟优先
    config.default_policy = UnifiedPlacementPolicy::LATENCY_FIRST;
    
    // 2. 创建客户端
    auto client = FlatMemoryClient::Create(config);
    if (!client) {
        std::cerr << "Failed to create client" << std::endl;
        return;
    }
    
    // 3. 存储 KVCache
    std::vector<float> kv_data(1024 * 1024);  // 4MB
    std::fill(kv_data.begin(), kv_data.end(), 1.0f);
    
    std::string key = "request_abc123/layer_0";
    int ret = client->Put(key, kv_data.data(), kv_data.size() * sizeof(float));
    
    if (ret == 0) {
        std::cout << "Stored KVCache: " << key << std::endl;
    }
    
    // 4. 读取 KVCache
    std::vector<float> read_buffer(1024 * 1024);
    ssize_t bytes = client->Get(key, read_buffer.data(), 
                                 read_buffer.size() * sizeof(float));
    
    if (bytes > 0) {
        std::cout << "Read " << bytes << " bytes from " << key << std::endl;
    }
    
    // 5. 查看统计
    client->PrintStats();
}

/**
 * @brief 示例 2: 多层 KVCache 存储
 * 
 * 典型 LLM 推理场景：存储多层 Transformer 的 KVCache
 */
void MultiLayerKVCacheExample() {
    std::cout << "\n=== Example 2: Multi-Layer KVCache ===" << std::endl;
    
    // 配置（简化版）
    FlatClusterConfig config;
    config.local_node.local_dram.enabled = true;
    config.local_node.local_dram.capacity = 16ULL * 1024 * 1024 * 1024;
    
    auto client = FlatMemoryClient::Create(config);
    if (!client) return;
    
    // 模拟 32 层 Transformer
    const int num_layers = 32;
    const size_t kv_size_per_layer = 4 * 1024 * 1024;  // 4MB per layer
    
    std::string request_id = "req_" + std::to_string(std::rand());
    
    // 存储所有层的 KVCache
    std::vector<float> layer_data(kv_size_per_layer / sizeof(float));
    
    auto start = std::chrono::high_resolution_clock::now();
    
    for (int layer = 0; layer < num_layers; ++layer) {
        std::string key = request_id + "/layer_" + std::to_string(layer);
        
        // 填充数据（实际场景中这是注意力计算的输出）
        std::fill(layer_data.begin(), layer_data.end(), 
                  static_cast<float>(layer));
        
        client->Put(key, layer_data.data(), kv_size_per_layer);
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    
    std::cout << "Stored " << num_layers << " layers in " 
              << duration.count() << " ms" << std::endl;
    std::cout << "Total size: " << (num_layers * kv_size_per_layer) / (1024*1024) 
              << " MB" << std::endl;
    
    // 读取特定层
    std::string target_key = request_id + "/layer_15";
    std::vector<float> read_buffer(kv_size_per_layer / sizeof(float));
    
    client->Get(target_key, read_buffer.data(), kv_size_per_layer);
    std::cout << "Read layer 15, first value: " << read_buffer[0] << std::endl;
    
    client->PrintStats();
}

/**
 * @brief 示例 3: 显式介质放置
 * 
 * 用户知道某些数据更重要，显式指定放置位置
 */
void ExplicitPlacementExample() {
    std::cout << "\n=== Example 3: Explicit Placement ===" << std::endl;
    
    FlatClusterConfig config;
    
    // 启用 GPU HBM
    config.local_node.hbm.enabled = true;
    config.local_node.hbm.device_id = 0;
    config.local_node.hbm.capacity = 4ULL * 1024 * 1024 * 1024;  // 4GB
    
    // 启用本地 DRAM
    config.local_node.local_dram.enabled = true;
    config.local_node.local_dram.capacity = 32ULL * 1024 * 1024 * 1024;
    
    auto client = FlatMemoryClient::Create(config);
    if (!client) return;
    
    std::vector<float> data(1024 * 1024);
    
    // 热门请求放 HBM
    std::string hot_key = "popular_request/layer_0";
    
    // 首先按默认策略存储（可能去了 DRAM）
    client->Put(hot_key, data.data(), data.size() * sizeof(float));
    
    // 显式迁移到 HBM
    int ret = client->MigrateTo(hot_key, UnifiedMediumType::GPU_HBM);
    if (ret == 0) {
        std::cout << "Migrated " << hot_key << " to HBM" << std::endl;
    }
    
    // 冷门请求保持在 DRAM
    std::string cold_key = "rare_request/layer_0";
    client->Put(cold_key, data.data(), data.size() * sizeof(float), 
                UnifiedPlacementPolicy::CAPACITY_FIRST);  // 容量优先
    
    // 查看分布
    auto hbm_keys = client->GetKeysByMedium(UnifiedMediumType::GPU_HBM);
    auto dram_keys = client->GetKeysByMedium(UnifiedMediumType::LOCAL_DRAM);
    
    std::cout << "HBM keys: " << hbm_keys.size() << std::endl;
    std::cout << "DRAM keys: " << dram_keys.size() << std::endl;
    
    client->PrintStats();
}

/**
 * @brief 示例 4: Prefetch 场景
 * 
 * 预测下一步需要的数据，提前加载到高速存储
 */
void PrefetchExample() {
    std::cout << "\n=== Example 4: Prefetch ===" << std::endl;
    
    FlatClusterConfig config;
    
    // 本地快速存储
    config.local_node.local_dram.enabled = true;
    config.local_node.local_dram.capacity = 8ULL * 1024 * 1024 * 1024;
    
    // 远程大容量存储
    RemoteNodeConfig remote;
    remote.node_id = "storage_node";
    remote.address = "192.168.1.200";
    remote.has_ssd = true;
    remote.ssd_path = "/mnt/3fs/kvcache_archive";
    remote.ssd_capacity = 1ULL * 1024 * 1024 * 1024 * 1024;  // 1TB
    remote.ssd_type = RemoteStorageType::DFS_3FS;
    config.remote_nodes.push_back(remote);
    
    auto client = FlatMemoryClient::Create(config);
    if (!client) return;
    
    // 场景：系统预测用户可能会继续某个对话
    std::string predicted_request = "conversation_xyz/layer_0";
    
    // 假设数据当前在远程 SSD
    // 预取到本地 DRAM
    int ret = client->Prefetch(predicted_request, UnifiedMediumType::LOCAL_DRAM);
    
    if (ret == 0) {
        std::cout << "Prefetched " << predicted_request 
                  << " to LOCAL_DRAM" << std::endl;
    }
    
    // 用户果然继续了，现在访问很快
    std::vector<float> buffer(1024 * 1024);
    
    auto start = std::chrono::high_resolution_clock::now();
    client->Get(predicted_request, buffer.data(), buffer.size() * sizeof(float));
    auto end = std::chrono::high_resolution_clock::now();
    
    auto duration_us = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    std::cout << "Access latency after prefetch: " << duration_us.count() 
              << " us" << std::endl;
}

/**
 * @brief 示例 5: 从配置文件创建
 */
void ConfigFileExample() {
    std::cout << "\n=== Example 5: Config File ===" << std::endl;
    
    // 配置文件示例内容：
    // {
    //   "local_node": {
    //     "node_id": "inference_node_0",
    //     "address": "192.168.1.100",
    //     "port": 12345,
    //     "hbm": {
    //       "enabled": true,
    //       "device_id": 0,
    //       "capacity": 4294967296
    //     },
    //     "local_dram": {
    //       "enabled": true,
    //       "capacity": 34359738368
    //     },
    //     "local_ssd": {
    //       "enabled": true,
    //       "path": "/mnt/nvme0/kvcache",
    //       "capacity": 107374182400
    //     }
    //   },
    //   "remote_nodes": [
    //     {
    //       "node_id": "storage_node_0",
    //       "address": "192.168.1.200",
    //       "rdma_port": 12345,
    //       "dram_capacity": 68719476736,
    //       "has_ssd": true,
    //       "ssd_path": "/mnt/3fs/kvcache",
    //       "ssd_capacity": 1099511627776,
    //       "ssd_type": "3fs"
    //     }
    //   ],
    //   "default_policy": "latency_first"
    // }
    
    auto client = FlatMemoryClient::Create("/etc/mooncake/flat_memory.json");
    if (!client) {
        std::cout << "Config file not found, skipping example" << std::endl;
        return;
    }
    
    client->PrintStats();
}

/**
 * @brief 示例 6: 异步操作
 */
void AsyncExample() {
    std::cout << "\n=== Example 6: Async Operations ===" << std::endl;
    
    FlatClusterConfig config;
    config.local_node.local_dram.enabled = true;
    config.local_node.local_dram.capacity = 8ULL * 1024 * 1024 * 1024;
    
    auto client = FlatMemoryClient::Create(config);
    if (!client) return;
    
    // 存储数据
    std::vector<float> data(1024 * 1024);
    std::string key = "async_test/layer_0";
    client->Put(key, data.data(), data.size() * sizeof(float));
    
    // 异步读取
    std::vector<float> buffer(1024 * 1024);
    std::atomic<bool> done{false};
    
    client->AsyncGet(key, buffer.data(), buffer.size() * sizeof(float),
        [&done](int status, size_t bytes) {
            std::cout << "Async read completed: status=" << status 
                      << ", bytes=" << bytes << std::endl;
            done = true;
        });
    
    // 做其他事情...
    std::cout << "Doing other work while waiting..." << std::endl;
    
    // 等待完成
    while (!done) {
        std::this_thread::yield();
    }
    
    std::cout << "All done!" << std::endl;
}

/**
 * @brief 主函数
 */
int main(int argc, char* argv[]) {
    std::cout << "======================================" << std::endl;
    std::cout << "  KVCache Flat Memory System Examples" << std::endl;
    std::cout << "======================================" << std::endl;
    
    // 注意：这些示例需要实际的硬件配置才能运行
    // 在没有配置的环境中会创建失败
    
    try {
        BasicUsageExample();
        MultiLayerKVCacheExample();
        ExplicitPlacementExample();
        PrefetchExample();
        ConfigFileExample();
        AsyncExample();
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}
