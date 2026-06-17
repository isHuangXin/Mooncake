#include "file_storage.h"

#include <future>
#include <iomanip>
#include <memory>
#include <vector>

#include "aligned_client_buffer.hpp"
#include "storage_backend.h"
#include "utils.h"
#ifdef USE_URING
#include "file_interface.h"
#endif

namespace mooncake {

FileStorageConfig FileStorageConfig::FromEnvironment() {
    FileStorageConfig config;

    auto storage_backend_descriptor =
        GetEnvStringOr("MOONCAKE_OFFLOAD_STORAGE_BACKEND_DESCRIPTOR",
                       "bucket_storage_backend");

    if (storage_backend_descriptor == "bucket_storage_backend") {
        config.storage_backend_type = StorageBackendType::kBucket;
    } else if (storage_backend_descriptor == "file_per_key_storage_backend") {
        config.storage_backend_type = StorageBackendType::kFilePerKey;
    } else if (storage_backend_descriptor ==
               "offset_allocator_storage_backend") {
        config.storage_backend_type = StorageBackendType::kOffsetAllocator;
    } else {
        LOG(ERROR) << "Unknown storage backend.";
    }

    config.storage_filepath = GetEnvStringOr(
        "MOONCAKE_OFFLOAD_FILE_STORAGE_PATH", config.storage_filepath);

    config.local_buffer_size = GetEnvOr<int64_t>(
        "MOONCAKE_OFFLOAD_LOCAL_BUFFER_SIZE_BYTES", config.local_buffer_size);

    config.scanmeta_iterator_keys_limit =
        GetEnvOr<int64_t>("MOONCAKE_SCANMETA_ITERATOR_KEYS_LIMIT",
                          config.scanmeta_iterator_keys_limit);

    config.total_keys_limit = GetEnvOr<int64_t>(
        "MOONCAKE_OFFLOAD_TOTAL_KEYS_LIMIT", config.total_keys_limit);

    config.total_size_limit = GetEnvOr<int64_t>(
        "MOONCAKE_OFFLOAD_TOTAL_SIZE_LIMIT_BYTES", config.total_size_limit);

    config.heartbeat_interval_seconds =
        GetEnvOr<uint32_t>("MOONCAKE_OFFLOAD_HEARTBEAT_INTERVAL_SECONDS",
                           config.heartbeat_interval_seconds);
    config.client_buffer_gc_interval_seconds =
        GetEnvOr<uint32_t>("MOONCAKE_OFFLOAD_CLIENT_BUFFER_GC_INTERVAL_SECONDS",
                           config.heartbeat_interval_seconds);

    config.client_buffer_gc_ttl_ms =
        GetEnvOr<uint64_t>("MOONCAKE_OFFLOAD_CLIENT_BUFFER_GC_TTL_MS",
                           config.client_buffer_gc_ttl_ms);

    auto use_uring_str = GetEnvStringOr("MOONCAKE_USE_URING", "false");
    config.use_uring = (use_uring_str == "true" || use_uring_str == "1");

    return config;
}

bool FileStorageConfig::ValidatePath(std::string path) const {
    if (path.empty()) {
        LOG(ERROR) << "FileStorageConfig: storage_filepath is invalid";
        return false;
    }
    namespace fs = std::filesystem;
    // 1. Must be an absolute path
    if (!fs::path(path).is_absolute()) {
        LOG(ERROR)
            << "FileStorageConfig: storage_filepath must be an absolute path: "
            << path;
        return false;
    }

    // 2. Check if the path contains ".." components that could lead to path
    // traversal (static check)
    fs::path p(path);
    for (const auto& component : p) {
        if (component == "..") {
            LOG(ERROR) << "FileStorageConfig: path traversal is not allowed: "
                       << path;
            return false;
        }
    }

    struct stat stat_buf;

    // 3. Use stat() to check if the path exists
    if (::stat(path.c_str(), &stat_buf) != 0) {
        LOG(ERROR) << "FileStorageConfig: storage_filepath does not exist: "
                   << path;
        return false;
    }
    // Path exists — check if it is a directory
    if (!S_ISDIR(stat_buf.st_mode)) {
        LOG(ERROR) << "FileStorageConfig: storage_filepath is not a directory: "
                   << path;
        return false;
    }

    // (Optional) Check write permission
    if (::access(path.c_str(), W_OK) != 0) {
        LOG(ERROR) << "FileStorageConfig: no write permission on directory: "
                   << path;
        return false;
    }

    // 4. Additional security: prevent symlink bypass (optional)
    // Use lstat to avoid automatic dereferencing of symbolic links
    struct stat lstat_buf;
    if (::lstat(path.c_str(), &lstat_buf) == 0) {
        if (S_ISLNK(lstat_buf.st_mode)) {
            LOG(ERROR) << "FileStorageConfig: symbolic link is not allowed: "
                       << path;
            return false;
        }
    }

    return true;
}

bool FileStorageConfig::Validate() const {
    if (!ValidatePath(storage_filepath)) {
        return false;
    }
    if (total_keys_limit <= 0) {
        LOG(ERROR) << "FileStorageConfig: total_keys_limit must > 0";
        return false;
    }
    if (total_size_limit == 0) {
        LOG(ERROR) << "FileStorageConfig: total_size_limit should not be zero";
        return false;
    }
    if (heartbeat_interval_seconds <= 0) {
        LOG(ERROR) << "FileStorageConfig: heartbeat_interval_seconds must > 0";
        return false;
    }
    return true;
}

FileStorage::FileStorage(const FileStorageConfig& config,
                         std::shared_ptr<Client> client,
                         const std::string& local_rpc_addr)
    : config_(config),
      client_(client),
      local_rpc_addr_(local_rpc_addr),
      client_buffer_allocator_(
          AlignedClientBufferAllocator::create(config.local_buffer_size, "")) {
    if (!config.Validate()) {
        throw std::invalid_argument("Invalid FileStorage configuration");
    }

    auto create_storage_backend_result = CreateStorageBackend(config_);
    if (!create_storage_backend_result) {
        LOG(ERROR) << "Failed to create storage backend";
        throw std::runtime_error("Failed to create storage backend");
    }

    storage_backend_ = create_storage_backend_result.value();

    // Register the client buffer with the process-wide io_uring fixed-buffer
    // mechanism. This must happen before any I/O threads start so that they
    // can lazily pick up the registration on their first I/O call.
#ifdef USE_URING
    if (config.use_uring) {
        auto aligned_allocator =
            std::static_pointer_cast<AlignedClientBufferAllocator>(
                client_buffer_allocator_);
        if (aligned_allocator) {
            void* base_ptr = aligned_allocator->get_base_pointer();
            size_t size = aligned_allocator->get_total_size();
            if (UringFile::register_global_buffer(base_ptr, size)) {
                LOG(INFO) << "Successfully registered buffer with UringFile: "
                          << "base=" << base_ptr << ", size=" << size;
            } else {
                LOG(WARNING) << "Failed to register buffer with UringFile";
            }
        }
    }
#endif
}

FileStorage::~FileStorage() {
    LOG(INFO) << "Shutdown FileStorage...";
    heartbeat_running_ = false;
    if (heartbeat_thread_.joinable()) {
        heartbeat_thread_.join();
    }
    client_buffer_gc_running_ = false;
    if (client_buffer_gc_thread_.joinable()) {
        client_buffer_gc_thread_.join();
    }
}

tl::expected<void, ErrorCode> FileStorage::Init() {
    auto register_memory_result = RegisterLocalMemory();
    if (!register_memory_result) {
        LOG(ERROR) << "Failed to register local memory: "
                   << register_memory_result.error();
        return register_memory_result;
    }
    auto init_storage_backend_result = storage_backend_->Init();
    if (!init_storage_backend_result) {
        LOG(ERROR) << "Failed to init storage backend: "
                   << init_storage_backend_result.error();
        return init_storage_backend_result;
    }
    auto enable_offloading_result = IsEnableOffloading();
    if (!enable_offloading_result) {
        LOG(ERROR) << "Failed to get enable persist result, error : "
                   << enable_offloading_result.error();
        return tl::make_unexpected(enable_offloading_result.error());
    }
    {
        MutexLocker locker(&offloading_mutex_);
        enable_offloading_ = enable_offloading_result.value();
        auto mount_file_storage_result =
            client_->MountLocalDiskSegment(enable_offloading_);
        if (!mount_file_storage_result) {
            LOG(ERROR) << "Failed to mount file storage: "
                       << mount_file_storage_result.error();
            return mount_file_storage_result;
        }
    }

    auto scan_meta_result = storage_backend_->ScanMeta(
        [this](const std::vector<std::string>& keys,
               std::vector<StorageObjectMetadata>& metadatas) {
            for (auto& metadata : metadatas) {
                metadata.transport_endpoint = local_rpc_addr_;
            }
            auto add_object_result =
                client_->NotifyOffloadSuccess(keys, metadatas);
            if (!add_object_result) {
                LOG(ERROR) << "Failed to add object to master: "
                           << add_object_result.error();
                return add_object_result.error();
            }
            return ErrorCode::OK;
        });

    if (!scan_meta_result) {
        LOG(ERROR) << "Failed to scan meta and send to master: "
                   << scan_meta_result.error();
        return scan_meta_result;
    }

    heartbeat_running_.store(true);
    heartbeat_thread_ = std::thread([this]() {
        LOG(INFO) << "Starting periodic task with interval: "
                  << config_.heartbeat_interval_seconds
                  << "s, running is: " << heartbeat_running_.load();
        while (heartbeat_running_.load()) {
            Heartbeat();
            std::this_thread::sleep_for(
                std::chrono::seconds(config_.heartbeat_interval_seconds));
        }
    });
    client_buffer_gc_running_.store(true);
    client_buffer_gc_thread_ =
        std::thread(&FileStorage::ClientBufferGCThreadFunc, this);

    // MOONCAKE_SSD_OPT: Create dedicated thread pool for parallel bucket
    // offload.  Threads are reused across eviction rounds, avoiding per-wave
    // std::async thread creation/destruction overhead.
    const int offload_parallelism =
        GetEnvOr<int>("MOONCAKE_OFFLOAD_PARALLELISM", 8);
    if (offload_parallelism > 1) {
        offload_thread_pool_ =
            std::make_unique<ThreadPool>(offload_parallelism);
        LOG(INFO) << "[MOONCAKE_SSD_OPT] Offload thread pool created with "
                  << offload_parallelism << " threads";
    }

    return {};
}

tl::expected<FileStorage::BatchGetResult, ErrorCode> FileStorage::BatchGet(
    const std::vector<std::string>& keys, const std::vector<int64_t>& sizes) {
    auto start_time = std::chrono::steady_clock::now();
    auto allocate_res = AllocateBatch(keys, sizes);
    if (!allocate_res) {
        LOG(ERROR) << "Failed to allocate batch objects";
        return tl::make_unexpected(allocate_res.error());
    }
    auto allocated_batch = allocate_res.value();

    // MOONCAKE_SSD_OPT: Measure pure SSD read bandwidth
    auto read_start = std::chrono::steady_clock::now();
    auto result = BatchLoad(allocated_batch->slices);
    auto read_elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
                               std::chrono::steady_clock::now() - read_start)
                               .count();
    if (!result) {
        LOG(ERROR) << "Batch load object failed,err_code = " << result.error();
        return tl::make_unexpected(result.error());
    }

    // FLAT_MEMORY: Log multi-perspective SSD read bandwidth
    // ① batch (BatchLoad only), ② experiment-wide
    {
        size_t total_read_bytes = 0;
        for (const auto& [key, slice] : allocated_batch->slices) {
            total_read_bytes += slice.size;
        }

        // Update cumulative counters for experiment-wide tracking
        auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        int64_t expected_ts = 0;
        first_read_ts_us_.compare_exchange_strong(expected_ts, now_us);
        cumulative_read_bytes_.fetch_add(total_read_bytes);

        if (read_elapsed_us > 0 && total_read_bytes > 0) {
            double read_bw_gbps =
                static_cast<double>(total_read_bytes) /
                (1024.0 * 1024.0 * 1024.0) /
                (static_cast<double>(read_elapsed_us) / 1e6);
            // ① batch: BatchLoad only
            LOG(INFO) << "[MOONCAKE_SSD_OPT] SSD READ bandwidth:"
                      << "\n  \xe2\x91\xa0 batch:      "
                      << total_read_bytes / (1024 * 1024) << " MB in "
                      << read_elapsed_us / 1000 << "ms = "
                      << std::fixed << std::setprecision(2) << read_bw_gbps
                      << " GB/s (BatchLoad, " << keys.size() << " keys)";
            // ② experiment-wide
            auto cum_bytes = cumulative_read_bytes_.load();
            auto first_ts = first_read_ts_us_.load();
            if (first_ts > 0) {
                double wall_sec = static_cast<double>(now_us - first_ts) / 1e6;
                if (wall_sec > 0.001) {
                    double exp_bw = static_cast<double>(cum_bytes) /
                                    (1024.0 * 1024.0 * 1024.0) / wall_sec;
                    LOG(INFO) << "  \xe2\x91\xa1 exp-wide:   "
                              << cum_bytes / (1024 * 1024) << " MB over "
                              << std::fixed << std::setprecision(1) << wall_sec
                              << "s = " << std::setprecision(2) << exp_bw
                              << " GB/s (cumulative since first read)";
                }
            }
        }
    }

    // After BatchLoad, slice.ptr may have been adjusted by offset_in_buffer
    // (for O_DIRECT aligned reads). Update pointers to reflect actual data
    // positions.
    for (size_t i = 0; i < keys.size(); ++i) {
        auto it = allocated_batch->slices.find(keys[i]);
        if (it != allocated_batch->slices.end()) {
            allocated_batch->pointers[i] =
                reinterpret_cast<uintptr_t>(it->second.ptr);
        }
    }

    uint64_t batch_id = allocated_batch->batch_id;
    BatchGetResult batch_result{batch_id, allocated_batch->pointers};

    MutexLocker locker(&client_buffer_mutex_);
    client_buffer_allocated_batches_.emplace(batch_id,
                                             std::move(allocated_batch));
    auto end_time = std::chrono::steady_clock::now();
    auto elapsed_time = std::chrono::duration_cast<std::chrono::microseconds>(
                            end_time - start_time)
                            .count();
    VLOG(1) << "Time taken for FileStorage::BatchGet: " << elapsed_time
            << "us, key size: " << keys.size() << ", batch_id: " << batch_id;
    return batch_result;
}

tl::expected<void, ErrorCode> FileStorage::OffloadObjects(
    const std::unordered_map<std::string, int64_t>& offloading_objects) {
    std::vector<std::vector<std::string>> buckets_keys;
    if (auto bucket_backend =
            std::dynamic_pointer_cast<BucketStorageBackend>(storage_backend_)) {
        auto allocate_res = bucket_backend->AllocateOffloadingBuckets(
            offloading_objects, buckets_keys);
        if (!allocate_res) {
            LOG(ERROR) << "AllocateOffloadingBuckets failed with error: "
                       << allocate_res.error();
            return allocate_res;
        }
    } else {
        std::vector<std::string> keys;
        keys.reserve(offloading_objects.size());
        for (const auto& it : offloading_objects) {
            keys.emplace_back(it.first);
        }
        buckets_keys.emplace_back(std::move(keys));
    }

    auto complete_handler =
        [this](const std::vector<std::string>& keys,
               std::vector<StorageObjectMetadata>& metadatas) -> ErrorCode {
        VLOG(1) << "Success to store objects, keys count: " << keys.size();
        for (auto& metadata : metadatas) {
            metadata.transport_endpoint = local_rpc_addr_;
        }
        auto result = client_->NotifyOffloadSuccess(keys, metadatas);
        if (!result) {
            LOG(ERROR) << "NotifyOffloadSuccess failed with error: "
                       << result.error();
            return result.error();
        }
        return ErrorCode::OK;
    };

    // MOONCAKE_SSD_OPT: Three-phase bucket offload using ThreadPool + io_uring
    // batch write.  Parallelism is configured via MOONCAKE_OFFLOAD_PARALLELISM
    // env var (default 8) and thread pool is created in Init().
    const size_t num_buckets = buckets_keys.size();

    if (num_buckets <= 1 || !offload_thread_pool_) {
        // Single bucket or parallelism disabled — use original serial path
        for (const auto& keys : buckets_keys) {
            std::unordered_map<std::string, std::vector<Slice>> batch_object;
            auto query_result = BatchQuerySegmentSlices(keys, batch_object);
            if (!query_result) {
                LOG(ERROR) << "BatchQuerySlices failed with error: "
                           << query_result.error();
                continue;
            }

            auto eviction_handler = [this](const std::string& evicted_key) {
                auto result =
                    client_->EvictDiskReplica(evicted_key, ReplicaType::LOCAL_DISK);
                if (!result) {
                    LOG(WARNING)
                        << "Failed to notify master about evicted local disk key: "
                        << evicted_key << ", error: " << result.error();
                }
            };

            auto offload_res = storage_backend_->BatchOffload(
                batch_object, complete_handler, eviction_handler);
            if (!offload_res) {
                LOG(ERROR) << "Failed to store objects with error: "
                           << offload_res.error();
                if (offload_res.error() == ErrorCode::KEYS_ULTRA_LIMIT) {
                    MutexLocker locker(&offloading_mutex_);
                    enable_offloading_ = false;
                    return tl::make_unexpected(offload_res.error());
                }
                if (offload_res.error() != ErrorCode::INVALID_READ) {
                    return tl::make_unexpected(offload_res.error());
                }
            }
        }
    } else {
        // MOONCAKE_SSD_OPT: Three-phase pipeline using ThreadPool + io_uring
        // batch write.
        //
        //  Phase 1 (ThreadPool, parallel): BuildBucket + PrepareWriteBucket
        //    — CPU work: BatchQuerySegmentSlices, memcpy to aligned buffer,
        //      open file, store metadata.  Each bucket is independent.
        //
        //  Phase 2 (single thread): io_uring batch_write
        //    — Submit all bucket writes as SQEs in one batch, then reap all
        //      CQEs at once.  Maximises NVMe device queue depth.
        //
        //  Phase 3 (ThreadPool, parallel): complete_handler + metadata commit
        //    — Notify master of offload success and commit to metadata maps.
        //
        auto bucket_backend =
            std::dynamic_pointer_cast<BucketStorageBackend>(storage_backend_);

        LOG(INFO) << "[MOONCAKE_SSD_OPT] Three-phase offload: "
                  << num_buckets << " buckets, pool_threads="
                  << (offload_thread_pool_ ? "yes" : "no");
        auto offload_start = std::chrono::steady_clock::now();

        // ---- Phase 1: Parallel Prepare ----
        std::vector<std::future<
            tl::expected<BucketStorageBackend::PreparedBucket, ErrorCode>>>
            prepare_futures;
        prepare_futures.reserve(num_buckets);

        std::atomic<bool> keys_ultra_limit{false};

        for (size_t i = 0; i < num_buckets && !keys_ultra_limit; ++i) {
            const auto& keys = buckets_keys[i];
            prepare_futures.push_back(offload_thread_pool_->submit(
                [this, &keys, &keys_ultra_limit, &bucket_backend]()
                    -> tl::expected<BucketStorageBackend::PreparedBucket,
                                    ErrorCode> {
                    // Query segment slices (CPU, read-only on DRAM)
                    std::unordered_map<std::string, std::vector<Slice>>
                        batch_object;
                    auto query_result =
                        BatchQuerySegmentSlices(keys, batch_object);
                    if (!query_result) {
                        LOG(ERROR)
                            << "Phase1: BatchQuerySlices failed: "
                            << query_result.error();
                        return tl::make_unexpected(query_result.error());
                    }

                    // PrepareBatchOffload: BuildBucket + PrepareWriteBucket
                    auto prepare_res =
                        bucket_backend->PrepareBatchOffload(batch_object);
                    if (!prepare_res &&
                        prepare_res.error() ==
                            ErrorCode::KEYS_ULTRA_LIMIT) {
                        keys_ultra_limit.store(true,
                                               std::memory_order_relaxed);
                    }
                    return prepare_res;
                }));
        }

        // Collect Phase 1 results
        std::vector<BucketStorageBackend::PreparedBucket> prepared;
        prepared.reserve(num_buckets);
        bool phase1_fatal = false;

        for (auto& fut : prepare_futures) {
            auto res = fut.get();
            if (!res) {
                LOG(ERROR) << "Phase1: bucket prepare failed: " << res.error();
                if (res.error() == ErrorCode::KEYS_ULTRA_LIMIT) {
                    MutexLocker locker(&offloading_mutex_);
                    enable_offloading_ = false;
                    phase1_fatal = true;
                    break;
                }
                continue;  // skip this bucket, proceed with others
            }
            prepared.push_back(std::move(res.value()));
        }

        if (phase1_fatal) {
            return tl::make_unexpected(ErrorCode::KEYS_ULTRA_LIMIT);
        }

        if (prepared.empty()) {
            LOG(WARNING) << "[MOONCAKE_SSD_OPT] No buckets prepared, skipping "
                            "Phase 2/3";
            return {};
        }

        auto phase1_elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - offload_start)
                .count();

        // ---- Phase 2: io_uring batch write ----
        auto phase2_start = std::chrono::steady_clock::now();

        if (bucket_backend) {
            auto flush_res = bucket_backend->FlushPreparedBuckets(prepared);
            if (!flush_res) {
                LOG(ERROR) << "Phase2: FlushPreparedBuckets failed: "
                           << flush_res.error();
                return tl::make_unexpected(flush_res.error());
            }
        }

        auto phase2_elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - phase2_start)
                .count();

        // ---- Phase 3: Parallel Finalize (complete_handler + metadata) ----
        auto phase3_start = std::chrono::steady_clock::now();

        std::vector<std::future<tl::expected<int64_t, ErrorCode>>>
            finalize_futures;
        finalize_futures.reserve(prepared.size());

        for (auto& p : prepared) {
            finalize_futures.push_back(offload_thread_pool_->submit(
                [&complete_handler, &bucket_backend,
                 bucket_id = p.bucket_id,
                 metadata = std::move(p.metadata),
                 metadatas = std::move(p.metadatas)]() mutable
                    -> tl::expected<int64_t, ErrorCode> {
                    // Notify master of offload success
                    auto error_code =
                        complete_handler(metadata->keys, metadatas);
                    if (error_code != ErrorCode::OK) {
                        LOG(ERROR)
                            << "Phase3: complete_handler failed, id="
                            << bucket_id;
                        return tl::make_unexpected(error_code);
                    }

                    // Commit metadata to in-memory maps
                    return bucket_backend->CommitBucket(
                        bucket_id, std::move(metadata), metadatas);
                }));
        }

        // Collect Phase 3 results
        for (auto& fut : finalize_futures) {
            auto res = fut.get();
            if (!res) {
                LOG(ERROR) << "Phase3: finalize failed: " << res.error();
                if (res.error() != ErrorCode::OBJECT_ALREADY_EXISTS) {
                    // non-fatal for duplicates, fatal for others
                }
            }
        }

        auto total_elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - offload_start)
                .count();
        auto phase3_elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - phase3_start)
                .count();
        LOG(INFO) << "[MOONCAKE_SSD_OPT] Three-phase offload completed: "
                  << prepared.size() << "/" << num_buckets << " buckets in "
                  << total_elapsed << "ms (P1=" << phase1_elapsed
                  << "ms, P2=" << phase2_elapsed
                  << "ms, P3=" << phase3_elapsed << "ms)";

        // FLAT_MEMORY: Log multi-perspective SSD write bandwidth
        // ① burst (Phase 2 only), ② end-to-end (P1+P2+P3), ③ experiment-wide
        size_t total_write_bytes = 0;
        for (const auto& p : prepared) total_write_bytes += p.aligned_size;

        // Update cumulative counters for experiment-wide tracking
        auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        int64_t expected_ts = 0;
        first_write_ts_us_.compare_exchange_strong(expected_ts, now_us);
        cumulative_write_bytes_.fetch_add(total_write_bytes);

        if (total_write_bytes > 0) {
            auto to_gbps = [](size_t bytes, double ms) -> double {
                return static_cast<double>(bytes) /
                       (1024.0 * 1024.0 * 1024.0) / (ms / 1000.0);
            };
            size_t mb = total_write_bytes / (1024 * 1024);

            // ① burst: Phase 2 only (pure io_uring)
            if (phase2_elapsed > 0) {
                LOG(INFO) << "[MOONCAKE_SSD_OPT] SSD WRITE bandwidth:"
                          << "\n  \xe2\x91\xa0 burst:      " << mb << " MB in "
                          << phase2_elapsed << "ms = "
                          << std::fixed << std::setprecision(2)
                          << to_gbps(total_write_bytes, phase2_elapsed)
                          << " GB/s (io_uring batch_write, "
                          << prepared.size() << " SQEs)";
            }
            // ② end-to-end: P1+P2+P3
            if (total_elapsed > 0) {
                LOG(INFO) << "  \xe2\x91\xa1 end-to-end: " << mb << " MB in "
                          << total_elapsed << "ms = "
                          << std::fixed << std::setprecision(2)
                          << to_gbps(total_write_bytes, total_elapsed)
                          << " GB/s (P1+P2+P3 pipeline)";
            }
            // ③ experiment-wide
            auto cum_bytes = cumulative_write_bytes_.load();
            auto first_ts = first_write_ts_us_.load();
            if (first_ts > 0) {
                double wall_sec = static_cast<double>(now_us - first_ts) / 1e6;
                if (wall_sec > 0.001) {
                    double exp_bw = static_cast<double>(cum_bytes) /
                                    (1024.0 * 1024.0 * 1024.0) / wall_sec;
                    LOG(INFO) << "  \xe2\x91\xa2 exp-wide:   "
                              << cum_bytes / (1024 * 1024) << " MB over "
                              << std::fixed << std::setprecision(1) << wall_sec
                              << "s = " << std::setprecision(2) << exp_bw
                              << " GB/s (cumulative since first write)";
                }
            }
        }
    }
    return {};
}

tl::expected<bool, ErrorCode> FileStorage::IsEnableOffloading() {
    auto is_enable_offloading_result = storage_backend_->IsEnableOffloading();
    if (!is_enable_offloading_result) {
        LOG(ERROR) << "Failed to get enabling offload: "
                   << is_enable_offloading_result.error();
        return tl::make_unexpected(is_enable_offloading_result.error());
    }

    auto enable_offloading = is_enable_offloading_result.value();

    return enable_offloading;
}

tl::expected<void, ErrorCode> FileStorage::Heartbeat() {
    if (client_ == nullptr) {
        LOG(ERROR) << "client is nullptr";
        return tl::make_unexpected(ErrorCode::INVALID_PARAMS);
    }
    std::unordered_map<std::string, int64_t>
        offloading_objects;  // Objects selected for offloading

    // === STEP 1: Send heartbeat and get offloading decisions ===
    {
        MutexLocker locker(&offloading_mutex_);
        auto heartbeat_result = client_->OffloadObjectHeartbeat(
            enable_offloading_, offloading_objects);
        if (!heartbeat_result) {
            LOG(ERROR) << "Failed to send heartbeat with error: "
                       << heartbeat_result.error();
            return heartbeat_result;
        }
    }

    // === STEP 2: Persist offloaded objects (trigger actual data migration) ===
    auto offload_result = OffloadObjects(offloading_objects);
    if (!offload_result) {
        LOG(ERROR) << "Failed to persist objects with error: "
                   << offload_result.error();
        return offload_result;
    }

    // TODO(eviction): Implement an LRU eviction mechanism to manage local
    // storage capacity.
    return {};
}

tl::expected<void, ErrorCode> FileStorage::BatchLoad(
    std::unordered_map<std::string, Slice>& batch_object) {
    auto start_time = std::chrono::steady_clock::now();
    auto result = storage_backend_->BatchLoad(batch_object);
    auto end_time = std::chrono::steady_clock::now();
    auto elapsed_time = std::chrono::duration_cast<std::chrono::microseconds>(
                            end_time - start_time)
                            .count();
    VLOG(1) << "Time taken for BatchStore: " << elapsed_time
            << "us,with keys count: " << batch_object.size();
    if (!result) {
        LOG(ERROR) << "Batch load object failed,err_code = " << result.error();
    }
    return result;
}

tl::expected<void, ErrorCode> FileStorage::BatchQuerySegmentSlices(
    const std::vector<std::string>& keys,
    std::unordered_map<std::string, std::vector<Slice>>& batched_slices) {
    auto batched_query_results = client_->BatchQuery(keys);
    if (batched_query_results.empty())
        return tl::make_unexpected(ErrorCode::INVALID_REPLICA);
    for (size_t i = 0; i < batched_query_results.size(); ++i) {
        if (batched_query_results[i]) {
            for (const auto& descriptor :
                 batched_query_results[i].value().replicas) {
                if (descriptor.is_memory_replica()) {
                    const auto& memory_descriptor =
                        descriptor.get_memory_descriptor();
                    if (memory_descriptor.buffer_descriptor
                            .transport_endpoint_ ==
                        client_->GetTransportEndpoint()) {
                        std::vector<Slice> slices;
                        void* slice_ptr = reinterpret_cast<void*>(
                            memory_descriptor.buffer_descriptor
                                .buffer_address_);
                        slices.emplace_back(
                            Slice{slice_ptr,
                                  memory_descriptor.buffer_descriptor.size_});
                        batched_slices.insert({keys[i], std::move(slices)});
                        break;
                    }
                }
            }
            if (batched_slices.find(keys[i]) == batched_slices.end()) {
                LOG(ERROR) << "Key not found: " << keys[i];
                return tl::make_unexpected(ErrorCode::INVALID_KEY);
            }
        } else {
            LOG(ERROR) << "Key not found: " << keys[i];
            return tl::make_unexpected(batched_query_results[i].error());
        }
    }
    return {};
}

tl::expected<void, ErrorCode> FileStorage::RegisterLocalMemory() {
    auto error_code = client_->RegisterLocalMemory(
        client_buffer_allocator_->getBase(), config_.local_buffer_size,
        kWildcardLocation, false, false);
    if (!error_code) {
        LOG(ERROR) << "Failed to register local memory: " << error_code.error();
        return error_code;
    }
    return {};
}

tl::expected<std::shared_ptr<FileStorage::AllocatedBatch>, ErrorCode>
FileStorage::AllocateBatch(const std::vector<std::string>& keys,
                           const std::vector<int64_t>& sizes) {
    auto result = std::make_shared<AllocatedBatch>();
    result->batch_id = next_batch_id_.fetch_add(1, std::memory_order_relaxed);
    std::chrono::steady_clock::time_point now =
        std::chrono::steady_clock::now();
    auto lease_timeout =
        now + std::chrono::milliseconds(config_.client_buffer_gc_ttl_ms);
    static constexpr size_t kDirectIOAlignment = 4096;

    u_int64_t total_size = 0;
    bool gc_triggered = false;
    for (size_t i = 0; i < keys.size(); ++i) {
        assert(sizes[i] <= kMaxSliceSize);

        // Allocate oversized buffer for O_DIRECT alignment:
        //   +4096 for aligning the ptr to 4096 boundary
        //   +4096 for aligned read tail padding (actual_offset may not be
        //   aligned)
        size_t data_size = static_cast<size_t>(sizes[i]);
        size_t alloc_size =
            align_up(data_size, kDirectIOAlignment) + 2 * kDirectIOAlignment;

        auto alloc_result = client_buffer_allocator_->allocate(alloc_size);
        if (!alloc_result && !gc_triggered) {
            gc_triggered = true;
            {
                MutexLocker locker(&client_buffer_mutex_);
                auto gc_now = std::chrono::steady_clock::now();
                for (auto it = client_buffer_allocated_batches_.begin();
                     it != client_buffer_allocated_batches_.end();) {
                    if (gc_now >= it->second->lease_timeout) {
                        it = client_buffer_allocated_batches_.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
            alloc_result = client_buffer_allocator_->allocate(alloc_size);
        }
        if (!alloc_result) {
            LOG(ERROR) << "Failed to allocate slice buffer, size = "
                       << alloc_size << " (data_size=" << data_size
                       << "), key = " << keys[i];
            return tl::make_unexpected(ErrorCode::BUFFER_OVERFLOW);
        }

        // Align ptr to 4096 boundary for O_DIRECT
        void* raw_ptr = alloc_result->ptr();
        void* aligned_ptr = reinterpret_cast<void*>(
            (reinterpret_cast<uintptr_t>(raw_ptr) + kDirectIOAlignment - 1) &
            ~(kDirectIOAlignment - 1));

        total_size += data_size;
        // Slice records data_size; the buffer behind aligned_ptr is oversized
        // to accommodate aligned reads
        result->slices.emplace(keys[i], Slice{aligned_ptr, data_size});
        // pointers will be adjusted after BatchLoad (offset_in_buffer
        // correction)
        result->pointers.emplace_back(reinterpret_cast<uintptr_t>(aligned_ptr));
        result->handles.emplace_back(std::move(alloc_result.value()));
        result->lease_timeout = lease_timeout;
    }
    result->total_size = total_size;
    return result;
}

void FileStorage::ClientBufferGCThreadFunc() {
    LOG(INFO) << "action=client_buffer_gc_thread_started";
    while (client_buffer_gc_running_) {
        {
            MutexLocker locker(&client_buffer_mutex_);
            if (!client_buffer_allocated_batches_.empty()) {
                auto now = std::chrono::steady_clock::now();
                for (auto it = client_buffer_allocated_batches_.begin();
                     it != client_buffer_allocated_batches_.end();) {
                    if (now >= it->second->lease_timeout) {
                        VLOG(1) << "GC releasing batch_id: " << it->first
                                << " (lease expired)";
                        it = client_buffer_allocated_batches_.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
        }
        std::this_thread::sleep_for(
            std::chrono::seconds(config_.client_buffer_gc_interval_seconds));
    }
    LOG(INFO) << "action=client_buffer_gc_thread_stopped";
}

bool FileStorage::ReleaseBuffer(uint64_t batch_id) {
    MutexLocker locker(&client_buffer_mutex_);
    auto it = client_buffer_allocated_batches_.find(batch_id);
    if (it != client_buffer_allocated_batches_.end()) {
        VLOG(1) << "Releasing buffer for batch_id: " << batch_id
                << " (transfer completed)";
        client_buffer_allocated_batches_.erase(it);
        return true;
    }
    VLOG(1) << "batch_id " << batch_id
            << " not found (may have been GC'd already)";
    return false;
}

}  // namespace mooncake