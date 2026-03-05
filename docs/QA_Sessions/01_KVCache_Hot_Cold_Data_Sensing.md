# Mooncake KVCache 冷热数据感知机制详解

## 问题

> 你能帮我详细讲解Mooncake的实现逻辑，特别是怎么感知kvcache的冷热

---

## 回答

### 一、整体架构

根据代码分析，Mooncake 的冷热数据感知是一个**多层次、多机制协同**的系统：

```
┌─────────────────────────────────────────────────────────────────────┐
│                     KVCache 冷热感知系统                              │
├─────────────────────────────────────────────────────────────────────┤
│  Soft Pin 机制    │   Lease 机制     │   Eviction 策略    │  水位触发  │
│  (热数据保护)      │  (读写保护)       │   (LRU/FIFO)      │  (空间管理)│
└─────────────────────────────────────────────────────────────────────┘
                                    │
                                    ▼
         ┌─────────────────────────────────────────────────┐
         │            Master Service (控制面)               │
         │  • 元数据管理 (ObjectMetadata)                    │
         │  • Eviction 线程 (后台周期性检查)                  │
         │  • 空间水位监控                                   │
         └─────────────────────────────────────────────────┘
```

### 二、核心感知机制

#### 2.1 Soft Pin 机制（热数据标记）

**位置**: `master_service.h`

```cpp
struct ObjectMetadata {
    // 软固定超时时间 - 可选字段，只有 VIP 对象才会设置
    mutable std::optional<std::chrono::steady_clock::time_point>
        soft_pin_timeout;  // optional soft pin, only set for vip objects
    
    // 检查对象是否处于软固定状态
    bool IsSoftPinned(std::chrono::steady_clock::time_point& now) const {
        return soft_pin_timeout && now < *soft_pin_timeout;
    }
};
```

**工作原理**:
1. **Put 时标记**: 当存储对象时，可通过 `ReplicateConfig::with_soft_pin` 标记为热数据
2. **TTL 机制**: Soft Pin 状态有 TTL（默认 30 分钟），超时后自动解除
3. **访问刷新**: 每次 `ExistKey` 或 `GetReplicaList` 成功时，会调用 `GrantLease()` 刷新 soft_pin_timeout

**关键参数**:
```cpp
// types.h
static constexpr uint64_t DEFAULT_KV_SOFT_PIN_TTL_MS = 30 * 60 * 1000;  // 30分钟
static constexpr bool DEFAULT_ALLOW_EVICT_SOFT_PINNED_OBJECTS = true;
```

#### 2.2 Lease 机制（读写保护）

**位置**: `master_service.cpp`

```cpp
auto MasterService::ExistKey(const std::string& key)
    -> tl::expected<bool, ErrorCode> {
    // ...
    if (metadata.HasReplica(&Replica::fn_is_completed)) {
        // 授予租约，保护对象不被驱逐
        metadata.GrantLease(default_kv_lease_ttl_, default_kv_soft_pin_ttl_);
        return true;
    }
    return false;
}
```

**Lease 授予逻辑**:
```cpp
void GrantLease(const uint64_t ttl, const uint64_t soft_ttl) const {
    std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    // 硬租约：只会延长，不会缩短
    lease_timeout = std::max(lease_timeout, now + std::chrono::milliseconds(ttl));
    // 软固定：如果启用了soft pin，也刷新
    if (soft_pin_timeout) {
        soft_pin_timeout = std::max(*soft_pin_timeout,
                                    now + std::chrono::milliseconds(soft_ttl));
    }
}
```

**默认 TTL**:
```cpp
static constexpr uint64_t DEFAULT_DEFAULT_KV_LEASE_TTL = 5000;  // 5秒
```

#### 2.3 Eviction 策略

**位置**: `eviction_strategy.h`

Mooncake 实现了两种驱逐策略：

##### LRU (最近最少使用)
```cpp
class LRUEvictionStrategy : public EvictionStrategy {
    virtual ErrorCode AddKey(const std::string& key) override {
        // 添加到列表头部
        all_key_list_.push_front(key);
        all_key_idx_map_[key] = all_key_list_.begin();
        return ErrorCode::OK;
    }

    virtual ErrorCode UpdateKey(const std::string& key) override {
        // 访问时移动到列表头部（最热）
        all_key_list_.erase(it->second);
        all_key_list_.push_front(key);
        all_key_idx_map_[key] = all_key_list_.begin();
        return ErrorCode::OK;
    }

    virtual std::string EvictKey(void) override {
        // 驱逐列表尾部（最冷）
        std::string evicted_key = all_key_list_.back();
        all_key_list_.pop_back();
        return evicted_key;
    }
};
```

##### FIFO (先进先出)
```cpp
class FIFOEvictionStrategy : public EvictionStrategy {
    // 简单队列，不根据访问更新位置
    virtual ErrorCode UpdateKey(const std::string& key) override {
        return ErrorCode::OK;  // FIFO 不更新位置
    }
};
```

#### 2.4 水位触发机制

**位置**: `master_service.cpp`

```cpp
void MasterService::EvictionThreadFunc() {
    while (eviction_running_) {
        double used_ratio = MasterMetricManager::instance().get_global_mem_used_ratio();
        
        // 触发条件：使用率 > 高水位阈值 (默认95%)
        if (used_ratio > eviction_high_watermark_ratio_ ||
            (need_eviction_ && eviction_ratio_ > 0.0)) {
            
            // 计算目标驱逐比例
            double evict_ratio_target = std::max(
                eviction_ratio_,  // 默认5%
                used_ratio - eviction_high_watermark_ratio_ + eviction_ratio_);
            
            BatchEvict(evict_ratio_target, evict_ratio_lowerbound);
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(kEvictionThreadSleepMs));
    }
}
```

**关键参数**:
```cpp
static constexpr double DEFAULT_EVICTION_RATIO = 0.05;                    // 5%
static constexpr double DEFAULT_EVICTION_HIGH_WATERMARK_RATIO = 0.95;     // 95%
```

### 三、BatchEvict 驱逐流程（冷热感知核心）

**位置**: `master_service.cpp`

这是 Mooncake 冷热感知最核心的实现：

```cpp
void MasterService::BatchEvict(double evict_ratio_target, double evict_ratio_lowerbound) {
    // 候选分类
    std::vector<time_point> no_pin_objects;    // 非软固定对象
    std::vector<time_point> soft_pin_objects;  // 软固定对象
    
    // === 第一遍：只驱逐非软固定且租约过期的对象 ===
    for (auto& metadata : shard->metadata) {
        // 跳过租约未过期的对象
        if (!metadata.IsLeaseExpired(now) || !can_evict_replicas(metadata)) {
            continue;
        }
        
        if (!metadata.IsSoftPinned(now)) {
            // 非软固定 -> 第一优先级驱逐
            candidates.push_back(metadata.lease_timeout);
        } else if (allow_evict_soft_pinned_objects_) {
            // 软固定 -> 第二优先级驱逐候选
            soft_pin_objects.push_back(metadata.lease_timeout);
        }
    }
    
    // 按 lease_timeout 排序，驱逐最老的（最冷的）
    std::nth_element(candidates.begin(), candidates.begin() + evict_num, candidates.end());
    
    // === 第二遍：如果空间仍不足，驱逐软固定对象 ===
    if (target_evict_num > no_pin_objects.size() && !soft_pin_objects.empty()) {
        // 优先驱逐 lease_timeout 更早的软固定对象
        std::nth_element(soft_pin_objects.begin(), 
                        soft_pin_objects.begin() + soft_pin_evict_num,
                        soft_pin_objects.end());
    }
}
```

### 冷热判定逻辑总结

```
┌─────────────────────────────────────────────────────────────────────┐
│                         驱逐优先级（从高到低）                          │
├─────────────────────────────────────────────────────────────────────┤
│  Level 1: 租约过期 + 非软固定 + 引用计数为0 → 最容易被驱逐（最冷）          │
│  Level 2: 租约过期 + 软固定 + 引用计数为0 → 次优先级                     │
│  Level 3: 租约未过期 或 引用计数>0 → 受保护，不能驱逐                     │
└─────────────────────────────────────────────────────────────────────┘

热度判定因素：
1. Soft Pin 状态：with_soft_pin=true 的对象更"热"
2. 访问频率：每次 Get/Exist 会刷新 lease_timeout 和 soft_pin_timeout
3. Lease 有效期：lease_timeout 越新，越不容易被驱逐
4. 引用计数：正在被读写的对象（refcnt > 0）绝对不会被驱逐
```

### 四、数据流与访问路径

#### 4.1 Put 操作（写入时设置冷热属性）

```cpp
// replica.h 中的配置
struct ReplicateConfig {
    size_t replica_num{1};          // 副本数
    bool with_soft_pin{false};      // 是否启用软固定（热数据标记）
    std::string preferred_segment;  // 首选存储段
};

// 创建 ObjectMetadata 时
ObjectMetadata(client_id, put_start_time, value_length, replicas, enable_soft_pin) {
    if (enable_soft_pin) {
        soft_pin_timeout.emplace();  // 启用软固定
        MasterMetricManager::instance().inc_soft_pin_key_count(1);
    }
}
```

#### 4.2 Get 操作（访问时刷新热度）

```cpp
auto MasterService::GetReplicaList(const std::string& key, size_t version)
    -> tl::expected<GetReplicaListResponse, ErrorCode> {
    // ...
    // 授予租约，标记为"最近访问"
    metadata.GrantLease(default_kv_lease_ttl_, default_kv_soft_pin_ttl_);
    // ...
}
```

### 五、关键配置参数汇总

| 参数名 | 默认值 | 说明 |
|--------|--------|------|
| `eviction_high_watermark_ratio` | 0.95 | 触发驱逐的空间使用率阈值 |
| `eviction_ratio` | 0.05 | 每次驱逐的目标比例 |
| `default_kv_lease_ttl` | 5000ms | 默认租约 TTL |
| `default_kv_soft_pin_ttl` | 30分钟 | 软固定 TTL |
| `allow_evict_soft_pinned_objects` | true | 是否允许驱逐软固定对象 |

### 六、设计亮点

1. **渐进式驱逐**: 先驱逐非软固定对象，再驱逐软固定对象
2. **时间衰减**: Soft Pin 状态会随时间自动失效，避免内存永久占用
3. **访问刷新**: 每次访问都会刷新热度，实现自适应的冷热感知
4. **引用保护**: 正在使用的对象通过引用计数保护，绝不会被意外驱逐
5. **近似 LRU**: 使用 `std::nth_element` 实现 O(n) 的近似 LRU，避免全局排序开销

这套机制使得 Mooncake 能够自动识别热数据（如系统提示、频繁访问的 KVCache）并优先保留，同时在内存压力大时能够合理释放冷数据。
