// FLAT_MEMORY: SSD bandwidth meter for the *legacy* root_fs_dir persistence
// path.
//
// Why this exists
// ---------------
// The offload subsystem (FileStorage / BucketStorageBackend /
// StorageBackendAdaptor, selected by MOONCAKE_OFFLOAD_STORAGE_BACKEND_DESCRIPTOR)
// already emits "[MOONCAKE_SSD_BW]" bandwidth logs from its BatchOffload /
// BatchLoad paths. However, in the experiment_1 (sglang + mooncake, posix_io)
// configuration the data that actually lands on the SSD is written by a
// *different* code path: the legacy persistence path driven by the master's
// --root_fs_dir flag (cluster_id = "mooncake_cluster"), i.e.
//   Client::PutToLocalFile -> StorageBackend::StoreObject       (WRITE)
//   FilereadWorkerPool::workerThread -> StorageBackend::LoadObject (READ)
// Neither of those had any bandwidth instrumentation, which is why no
// "[MOONCAKE_SSD_BW]" line ever appeared even though the SSD was clearly busy.
//
// This meter closes that gap. It is intentionally *aggregating*: the legacy
// path stores one file per (key, _k/_v) shard and a single benchmark run can
// produce millions of files, so logging once per object would flood the log and
// perturb the very bandwidth we are trying to measure. Instead each call site
// owns a SsdBwMeter and feeds it (bytes, elapsed) samples; the meter emits a
// single rolled-up "[MOONCAKE_SSD_BW]" line every `flush_ops` samples or
// `flush_interval` of wall-clock time, whichever comes first, plus a final
// summary on destruction.
//
// The output format matches the existing offload-path logs (MB processed, ms
// elapsed, GB/s) so downstream log parsing keeps working unchanged.

#pragma once

#include <glog/logging.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <mutex>
#include <string>

namespace mooncake {

// Thread-safe, low-overhead accumulator that periodically logs aggregated SSD
// bandwidth. One instance is meant to be shared by all worker threads of a
// given pool (e.g. the write thread pool, the fileread worker pool).
class SsdBwMeter {
   public:
    // label    : appears in the log line, e.g. "WRITE (legacy)".
    // flush_ops: emit a log line after this many samples have accumulated.
    // flush_interval: also emit if at least this much wall-clock time has
    //                 elapsed since the last flush (covers low-throughput tails).
    explicit SsdBwMeter(std::string label, uint64_t flush_ops = 4096,
                        std::chrono::seconds flush_interval =
                            std::chrono::seconds(5))
        : label_(std::move(label)),
          flush_ops_(flush_ops == 0 ? 1 : flush_ops),
          flush_interval_(flush_interval),
          window_start_(std::chrono::steady_clock::now()) {}

    ~SsdBwMeter() { Flush(/*force=*/true); }

    SsdBwMeter(const SsdBwMeter&) = delete;
    SsdBwMeter& operator=(const SsdBwMeter&) = delete;

    // Record one completed I/O of `bytes` that took `device_us` microseconds.
    // `device_us` is the time spent inside the synchronous read()/write() call,
    // i.e. real device service time at this layer.
    void Record(uint64_t bytes, int64_t device_us) {
        if (bytes == 0) return;
        std::lock_guard<std::mutex> lock(mutex_);
        window_bytes_ += bytes;
        window_device_us_ += (device_us > 0 ? device_us : 0);
        window_ops_ += 1;
        total_bytes_ += bytes;
        total_ops_ += 1;

        const auto now = std::chrono::steady_clock::now();
        const bool ops_due = window_ops_ >= flush_ops_;
        const bool time_due = (now - window_start_) >= flush_interval_;
        if (ops_due || time_due) {
            FlushLocked(now);
        }
    }

    // Public flush (e.g. at the end of a phase). Safe to call anytime.
    void Flush(bool force) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!force && window_ops_ == 0) return;
        FlushLocked(std::chrono::steady_clock::now());
    }

   private:
    void FlushLocked(std::chrono::steady_clock::time_point now) {
        if (window_ops_ == 0) {
            window_start_ = now;
            return;
        }
        const double mb =
            static_cast<double>(window_bytes_) / (1024.0 * 1024.0);
        // Two views of throughput:
        //   * device GB/s  -> sum(bytes) / sum(per-op device time). This is the
        //                     per-thread service rate, useful for understanding
        //                     raw media speed independent of pool concurrency.
        //   * wall  ms     -> wall-clock span of the window, so the reader can
        //                     also derive aggregate (concurrent) throughput.
        const double device_sec =
            static_cast<double>(window_device_us_) / 1e6;
        const double wall_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now - window_start_)
                .count();
        const double device_gbps =
            device_sec > 0.0
                ? static_cast<double>(window_bytes_) /
                      (1024.0 * 1024.0 * 1024.0) / device_sec
                : 0.0;

        LOG(INFO) << "[MOONCAKE_SSD_BW] SSD " << label_ << " bandwidth: "
                  << std::fixed << std::setprecision(1) << mb << " MB in "
                  << std::setprecision(1) << wall_ms << " ms wall / "
                  << std::setprecision(1) << device_sec * 1000.0
                  << " ms device = " << std::setprecision(2) << device_gbps
                  << " GB/s device (" << window_ops_ << " ops, "
                  << total_ops_ << " total ops, " << std::setprecision(1)
                  << static_cast<double>(total_bytes_) / (1024.0 * 1024.0)
                  << " MB total)";

        window_bytes_ = 0;
        window_device_us_ = 0;
        window_ops_ = 0;
        window_start_ = now;
    }

    const std::string label_;
    const uint64_t flush_ops_;
    const std::chrono::seconds flush_interval_;

    std::mutex mutex_;
    uint64_t window_bytes_ = 0;
    int64_t window_device_us_ = 0;
    uint64_t window_ops_ = 0;
    std::chrono::steady_clock::time_point window_start_;

    uint64_t total_bytes_ = 0;
    uint64_t total_ops_ = 0;
};

}  // namespace mooncake
