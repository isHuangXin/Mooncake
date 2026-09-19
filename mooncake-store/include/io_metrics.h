#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace mooncake {

// Non-resetting, per-instance counters, independent of StorageIOMetric and its
// legacy native-window operation/latency denominators.
std::string NewIoMetricsInstanceId();

struct SsdToHostFetchSnapshot {
    uint64_t bytes = 0;
    uint64_t latency_ns_sum = 0;
    uint64_t batches = 0;
    uint64_t errors = 0;
    uint64_t inflight = 0;
};

struct ClientIoStatsSnapshot {
    uint32_t schema_version = 1;
    std::string instance_id;
    std::vector<std::string> capabilities;
    std::optional<SsdToHostFetchSnapshot> ssd_to_host_fetch;
};

class SsdToHostFetchStats {
   public:
    using Clock = std::chrono::steady_clock;

    ClientIoStatsSnapshot Snapshot(bool available) const {
        ClientIoStatsSnapshot snapshot;
        snapshot.instance_id = instance_id_;
        std::lock_guard<std::mutex> lock(mutex_);
        if (available && classification_complete_) {
            snapshot.capabilities.emplace_back("ssd_to_host_fetch_v1");
            snapshot.ssd_to_host_fetch = totals_;
        }
        return snapshot;
    }

    void MarkUnclassifiable() {
        std::lock_guard<std::mutex> lock(mutex_);
        classification_complete_ = false;
    }

    // A nonempty, all-Host owner sub-batch is one attempt. Validation, RPC,
    // transfer and TTL failures count only as errors. No lock spans I/O.
    class Attempt {
       public:
        Attempt(SsdToHostFetchStats& stats, bool eligible)
            : stats_(eligible ? &stats : nullptr) {
            if (stats_) {
                std::lock_guard<std::mutex> lock(stats_->mutex_);
                ++stats_->totals_.inflight;
            }
        }
        Attempt(const Attempt&) = delete;
        Attempt& operator=(const Attempt&) = delete;
        ~Attempt() {
            if (stats_) {
                std::lock_guard<std::mutex> lock(stats_->mutex_);
                --stats_->totals_.inflight;
                ++stats_->totals_.errors;
            }
        }

        void Start(Clock::time_point start) { start_ = start; }

        // Transfer completed and parameters/TTL accepted, NOT checksum success.
        // The end timestamp precedes release-buffer RPC and outer checksums.
        void Complete(uint64_t bytes, Clock::time_point end) {
            if (!stats_) return;
            auto* stats = stats_;
            const auto ns =
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    end - start_)
                    .count();
            std::lock_guard<std::mutex> lock(stats->mutex_);
            stats->totals_.bytes += bytes;
            stats->totals_.latency_ns_sum += static_cast<uint64_t>(ns);
            ++stats->totals_.batches;
            --stats->totals_.inflight;
            stats_ = nullptr;
        }

       private:
        SsdToHostFetchStats* stats_;
        Clock::time_point start_{};
    };

   private:
    const std::string instance_id_ = NewIoMetricsInstanceId();
    mutable std::mutex mutex_;
    SsdToHostFetchSnapshot totals_;
    bool classification_complete_ = true;
};

// Shared by BucketStorageBackend and its owner exporter. Counts bucket events,
// not keys, files, syscalls or NVMe commands. Rollback/eviction never subtracts.
// DATA was synced and metadata write succeeded; metadata crash durability is
// NOT guaranteed. Observability adds no sync I/O to the existing write path.
class SsdDataSyncedStats {
   public:
    const std::string& instance_id() const { return instance_id_; }
    void RecordBucketCompleted() {
        buckets_completed_.fetch_add(1, std::memory_order_relaxed);
    }
    uint64_t buckets_completed() const {
        return buckets_completed_.load(std::memory_order_relaxed);
    }

   private:
    const std::string instance_id_ = NewIoMetricsInstanceId();
    std::atomic<uint64_t> buckets_completed_{0};
};

// Completed KV DATA-file bytes, independent of data-synced bucket success.
// Timestamping is at userspace CQE consumption, not kernel/device completion.
enum class SsdKvIoDirection : size_t { Read = 0, Write = 1 };

class SsdKvIoStats {
   public:
    using Clock = std::chrono::steady_clock;
    using Now = std::function<Clock::time_point()>;
    static constexpr uint64_t kBucketWidthNs = 100000000;
    static constexpr size_t kCapacity = 16384;

    struct Bucket {
        uint64_t id = 0;
        std::array<uint64_t, 2> bytes{};
    };
    struct Snapshot {
        std::string instance_id;
        uint64_t snapshot_ns = 0;
        uint64_t oldest_bucket_id = 0;
        uint64_t newest_bucket_id = 0;
        std::array<uint64_t, 2> completed_bytes{};
        std::array<uint64_t, 2> observation_losses{};
        std::array<bool, 2> direction_available{true, true};
        std::vector<Bucket> buckets;
        bool available = true;

        void Serialize(std::string& text) const {
            if (!available) return;
            text += "# HELP mooncake_ssd_kv_io_window_info KV DATA-file CQE "
                    "observations, not device completion timestamps\n"
                    "# TYPE mooncake_ssd_kv_io_window_info gauge\n"
                    "mooncake_ssd_kv_io_window_info{schema_version=\"1\","
                    "instance_id=\"" + instance_id +
                    "\",semantics=\"bucket_data_cqe_observed_100ms_v1\","
                    "clock=\"steady_relative_ns\",backend=\"io_uring\"} 1\n";
            const auto scalar = [&](const char* name, uint64_t value) {
                text += std::string("mooncake_ssd_kv_io_") + name + " " +
                        std::to_string(value) + "\n";
            };
            scalar("bucket_width_ns", kBucketWidthNs);
            scalar("capacity_buckets", kCapacity);
            scalar("snapshot_ns", snapshot_ns);
            scalar("oldest_bucket_id", oldest_bucket_id);
            scalar("newest_bucket_id", newest_bucket_id);
            text += "# TYPE mooncake_ssd_kv_io_direction_available gauge\n";
            for (size_t d = 0; d < 2; ++d) {
                const std::string direction = d == 0 ? "read" : "write";
                text += "mooncake_ssd_kv_io_direction_available{direction=\"" +
                        direction + "\"} " +
                        (direction_available[d] ? "1\n" : "0\n");
                if (!direction_available[d]) continue;
                text +=
                    "mooncake_ssd_kv_io_completed_bytes_total{direction=\"" +
                    direction + "\"} " + std::to_string(completed_bytes[d]) +
                    "\n";
                text +=
                    "mooncake_ssd_kv_io_observation_losses_total{direction=\"" +
                    direction + "\"} " +
                    std::to_string(observation_losses[d]) + "\n";
            }
            // Retention is time based, not the oldest nonempty slot. Emit both
            // directions for a nonempty live bucket; coverage defines zeros.
            for (uint64_t id = oldest_bucket_id; id <= newest_bucket_id; ++id) {
                const auto& bucket = buckets[id % kCapacity];
                if (bucket.id != id || (!bucket.bytes[0] && !bucket.bytes[1]))
                    continue;
                for (size_t d = 0; d < 2; ++d) {
                    if (!direction_available[d]) continue;
                    text += "mooncake_ssd_kv_io_bucket_bytes{bucket_id=\"" +
                            std::to_string(id) + "\",direction=\"" +
                            (d == 0 ? "read" : "write") + "\"} " +
                            std::to_string(bucket.bytes[d]) + "\n";
                }
            }
        }
    };

    explicit SsdKvIoStats(Now now = [] { return Clock::now(); },
                          std::array<bool, 2> direction_available = {true, true})
        : now_(std::move(now)),
          origin_(now_()),
          direction_available_(direction_available) {}

    void RecordCompleted(SsdKvIoDirection direction, uint64_t bytes) {
        if (!bytes) return;
        std::lock_guard<std::mutex> lock(mutex_);
        // Clock and update share the lock: a concurrent snapshot cannot include
        // this timestamp without its bytes (or vice versa).
        const uint64_t id = RelativeNs() / kBucketWidthNs;
        auto& bucket = buckets_[id % kCapacity];
        if (bucket.id != id) bucket = Bucket{id, {}};
        const auto d = static_cast<size_t>(direction);
        bucket.bytes[d] += bytes;
        completed_bytes_[d] += bytes;
    }

    void RecordObservationLoss(SsdKvIoDirection direction) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++observation_losses_[static_cast<size_t>(direction)];
    }

    // Never silently drop attribution and advertise a complete window.
    void Invalidate(SsdKvIoDirection direction) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++observation_losses_[static_cast<size_t>(direction)];
        available_ = false;
    }

    Snapshot GetSnapshot() const {
        Snapshot result;
        result.instance_id = instance_id_;
        result.buckets.resize(kCapacity);  // allocate before taking the lock
        std::lock_guard<std::mutex> lock(mutex_);
        result.snapshot_ns = RelativeNs();
        result.newest_bucket_id = result.snapshot_ns / kBucketWidthNs;
        result.oldest_bucket_id = result.newest_bucket_id >= kCapacity
                                      ? result.newest_bucket_id - kCapacity + 1
                                      : 0;
        result.completed_bytes = completed_bytes_;
        result.observation_losses = observation_losses_;
        result.available = available_;
        result.direction_available = direction_available_;
        std::copy(buckets_.begin(), buckets_.end(), result.buckets.begin());
        return result;
    }

   private:
    uint64_t RelativeNs() const {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                now_() - origin_)
                .count());
    }
    const std::string instance_id_ = NewIoMetricsInstanceId();
    const Now now_;
    const Clock::time_point origin_;
    const std::array<bool, 2> direction_available_;
    mutable std::mutex mutex_;
    std::array<Bucket, kCapacity> buckets_{};
    std::array<uint64_t, 2> completed_bytes_{};
    std::array<uint64_t, 2> observation_losses_{};
    bool available_ = true;
};

namespace detail {

// Attribution survives early SQ preparation errors, so stale-tag CQEs still
// reach their original DATA owner. Metadata/fsync have null observers. This is
// bounded independently of operation count and does not change submission order.
class SsdKvIoCompletionTracker {
   public:
    static constexpr size_t kCapacity = 1024;

    void Prepare(uint64_t tag, std::shared_ptr<SsdKvIoStats> observer,
                 SsdKvIoDirection direction) {
        auto* entry = Find(tag);
        if (!entry) {
            const size_t start = tag % kCapacity;
            for (size_t i = 0; i < kCapacity; ++i) {
                auto& candidate = entries_[(start + i) % kCapacity];
                if (!candidate.prepared && !candidate.submitted) {
                    entry = &candidate;
                    max_probe_ = std::max(max_probe_, i);
                    break;
                }
            }
        }
        if (!entry || pending_tail_ - pending_head_ >= kCapacity) {
            if (observer) observer->Invalidate(direction);
            for (auto& e : entries_)
                if (e.observer) e.observer->Invalidate(e.direction);
            return;
        }
        if (!entry->prepared && !entry->submitted)
            *entry = Entry{tag, std::move(observer), direction, 0, 0, true};
        ++entry->prepared;
        pending_[pending_tail_++ % kCapacity] = entry;
    }

    void Submitted(unsigned count) {
        // Follow SQ order, including scalar chunks sharing one tag.
        while (count-- && pending_head_ != pending_tail_) {
            auto* entry = pending_[pending_head_++ % kCapacity];
            --entry->prepared;
            ++entry->submitted;
        }
    }

    void SubmissionUncertain() {
        // Failed submit-and-wait may have entered the kernel before wait failed.
        // Reaped CQEs count normally; anything left at Reset signals loss.
        for (auto& e : entries_) {
            e.submitted += e.prepared;
            e.prepared = 0;
        }
        pending_head_ = pending_tail_;
    }

    void Observe(uint64_t tag, int result) {
        auto* entry = Find(tag);
        if (!entry || (!entry->prepared && !entry->submitted)) {
            if (result > 0) {
                if (entry) {
                    // Duplicate attribution is known; don't poison metadata or
                    // another DATA direction, and never invent bytes.
                    if (entry->observer)
                        entry->observer->RecordObservationLoss(
                            entry->direction);
                } else {
                    for (auto& e : entries_)
                        if (e.submitted && e.observer)
                            e.observer->RecordObservationLoss(e.direction);
                }
            }
            return;
        }
        if (entry->submitted)
            --entry->submitted;
        else
            --entry->prepared;  // CQE itself proves submission succeeded
        if (result > 0 && entry->observer)
            entry->observer->RecordCompleted(entry->direction,
                                             static_cast<uint64_t>(result));
    }

    void Reset() {
        for (auto& e : entries_) {
            // Known unsubmitted SQEs did no I/O. Submitted but unreaped requests
            // at queue teardown have unknown completion bytes: signal loss.
            if (e.submitted && e.observer)
                e.observer->RecordObservationLoss(e.direction);
            e = Entry{};
        }
        pending_head_ = pending_tail_ = 0;
        max_probe_ = 0;
    }

   private:
    struct Entry {
        uint64_t tag = 0;
        std::shared_ptr<SsdKvIoStats> observer;
        SsdKvIoDirection direction = SsdKvIoDirection::Read;
        unsigned prepared = 0;
        unsigned submitted = 0;
        bool occupied = false;
    };
    Entry* Find(uint64_t tag) {
        const size_t start = tag % kCapacity;
        for (size_t i = 0; i <= max_probe_; ++i) {
            auto& e = entries_[(start + i) % kCapacity];
            if (e.occupied && e.tag == tag) return &e;
        }
        return nullptr;
    }
    std::array<Entry, kCapacity> entries_{};
    std::array<Entry*, kCapacity> pending_{};
    size_t pending_head_ = 0;
    size_t pending_tail_ = 0;
    size_t max_probe_ = 0;
};

}  // namespace detail
}  // namespace mooncake
