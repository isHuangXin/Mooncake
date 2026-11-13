#include "gds_reader.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

#include "pyclient.h"
#include "client_metric.h"
#include "tenant_id.h"

#ifdef STORE_USE_GDS
#include <cuda_runtime_api.h>
#include <cufile.h>
#include <fcntl.h>
#include <linux/openat2.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace mooncake {

#ifdef STORE_USE_GDS
namespace {
std::mutex driver_mutex;
size_t driver_users = 0;

void CheckCUDA(cudaError_t error) {
    if (error != cudaSuccess) throw std::runtime_error(cudaGetErrorString(error));
}
void CheckCuFile(CUfileError_t error) {
    if (error.err != CU_FILE_SUCCESS)
        throw std::runtime_error("cuFile error " + std::to_string(error.err));
}
struct FileHandle {
    int fd = -1;
    CUfileHandle_t handle = nullptr;
    ~FileHandle() {
        if (handle) cuFileHandleDeregister(handle);
        if (fd >= 0) ::close(fd);
    }
};
bool SameFile(int fd, const LocalFileRead& range) {
    struct stat st {};
    return !fstat(fd, &st) && S_ISREG(st.st_mode) &&
           static_cast<uint64_t>(st.st_dev) == range.device &&
           static_cast<uint64_t>(st.st_ino) == range.inode &&
           static_cast<uint64_t>(st.st_size) == range.file_size &&
           st.st_mtim.tv_sec == range.mtime_sec && st.st_mtim.tv_nsec == range.mtime_nsec;
}
// One concurrent reader: its own registered staging buffer, stream and cuFile
// handles, which outlive a Read call (registering a handle costs milliseconds).
struct Lane {
    void* scratch = nullptr;
    bool registered = false;
    cudaStream_t stream = nullptr;
    std::unordered_map<std::string, std::unique_ptr<FileHandle>> files;
};
// Bounds the descriptors a lane keeps; the cache is dropped whole when full.
constexpr size_t kMaxOpenFiles = 256;
}
#endif

struct GDSReader::Impl {
    mutable std::mutex stats_mutex;
    StorageIOMetric io_metric{true};
    std::map<std::string, uint64_t> stats{
        {"ssd_read_bytes", 0}, {"ssd_payload_bytes", 0}, {"ssd_read_ops", 0},
        {"ssd_read_ns", 0}, {"read_errors", 0}, {"dram_payload_bytes", 0},
        {"gpu_staging_bytes", 0}, {"host_staging_peak_bytes", 0}};
    void Add(const std::string& key, uint64_t value) {
        std::lock_guard lock(stats_mutex);
        stats[key] += value;
        if (key == "read_errors") io_metric.Record(StorageIOKind::SSD_READ, 0, 0, value);
    }
#ifdef STORE_USE_GDS
    std::filesystem::path root;
    int root_fd = -1;
    int device = 0;
    size_t max_io = 0;
    bool driver_open = false;
    std::vector<std::unique_ptr<Lane>> lanes;
    std::vector<Lane*> idle;
    std::mutex lane_mutex;
    std::condition_variable lane_ready;

    ~Impl() {
        cudaSetDevice(device);
        for (auto& lane : lanes) {
            if (lane->stream) cudaStreamSynchronize(lane->stream);
            lane->files.clear();
            if (lane->registered) cuFileBufDeregister(lane->scratch);
            if (lane->scratch) cudaFree(lane->scratch);
            if (lane->stream) cudaStreamDestroy(lane->stream);
        }
        if (root_fd >= 0) ::close(root_fd);
        if (driver_open) {
            std::lock_guard lock(driver_mutex);
            if (--driver_users == 0) cuFileDriverClose();
        }
    }

    Lane* Acquire() {
        std::unique_lock lock(lane_mutex);
        lane_ready.wait(lock, [this] { return !idle.empty(); });
        Lane* lane = idle.back();
        idle.pop_back();
        return lane;
    }
    void Release(Lane* lane) {
        {
            std::lock_guard lock(lane_mutex);
            idle.push_back(lane);
        }
        lane_ready.notify_one();
    }

    FileHandle& Open(Lane& lane, const LocalFileRead& range) {
        auto relative = std::filesystem::path(range.path).lexically_relative(root);
        if (relative.empty() || relative.is_absolute())
            throw std::runtime_error("SSD file is outside the configured root");
        auto found = lane.files.find(range.path);
        // A cached descriptor of a since-replaced bucket file is reopened.
        if (found != lane.files.end() && !SameFile(found->second->fd, range))
            lane.files.erase(found), found = lane.files.end();
        if (found != lane.files.end()) return *found->second;
        if (lane.files.size() >= kMaxOpenFiles) lane.files.clear();
        auto file = std::make_unique<FileHandle>();
        struct open_how how {};
        how.flags = O_RDONLY | O_DIRECT | O_CLOEXEC;
        how.resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS;
        file->fd = syscall(SYS_openat2, root_fd, relative.c_str(), &how, sizeof(how));
        if (file->fd < 0) throw std::runtime_error("Cannot open local SSD range");
        if (!SameFile(file->fd, range)) throw std::runtime_error("Local SSD file identity changed");
        CUfileDescr_t descriptor {};
        descriptor.type = CU_FILE_HANDLE_TYPE_OPAQUE_FD;
        descriptor.handle.fd = file->fd;
        CheckCuFile(cuFileHandleRegister(&file->handle, &descriptor));
        return *(lane.files[range.path] = std::move(file));
    }

    void ReadFile(Lane& lane, const LocalFileRead& range, uintptr_t destination, size_t size) {
        if (range.path.empty() || range.size != size || !size ||
            range.offset > range.file_size || size > range.file_size - range.offset)
            throw std::runtime_error("Invalid or missing local SSD range");
        FileHandle& file = Open(lane, range);
        uint64_t copied = 0;
        while (copied < size) {
            uint64_t begin = (range.offset + copied) & ~uint64_t(4095);
            size_t skip = range.offset + copied - begin;
            size_t payload = std::min(size - copied, max_io - skip);
            uint64_t end = std::min(range.file_size,
                (begin + skip + payload + 4095) & ~uint64_t(4095));
            size_t bytes = end - begin;
            auto start = std::chrono::steady_clock::now();
            ssize_t result = cuFileRead(file.handle, lane.scratch, bytes, begin, 0);
            const auto finish = std::chrono::steady_clock::now();
            const auto finish_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(finish.time_since_epoch()).count();
            io_metric.Record(StorageIOKind::SSD_READ, result > 0 ? static_cast<uint64_t>(result) : 0, 1, 0, finish_ns);
            Add("ssd_read_ns", std::chrono::duration_cast<std::chrono::nanoseconds>(finish - start).count());
            Add("ssd_read_ops", 1);
            if (result > 0) Add("ssd_read_bytes", result);
            if (result != static_cast<ssize_t>(bytes))
                throw std::runtime_error("cuFile SSD read failed or returned a short read");
            CheckCUDA(cudaMemcpyAsync(reinterpret_cast<void*>(destination + copied),
                static_cast<char*>(lane.scratch) + skip, payload, cudaMemcpyDeviceToDevice, lane.stream));
            CheckCUDA(cudaStreamSynchronize(lane.stream));
            copied += payload;
        }
        Add("ssd_payload_bytes", size);
    }
#endif
};

GDSReader::GDSReader(const std::string& root, int device, size_t max_io_bytes, int workers)
    : impl_(std::make_unique<Impl>()) {
#ifndef STORE_USE_GDS
    throw std::runtime_error("GDS (compat) requires a STORE_USE_GDS build");
#else
    auto& p = *impl_;
    if (device < 0 || max_io_bytes < 4096 || max_io_bytes % 4096 ||
        max_io_bytes > (256ULL << 20) || workers < 1 || workers > 64)
        throw std::invalid_argument("Invalid GDS device or buffer size");
    p.root = std::filesystem::canonical(root);
    p.root_fd = ::open(p.root.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC);
    if (p.root_fd < 0) throw std::runtime_error("Cannot open GDS SSD root");
    p.device = device;
    p.max_io = max_io_bytes;
    CheckCUDA(cudaSetDevice(device));
    {
        std::lock_guard lock(driver_mutex);
        if (!driver_users) {
            setenv("CUFILE_FORCE_COMPAT_MODE", "true", 1);
            CheckCuFile(cuFileDriverOpen());
        }
        ++driver_users;
        p.driver_open = true;
    }
    for (int i = 0; i < workers; ++i) {
        auto& lane = *p.lanes.emplace_back(std::make_unique<Lane>());
        CheckCUDA(cudaStreamCreateWithFlags(&lane.stream, cudaStreamNonBlocking));
        CheckCUDA(cudaMalloc(&lane.scratch, max_io_bytes));
        CheckCuFile(cuFileBufRegister(lane.scratch, max_io_bytes, 0));
        lane.registered = true;
        p.idle.push_back(&lane);
    }
    p.stats["gpu_staging_bytes"] = max_io_bytes * workers;
    p.stats["workers"] = workers;
#endif
}

GDSReader::~GDSReader() = default;

std::map<std::string, uint64_t> GDSReader::Stats() const {
    std::lock_guard lock(impl_->stats_mutex);
    return impl_->stats;
}

// Reads record at completion time, so a window counts the reads that finish in it.
bool GDSReader::BeginIOWindow(uint64_t window_id, int64_t start_ns) {
    return impl_->io_metric.BeginWindow(window_id, start_ns, true);
}

bool GDSReader::EndIOWindow(uint64_t window_id, int64_t end_ns, bool abort) {
    return impl_->io_metric.EndWindow(window_id, end_ns, abort);
}

std::string GDSReader::IOWindow() const {
    auto output = impl_->io_metric.SnapshotJson();
#ifdef STORE_USE_GDS
    const bool available = impl_->driver_open && !impl_->lanes.empty();
#else
    const bool available = false;
#endif
    // Reader readiness does not prove a successful SSD file registration or read.
    output.pop_back();
    output += std::string(",\"ssd_io_available\":") +
              (available ? "true}" : "false}");
    return output;
}
int64_t GDSReader::IOClockNs() { return StorageIOMetric::MonotonicNs(); }

std::vector<int> GDSReader::Read(const std::shared_ptr<PyClient>& store,
    const std::vector<std::string>& keys,
    const std::vector<uintptr_t>& destinations, const std::vector<size_t>& sizes,
    bool skip_memory) {
#ifndef STORE_USE_GDS
    throw std::runtime_error("GDS (compat) requires a STORE_USE_GDS build");
#else
    auto& p = *impl_;
    if (!store || !store->client_ || !store->client_requester_ ||
        keys.size() != sizes.size() || keys.size() != destinations.size() ||
        keys.size() > 4096 ||
        std::unordered_set<std::string>(keys.begin(), keys.end()).size() != keys.size())
        throw std::invalid_argument("Invalid GDS read batch (keys must be unique)");
    CheckCUDA(cudaSetDevice(p.device));
    for (size_t i = 0; i < keys.size(); ++i) {
        cudaPointerAttributes attrs {};
        CheckCUDA(cudaPointerGetAttributes(&attrs, reinterpret_cast<void*>(destinations[i])));
        if (!sizes[i] || attrs.type != cudaMemoryTypeDevice || attrs.device != p.device)
            throw std::invalid_argument("GDS requires device buffers on its configured GPU");
    }
    std::vector<int> results(keys.size(), 0);
    Lane* lane = p.Acquire();
    struct LaneGuard {
        Impl& p;
        Lane* lane;
        ~LaneGuard() { p.Release(lane); }
    } lane_guard{p, lane};
    auto queries = store->client_->BatchQuery(keys);
    // FLAT_MEMORY: v0.3.13 offload RPCs address tenant-scoped storage keys.
    const TenantId tenant_id(store->client_->tenant_id());
    std::unordered_map<std::string, std::vector<size_t>> disks;
    std::vector<std::string> memory_keys;
    std::vector<QueryResult> memory_queries;
    std::vector<size_t> memory_indices;
    std::vector<BufferHandle> handles;
    std::unordered_map<std::string, std::vector<Slice>> slices;
    uint64_t host_bytes = 0;
    for (size_t i = 0; i < keys.size(); ++i) {
        if (!queries[i] || queries[i]->replicas.empty()) continue;
        const auto& replica = queries[i]->replicas.front();
        if (calculate_total_size(replica) != sizes[i]) continue;
        if (queries[i]->replicas.size() == 1 && replica.is_local_disk_replica()) {
            const auto& endpoint = replica.get_local_disk_descriptor().transport_endpoint;
            if (!endpoint.starts_with("127.0.0.1:"))
                throw std::runtime_error("GDS (compat) supports only a local SSD owner");
            disks[endpoint].push_back(i);
        } else if (!skip_memory) {
            // skip_memory leaves memory replicas to the caller's host-staged read.
            auto memory = std::find_if(queries[i]->replicas.begin(), queries[i]->replicas.end(),
                [](const auto& candidate) { return candidate.is_memory_replica(); });
            if (memory == queries[i]->replicas.end()) continue;
            auto buffer = store->client_buffer_allocator_->allocate(sizes[i]);
            if (!buffer) continue;
            allocateSlices(slices[keys[i]], *memory, buffer->ptr());
            handles.push_back(std::move(*buffer));
            memory_keys.push_back(keys[i]);
            memory_queries.push_back(*queries[i]);
            memory_indices.push_back(i);
            host_bytes += sizes[i];
        }
    }
    {
        std::lock_guard stats_lock(p.stats_mutex);
        p.stats["host_staging_peak_bytes"] = std::max(p.stats["host_staging_peak_bytes"], host_bytes);
    }
    if (!memory_keys.empty()) {
        auto loaded = store->client_->BatchGet(memory_keys, memory_queries, slices);
        for (size_t j = 0; j < loaded.size(); ++j) {
            size_t i = memory_indices[j];
            if (!loaded[j]) continue;
            CheckCUDA(cudaMemcpyAsync(reinterpret_cast<void*>(destinations[i]),
                                  handles[j].ptr(), sizes[i], cudaMemcpyHostToDevice, lane->stream));
            CheckCUDA(cudaStreamSynchronize(lane->stream));
            p.Add("dram_payload_bytes", sizes[i]);
            results[i] = 1;
        }
    }
    for (const auto& [endpoint, indices] : disks) {
        std::vector<std::string> disk_keys;
        std::vector<int64_t> disk_sizes;
        for (size_t i : indices) {
            disk_keys.push_back(tenant_id.MakeScopedKey(keys[i]));
            disk_sizes.push_back(sizes[i]);
        }
        auto batch = store->client_requester_->acquire_local_reads(endpoint, disk_keys, disk_sizes);
        if (!batch) {
            p.Add("read_errors", indices.size());
            LOG(ERROR) << "Cannot acquire GDS SSD read ranges: " << batch.error();
            continue;
        }
        // Open FDs preserve immutable bucket data even if a read lease expires.
        if (batch->files.size() == indices.size()) {
            for (size_t j = 0; j < indices.size(); ++j) {
                size_t i = indices[j];
                try {
                    p.ReadFile(*lane, batch->files[j], destinations[i], sizes[i]);
                    results[i] = 2;
                } catch (const std::exception& error) {
                    p.Add("read_errors", 1);
                    LOG(ERROR) << "GDS (compat) read failed: " << error.what();
                }
            }
        } else {
            p.Add("read_errors", indices.size());
        }
        store->client_requester_->release_offload_buffer(endpoint, batch->batch_id);
    }
    CheckCUDA(cudaStreamSynchronize(lane->stream));
    return results;
#endif
}

}  // namespace mooncake
