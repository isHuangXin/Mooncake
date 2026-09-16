#include <glog/logging.h>
#include <gtest/gtest.h>

// csignal must precede coro_http_client.hpp: the bundled ylt's coro_io.hpp
// calls std::signal without including <csignal> itself.
#include <csignal>
#include <cstdlib>
#include <future>
#include <limits>
#include <optional>
#include <string>
#include <unordered_set>
#include <ylt/coro_http/coro_http_client.hpp>

#include "client_metric.h"
#include "dummy_client.h"
#include "real_client.h"
#include "test_server_helpers.h"
#include "utils.h"

namespace mooncake::test {
namespace {

struct HttpResponse {
    int status;
    std::string body;
};

HttpResponse FetchUrl(const std::string& url) {
    coro_http::coro_http_client client;
    auto res = client.get(url);
    return HttpResponse{res.status, std::string(res.resp_body)};
}

int GetTestPort(std::unordered_set<int>& used_ports) {
    for (int i = 0; i < 100; ++i) {
        int port = getFreeTcpPort();
        if (port > 0 && port < 65535 && !used_ports.contains(port)) {
            used_ports.insert(port);
            return port;
        }
    }
    return -1;
}

size_t CountOccurrences(const std::string& text, const std::string& needle) {
    if (needle.empty()) return 0;

    size_t count = 0;
    size_t position = 0;
    while ((position = text.find(needle, position)) != std::string::npos) {
        ++count;
        position += needle.size();
    }
    return count;
}

class ScopedEnv {
   public:
    explicit ScopedEnv(const char* name) : name_(name) {
        const char* value = std::getenv(name);
        if (value) old_value_ = value;
    }

    ~ScopedEnv() {
        if (old_value_) {
            setenv(name_, old_value_->c_str(), 1);
        } else {
            unsetenv(name_);
        }
    }

   private:
    const char* name_;
    std::optional<std::string> old_value_;
};

tl::expected<void, ErrorCode> SetupClientWithHttp(
    const std::shared_ptr<RealClient>& client, const std::string& client_addr,
    const std::string& master_addr, bool enable_http, int http_port) {
    return client->setup_internal(
        client_addr, "P2PHANDSHAKE", /*global_segment_size=*/0,
        /*local_buffer_size=*/0, "tcp", "", master_addr, nullptr, "",
        /*local_rpc_port=*/50052, /*enable_ssd_offload=*/false,
        /*start_offload_rpc_server=*/false, /*ssd_offload_path=*/"",
        /*tenant_id=*/"default", enable_http, http_port);
}

}  // namespace

class ClientMetricsTest : public ::testing::Test {
   protected:
    void SetUp() override {
        google::InitGoogleLogging("ClientMetricsTest");
        FLAGS_logtostderr = true;
    }

    void TearDown() override { google::ShutdownGoogleLogging(); }
};

// FLAT_MEMORY: CPU-only substitutes keep real routing and helper logic intact.
class FetchMetricsClient : public Client {
   public:
    FetchMetricsClient() : Client("metrics-test", "P2PHANDSHAKE", "tcp") {}
    std::function<void()> during_transfer;
    ErrorCode transfer_error = ErrorCode::OK;
    std::optional<uint64_t> expected_checksum;
    size_t transfer_calls = 0;

    std::vector<tl::expected<QueryResult, ErrorCode>> BatchQuery(
        const std::vector<std::string>& keys) override {
        std::vector<tl::expected<QueryResult, ErrorCode>> results;
        for (size_t i = 0; i < keys.size(); ++i) {
            Replica::Descriptor replica;
            replica.status = ReplicaStatus::COMPLETE;
            replica.descriptor_variant =
                LocalDiskDescriptor{{0, 1}, 8, "owner"};
            results.emplace_back(QueryResult(
                {replica}, std::chrono::steady_clock::now() +
                               std::chrono::seconds(30), expected_checksum));
        }
        return results;
    }

    tl::expected<void, ErrorCode> BatchGetOffloadObject(
        const std::string&, const std::vector<std::string>&,
        const std::vector<uintptr_t>&,
        const std::unordered_map<std::string, std::vector<Slice>>&,
        OffloadBufferAccess) override {
        ++transfer_calls;
        if (during_transfer) during_transfer();
        if (transfer_error != ErrorCode::OK) {
            return tl::make_unexpected(transfer_error);
        }
        return {};
    }
};

class FetchMetricsRequester : public ClientRequester {
   public:
    std::function<void()> during_rpc;
    std::function<void()> during_release;
    ErrorCode rpc_error = ErrorCode::OK;
    uint64_t ttl_ms = 30000;
    bool wrong_pointer_count = false;
    size_t rpc_calls = 0;
    size_t releases = 0;

    tl::expected<BatchGetOffloadObjectResponse, ErrorCode>
    batch_get_offload_object(const std::string&,
                             const std::vector<std::string>& keys,
                             const std::vector<int64_t>&) override {
        ++rpc_calls;
        if (during_rpc) during_rpc();
        if (rpc_error != ErrorCode::OK) return tl::make_unexpected(rpc_error);
        return BatchGetOffloadObjectResponse(
            1, std::vector<uint64_t>(wrong_pointer_count ? 0 : keys.size(), 1),
            "owner-te", ttl_ms);
    }
    void release_offload_buffer(const std::string&, uint64_t) override {
        ++releases;
        if (during_release) during_release();
    }
};

class FetchMetricsRealClient : public RealClient {
   public:
    const void* device_pointer = nullptr;
    const void* unknown_pointer = nullptr;

   protected:
    device::MemoryKind ssd_fetch_memory_kind(const void* ptr) const override {
        if (ptr == unknown_pointer) return device::MemoryKind::kUnknown;
        return ptr == device_pointer ? device::MemoryKind::kDevice
                                     : device::MemoryKind::kHost;
    }
};

TEST_F(ClientMetricsTest, IoSnapshotIsCoherentCumulativeAndPerInstance) {
    SsdToHostFetchStats stats;
    const auto epoch = stats.Snapshot(true).instance_id;
    EXPECT_FALSE(epoch.empty());
    EXPECT_NE(epoch, SsdToHostFetchStats().Snapshot(true).instance_id);
    const auto start = SsdToHostFetchStats::Clock::now();
    {
        SsdToHostFetchStats::Attempt attempt(stats, true);
        attempt.Start(start);
        EXPECT_EQ(stats.Snapshot(true).ssd_to_host_fetch->inflight, 1);
        attempt.Complete(128, start + std::chrono::nanoseconds(200));
    }
    {
        SsdToHostFetchStats::Attempt failure(stats, true);
    }
    for (int i = 0; i < 2; ++i) {
        auto snapshot = stats.Snapshot(true);
        EXPECT_EQ(snapshot.schema_version, 1);
        EXPECT_EQ(snapshot.instance_id, epoch);
        EXPECT_EQ(snapshot.capabilities,
                  std::vector<std::string>{"ssd_to_host_fetch_v1"});
        ASSERT_TRUE(snapshot.ssd_to_host_fetch);
        EXPECT_EQ(snapshot.ssd_to_host_fetch->bytes, 128);
        EXPECT_EQ(snapshot.ssd_to_host_fetch->latency_ns_sum, 200);
        EXPECT_EQ(snapshot.ssd_to_host_fetch->batches, 1);
        EXPECT_EQ(snapshot.ssd_to_host_fetch->errors, 1);
        EXPECT_EQ(snapshot.ssd_to_host_fetch->inflight, 0);
    }
    EXPECT_TRUE(stats.Snapshot(false).capabilities.empty());
    EXPECT_FALSE(stats.Snapshot(false).ssd_to_host_fetch);

    std::atomic<bool> done{false};
    std::thread writer([&] {
        for (int i = 0; i < 1000; ++i) {
            SsdToHostFetchStats::Attempt attempt(stats, true);
            attempt.Start(start);
            attempt.Complete(128, start + std::chrono::nanoseconds(200));
        }
        done = true;
    });
    do {
        const auto totals = *stats.Snapshot(true).ssd_to_host_fetch;
        EXPECT_EQ(totals.bytes, totals.batches * 128);
        EXPECT_EQ(totals.latency_ns_sum, totals.batches * 200);
        EXPECT_LE(totals.inflight, 1);
    } while (!done);
    writer.join();
}

TEST_F(ClientMetricsTest, IoSnapshotDummyBackendIsUnavailable) {
    std::shared_ptr<PyClient> dummy = std::make_shared<DummyClient>();
    const auto snapshot = dummy->get_io_stats_snapshot();
    EXPECT_EQ(snapshot.schema_version, 1);
    EXPECT_TRUE(snapshot.instance_id.empty());
    EXPECT_TRUE(snapshot.capabilities.empty());
    EXPECT_FALSE(snapshot.ssd_to_host_fetch);
}

TEST_F(ClientMetricsTest, IoSnapshotRealClientEpochAndLegacyAreNonResetting) {
    RealClient first;
    RealClient second;
    const auto a = first.get_io_stats_snapshot();
    EXPECT_FALSE(a.instance_id.empty());
    EXPECT_NE(a.instance_id, second.get_io_stats_snapshot().instance_id);
    EXPECT_EQ(a.instance_id, first.get_io_stats_snapshot().instance_id);
    first.io_stats_->ssd_read_ns = 123;
    first.io_stats_->ssd_read_ops = 4;
    EXPECT_EQ(first.get_and_reset_io_stats().ssd_read_ns, 123);
    EXPECT_EQ(first.get_and_reset_io_stats().ssd_read_ops, 4);
    EXPECT_EQ(first.get_and_reset_io_stats().ssd_read_ns, 123);
}

TEST_F(ClientMetricsTest, IoFetchOrdinaryAndMultiBufferShareAccounting) {
    FetchMetricsRealClient real;
    auto client = std::make_shared<FetchMetricsClient>();
    auto requester = std::make_shared<FetchMetricsRequester>();
    real.client_ = client;
    real.client_requester_ = requester;
    auto initial = real.get_io_stats_snapshot();
    ASSERT_TRUE(initial.ssd_to_host_fetch);
    char a[8] = {}, b[8] = {};
    requester->during_rpc = [&] {
        EXPECT_EQ(real.get_io_stats_snapshot().ssd_to_host_fetch->inflight, 1);
    };
    requester->during_release = [&] {
        // Completion is published before release-buffer RPC, not delayed by it.
        EXPECT_EQ(real.get_io_stats_snapshot().ssd_to_host_fetch->inflight, 0);
    };
    auto ordinary = real.batch_get_into_internal({"a", "b"}, {a, b}, {8, 8});
    ASSERT_EQ(ordinary.size(), 2);
    ASSERT_TRUE(ordinary[0]);
    ASSERT_TRUE(ordinary[1]);
    auto first = *real.get_io_stats_snapshot().ssd_to_host_fetch;
    EXPECT_EQ(first.bytes, 16);
    EXPECT_EQ(first.batches, 1);  // two keys, one owner sub-batch
    auto multi = real.batch_get_into_multi_buffers_internal(
        {"a", "b"}, {{a, a + 4}, {b, b + 4}}, {{4, 4}, {4, 4}}, false);
    ASSERT_TRUE(multi[0]);
    ASSERT_TRUE(multi[1]);
    auto second = *real.get_io_stats_snapshot().ssd_to_host_fetch;
    EXPECT_EQ(second.bytes, 32);
    EXPECT_EQ(second.batches, 2);
    EXPECT_GE(second.latency_ns_sum, first.latency_ns_sum);
    EXPECT_EQ(second.errors, 0);
    EXPECT_EQ(requester->rpc_calls, 2);
    EXPECT_EQ(client->transfer_calls, 2);
    EXPECT_EQ(requester->releases, 2);
}

TEST_F(ClientMetricsTest, IoFetchChecksumFailureDoesNotUndoTransferSuccess) {
    if (!Environ::Get().GetStoreChecksumEnabled()) {
        GTEST_SKIP() << "Run with MOONCAKE_STORE_CHECKSUM=1";
    }
    FetchMetricsRealClient real;
    auto client = std::make_shared<FetchMetricsClient>();
    client->expected_checksum = 1;
    real.client_ = client;
    real.client_requester_ = std::make_shared<FetchMetricsRequester>();
    char buffer[8] = {};
    auto ordinary = real.batch_get_into_internal({"a"}, {buffer}, {8});
    ASSERT_FALSE(ordinary[0]);
    EXPECT_EQ(ordinary[0].error(), ErrorCode::CHECKSUM_MISMATCH);
    auto multi = real.batch_get_into_multi_buffers_internal(
        {"a"}, {{buffer, buffer + 4}}, {{4, 4}}, false);
    ASSERT_FALSE(multi[0]);
    EXPECT_EQ(multi[0].error(), ErrorCode::CHECKSUM_MISMATCH);
    const auto totals = *real.get_io_stats_snapshot().ssd_to_host_fetch;
    EXPECT_EQ(totals.batches, 2);
    EXPECT_EQ(totals.bytes, 16);
    EXPECT_EQ(totals.errors, 0);
    EXPECT_EQ(real.get_and_reset_io_stats().ssd_read_ops, 0);
}

TEST_F(ClientMetricsTest, IoFetchWaitsForTransferAndExcludesFailures) {
    FetchMetricsRealClient real;
    auto client = std::make_shared<FetchMetricsClient>();
    auto requester = std::make_shared<FetchMetricsRequester>();
    real.client_ = client;
    real.client_requester_ = requester;
    char buffer[8] = {};
    std::unordered_map<std::string, std::vector<Slice>> objects{
        {"a", {{buffer, sizeof(buffer)}}}};
    std::promise<void> entered, finish;
    auto finished = finish.get_future();
    client->during_transfer = [&] {
        entered.set_value();
        finished.wait();
    };
    auto result = std::async(std::launch::async, [&] {
        return real.batch_get_into_offload_object_internal("owner", objects);
    });
    entered.get_future().wait();
    auto pending = *real.get_io_stats_snapshot().ssd_to_host_fetch;
    EXPECT_EQ(pending.batches, 0);
    EXPECT_EQ(pending.bytes, 0);
    EXPECT_EQ(pending.latency_ns_sum, 0);
    EXPECT_EQ(pending.inflight, 1);
    finish.set_value();
    ASSERT_TRUE(result.get());
    client->during_transfer = {};
    auto success = *real.get_io_stats_snapshot().ssd_to_host_fetch;
    EXPECT_EQ(success.batches, 1);
    EXPECT_GT(success.latency_ns_sum, 0);

    requester->rpc_error = ErrorCode::RPC_FAIL;
    EXPECT_FALSE(real.batch_get_into_offload_object_internal("owner", objects));
    requester->rpc_error = ErrorCode::OK;
    client->transfer_error = ErrorCode::TRANSFER_FAIL;
    EXPECT_FALSE(real.batch_get_into_offload_object_internal("owner", objects));
    client->transfer_error = ErrorCode::OK;
    requester->ttl_ms = 0;
    auto expired =
        real.batch_get_into_offload_object_internal("owner", objects);
    ASSERT_FALSE(expired);
    EXPECT_EQ(expired.error(), ErrorCode::OBJECT_HAS_LEASE);
    requester->ttl_ms = 30000;
    requester->wrong_pointer_count = true;
    EXPECT_FALSE(real.batch_get_into_offload_object_internal("owner", objects));
    requester->wrong_pointer_count = false;
    RealClient::OffloadReadRange invalid_range{9, 8};
    EXPECT_FALSE(real.batch_get_into_offload_object_internal(
        "owner", objects, &invalid_range));
    auto after = *real.get_io_stats_snapshot().ssd_to_host_fetch;
    EXPECT_EQ(after.bytes, success.bytes);
    EXPECT_EQ(after.batches, success.batches);
    EXPECT_EQ(after.latency_ns_sum, success.latency_ns_sum);
    EXPECT_EQ(after.errors, 5);
    EXPECT_EQ(after.inflight, 0);
}

TEST_F(ClientMetricsTest, IoFetchExcludesEmptyDeviceMixedAndUnknown) {
    FetchMetricsRealClient real;
    real.client_ = std::make_shared<FetchMetricsClient>();
    real.client_requester_ = std::make_shared<FetchMetricsRequester>();
    char host[8] = {}, gpu[8] = {}, unknown[8] = {};
    real.device_pointer = gpu;
    real.unknown_pointer = unknown;
    std::unordered_map<std::string, std::vector<Slice>> objects;
    EXPECT_TRUE(real.batch_get_into_offload_object_internal("owner", objects));
    objects = {{"a", {{host, 0}}}};
    EXPECT_TRUE(real.batch_get_into_offload_object_internal("owner", objects));
    EXPECT_TRUE(real.batch_get_into_internal({"a"}, {gpu}, {8})[0]);
    EXPECT_TRUE(real.batch_get_into_multi_buffers_internal(
        {"a"}, {{host, gpu}}, {{4, 4}}, false)[0]);
    auto totals = *real.get_io_stats_snapshot().ssd_to_host_fetch;
    EXPECT_EQ(totals.batches, 0);
    EXPECT_EQ(totals.bytes, 0);
    EXPECT_EQ(totals.errors, 0);
    EXPECT_EQ(totals.inflight, 0);
    objects = {{"a", {{unknown, 8}}}};
    EXPECT_TRUE(real.batch_get_into_offload_object_internal("owner", objects));
    const auto unavailable = real.get_io_stats_snapshot();
    EXPECT_TRUE(unavailable.capabilities.empty());
    EXPECT_FALSE(unavailable.ssd_to_host_fetch);
}

TEST_F(ClientMetricsTest, IoDataSyncedMetricsExposeOnlyAttachedSource) {
    SsdMetric metric;
    std::string unavailable;
    metric.serialize(unavailable);
    EXPECT_EQ(unavailable.find("mooncake_ssd_io_info"), std::string::npos);
    EXPECT_EQ(unavailable.find("data_synced_buckets_completed"),
              std::string::npos);
    auto stats = std::make_shared<SsdDataSyncedStats>();
    metric.SetDataSyncedStats(stats);
    stats->RecordBucketCompleted();
    stats->RecordBucketCompleted();
    for (int i = 0; i < 2; ++i) {
        std::string serialized;
        metric.serialize(serialized);
        EXPECT_NE(
            serialized.find("instance_id=\"" + stats->instance_id() + "\""),
            std::string::npos);
        EXPECT_NE(serialized.find(
                      "semantics=\"data_synced_bucket_completions_v1\"} 1"),
                  std::string::npos);
        EXPECT_NE(serialized.find(
                      "mooncake_ssd_data_synced_buckets_completed_total 2\n"),
                  std::string::npos);
        EXPECT_NE(metric.summary_metrics().find(
                      "data_synced_buckets_completed_total=2"),
                  std::string::npos);
    }
    metric.SetDataSyncedStats(nullptr);
    std::string detached;
    metric.serialize(detached);
    EXPECT_EQ(detached.find("mooncake_ssd_io_info"), std::string::npos);
}

// FLAT_MEMORY: pure source tests use an injected clock, never files or rings.
TEST(SsdKvIoMetricsTest, BoundariesOriginAndTimeBasedRetention) {
    using Stats = SsdKvIoStats;
    const auto origin = Stats::Clock::time_point(std::chrono::hours(24));
    uint64_t ns = 0;
    Stats stats([&] { return origin + std::chrono::nanoseconds(ns); });
    const auto initial = stats.GetSnapshot();
    EXPECT_EQ(initial.snapshot_ns, 0);
    EXPECT_EQ(initial.oldest_bucket_id, 0);
    stats.RecordCompleted(SsdKvIoDirection::Read, 11);
    ns = Stats::kBucketWidthNs - 1;
    stats.RecordCompleted(SsdKvIoDirection::Write, 17);
    const auto before = stats.GetSnapshot();
    EXPECT_EQ(before.buckets[0].bytes[0], 11);
    EXPECT_EQ(before.buckets[0].bytes[1], 17);
    ns = Stats::kBucketWidthNs;
    stats.RecordCompleted(SsdKvIoDirection::Read, 23);
    auto after = stats.GetSnapshot();
    EXPECT_EQ(after.newest_bucket_id, 1);
    EXPECT_EQ(after.buckets[1].bytes[0], 23);
    EXPECT_EQ(after.completed_bytes[0], 34);
    EXPECT_EQ(before.buckets[1].bytes[0], 0);  // detached, non-resetting copy
    EXPECT_EQ(after.instance_id, initial.instance_id);

    ns = Stats::kCapacity * Stats::kBucketWidthNs;
    stats.RecordCompleted(SsdKvIoDirection::Write, 31);
    after = stats.GetSnapshot();
    EXPECT_EQ(after.oldest_bucket_id, 1);
    EXPECT_EQ(after.newest_bucket_id, Stats::kCapacity);
    EXPECT_EQ(after.buckets[0].id, Stats::kCapacity);
    EXPECT_EQ(after.buckets[0].bytes[0], 0);
    EXPECT_EQ(after.buckets[0].bytes[1], 31);
    EXPECT_EQ(after.completed_bytes[1], 48);  // evicted bytes remain cumulative

    ns = 3 * Stats::kCapacity * Stats::kBucketWidthNs;
    after = stats.GetSnapshot();
    EXPECT_EQ(after.oldest_bucket_id, 2 * Stats::kCapacity + 1);
    std::string text;
    after.Serialize(text);
    EXPECT_EQ(text.find("mooncake_ssd_kv_io_bucket_bytes{"), std::string::npos);
    EXPECT_NE(text.find("mooncake_ssd_kv_io_completed_bytes_total"
                        "{direction=\"read\"} 34\n"),
              std::string::npos);
}

TEST(SsdKvIoMetricsTest, SharedThreadsAndSnapshotsAreCoherent) {
    auto stats = std::make_shared<SsdKvIoStats>([] {
        return SsdKvIoStats::Clock::time_point{};
    });
    std::atomic<int> running{4};
    std::vector<std::thread> writers;
    for (int t = 0; t < 4; ++t) {
        writers.emplace_back([stats, &running, t] {
            for (int i = 0; i < 1000; ++i)
                stats->RecordCompleted(t % 2 ? SsdKvIoDirection::Write
                                            : SsdKvIoDirection::Read,
                                       7);
            --running;
        });
    }
    do {
        const auto snapshot = stats->GetSnapshot();
        EXPECT_EQ(snapshot.snapshot_ns, 0);
        EXPECT_EQ(snapshot.completed_bytes, snapshot.buckets[0].bytes);
    } while (running.load());
    for (auto& writer : writers) writer.join();
    const auto snapshot = stats->GetSnapshot();
    EXPECT_EQ(snapshot.completed_bytes[0], 14000);
    EXPECT_EQ(snapshot.completed_bytes[1], 14000);
    EXPECT_EQ(snapshot.observation_losses[0], 0);
    EXPECT_EQ(snapshot.observation_losses[1], 0);
}

TEST(SsdKvIoMetricsTest, FrozenWireIsSparseExactAndIndependentOfDurableOps) {
    uint64_t ns = 0;
    auto stats = std::make_shared<SsdKvIoStats>([&] {
        return SsdKvIoStats::Clock::time_point(std::chrono::nanoseconds(ns));
    });
    auto durable = std::make_shared<SsdDataSyncedStats>();
    SsdMetric metric;
    metric.SetDataSyncedStats(durable);
    std::string absent;
    metric.serialize(absent);
    EXPECT_EQ(absent.find("mooncake_ssd_kv_io_"), std::string::npos);
    metric.SetKvIoStats(stats);
    const uint64_t large = (uint64_t{1} << 53) + 1;
    stats->RecordCompleted(SsdKvIoDirection::Read, large);
    stats->RecordObservationLoss(SsdKvIoDirection::Write);
    ns = 2 * SsdKvIoStats::kBucketWidthNs + 3;
    stats->RecordCompleted(SsdKvIoDirection::Write, 9);
    const auto snapshot = stats->GetSnapshot();
    for (int i = 0; i < 2; ++i) {
        std::string text;
        metric.serialize(text);
        EXPECT_NE(
            text.find(
                "mooncake_ssd_kv_io_window_info{schema_version=\"1\","
                "instance_id=\"" + snapshot.instance_id +
                "\",semantics=\"bucket_data_cqe_observed_100ms_v1\","
                "clock=\"steady_relative_ns\",backend=\"io_uring\"} 1\n"),
            std::string::npos);
        for (const auto* line : {
                 "mooncake_ssd_kv_io_bucket_width_ns 100000000\n",
                 "mooncake_ssd_kv_io_capacity_buckets 16384\n",
                 "mooncake_ssd_kv_io_snapshot_ns 200000003\n",
                 "mooncake_ssd_kv_io_oldest_bucket_id 0\n",
                 "mooncake_ssd_kv_io_newest_bucket_id 2\n",
                 "mooncake_ssd_kv_io_completed_bytes_total"
                 "{direction=\"read\"} 9007199254740993\n",
                 "mooncake_ssd_kv_io_completed_bytes_total"
                 "{direction=\"write\"} 9\n",
                 "mooncake_ssd_kv_io_observation_losses_total"
                 "{direction=\"read\"} 0\n",
                 "mooncake_ssd_kv_io_observation_losses_total"
                 "{direction=\"write\"} 1\n",
                 "mooncake_ssd_kv_io_bucket_bytes"
                 "{bucket_id=\"0\",direction=\"read\"} 9007199254740993\n",
                 "mooncake_ssd_kv_io_bucket_bytes"
                 "{bucket_id=\"0\",direction=\"write\"} 0\n",
                 "mooncake_ssd_kv_io_bucket_bytes"
                 "{bucket_id=\"2\",direction=\"read\"} 0\n",
                 "mooncake_ssd_kv_io_bucket_bytes"
                 "{bucket_id=\"2\",direction=\"write\"} 9\n",
                 "mooncake_ssd_data_synced_buckets_completed_total 0\n"}) {
            EXPECT_NE(text.find(line), std::string::npos) << line;
        }
        EXPECT_EQ(text.find("bucket_id=\"1\""), std::string::npos);
    }
    EXPECT_EQ(metric.ssd_write_ops.value(), 0);
    auto replacement = std::make_shared<SsdKvIoStats>();
    EXPECT_NE(snapshot.instance_id, replacement->GetSnapshot().instance_id);
    metric.SetKvIoStats(replacement);
    stats.reset();
    std::string replaced;
    metric.serialize(replaced);
    EXPECT_EQ(replaced.find(snapshot.instance_id), std::string::npos);
    metric.SetKvIoStats(nullptr);
    std::string detached;
    metric.serialize(detached);
    EXPECT_EQ(detached.find("mooncake_ssd_kv_io_"), std::string::npos);
    EXPECT_NE(detached.find("mooncake_ssd_io_info"), std::string::npos);
}

TEST(SsdKvIoMetricsTest, SerializationUsesOneSnapshotClockRead) {
    uint64_t ticks = 0;
    auto stats = std::make_shared<SsdKvIoStats>([&] {
        return SsdKvIoStats::Clock::time_point(
            std::chrono::nanoseconds(ticks++ * SsdKvIoStats::kBucketWidthNs));
    });
    stats->RecordCompleted(SsdKvIoDirection::Read, 8);  // t=100ms, bucket 1
    SsdMetric metric;
    metric.SetKvIoStats(stats);
    std::string text;
    metric.serialize(text);  // t=200ms, one coherent source copy
    EXPECT_EQ(ticks, 3);
    EXPECT_NE(text.find("mooncake_ssd_kv_io_snapshot_ns 200000000\n"),
              std::string::npos);
    EXPECT_NE(text.find("mooncake_ssd_kv_io_newest_bucket_id 2\n"),
              std::string::npos);
    EXPECT_NE(text.find("bucket_id=\"1\",direction=\"read\"} 8\n"),
              std::string::npos);
}

TEST_F(ClientMetricsTest, TransferMetricsSummaryTest) {
    TransferMetric metrics;

    // Test empty metrics
    std::string summary = metrics.summary_metrics();
    EXPECT_TRUE(summary.find("Total Read: 0 B") != std::string::npos);
    EXPECT_TRUE(summary.find("Total Write: 0 B") != std::string::npos);
    EXPECT_TRUE(summary.find("Get: No data") != std::string::npos);
    EXPECT_TRUE(summary.find("Put: No data") != std::string::npos);

    // Add some data
    metrics.total_read_bytes.inc(1024);              // 1KB
    metrics.total_write_bytes.inc(2 * 1024 * 1024);  // 2MB

    // Add latency observations
    metrics.get_latency_us.observe(150);  // 150 microseconds
    metrics.get_latency_us.observe(200);  // 200 microseconds
    metrics.get_latency_us.observe(300);  // 300 microseconds

    metrics.put_latency_us.observe(500);  // 500 microseconds
    metrics.put_latency_us.observe(750);  // 750 microseconds

    summary = metrics.summary_metrics();

    // Check byte formatting
    EXPECT_TRUE(summary.find("Total Read: 1.00 KB") != std::string::npos);
    EXPECT_TRUE(summary.find("Total Write: 2.00 MB") != std::string::npos);
    EXPECT_TRUE(summary.find("Average Read Throughput:") != std::string::npos);
    EXPECT_TRUE(summary.find("Average Write Throughput:") != std::string::npos);

    // Check latency summaries
    EXPECT_TRUE(summary.find("Get: count=3") != std::string::npos);
    EXPECT_TRUE(summary.find("Put: count=2") != std::string::npos);

    // Check percentiles are present
    EXPECT_TRUE(summary.find("p95<") != std::string::npos);
    EXPECT_TRUE(summary.find("max<") != std::string::npos);

    std::cout << "Transfer Metrics Summary:\n" << summary << std::endl;
}

TEST_F(ClientMetricsTest, MasterClientMetricsSummaryTest) {
    MasterClientMetric metrics;

    // Test empty metrics
    std::string summary = metrics.summary_metrics();
    EXPECT_TRUE(summary.find("No RPC calls recorded") != std::string::npos);

    // Add some RPC calls
    std::array<std::string, 1> get_replica_label = {"GetReplicaList"};
    std::array<std::string, 1> mount_segment_label = {"MountSegment"};
    std::array<std::string, 1> unmount_segment_label = {"UnmountSegment"};

    // Simulate RPC calls
    metrics.rpc_count.inc(get_replica_label);
    metrics.rpc_count.inc(get_replica_label);
    metrics.rpc_count.inc(mount_segment_label);
    metrics.rpc_count.inc(unmount_segment_label);

    // Add latency observations
    metrics.rpc_latency.observe(get_replica_label, 200);  // 200 microseconds
    metrics.rpc_latency.observe(get_replica_label, 250);  // 250 microseconds
    metrics.rpc_latency.observe(mount_segment_label, 37789);   // 37.789 ms
    metrics.rpc_latency.observe(unmount_segment_label, 7536);  // 7.536 ms

    summary = metrics.summary_metrics();

    // Check that RPC calls are recorded
    EXPECT_TRUE(summary.find("GetReplicaList: count=2") != std::string::npos);
    EXPECT_TRUE(summary.find("MountSegment: count=1") != std::string::npos);
    EXPECT_TRUE(summary.find("UnmountSegment: count=1") != std::string::npos);

    // Check percentiles are present for RPCs with data
    EXPECT_TRUE(summary.find("p95<") != std::string::npos);
    EXPECT_TRUE(summary.find("max<") != std::string::npos);

    std::cout << "Master Client Metrics Summary:\n" << summary << std::endl;
}

TEST_F(ClientMetricsTest, ClientMetricsSummaryTest) {
    ClientMetric metrics;

    // Add some transfer data
    metrics.transfer_metric.total_read_bytes.inc(5 * 1024 * 1024);    // 5MB
    metrics.transfer_metric.total_write_bytes.inc(10 * 1024 * 1024);  // 10MB

    metrics.transfer_metric.batch_get_latency_us.observe(1500);  // 1.5ms
    metrics.transfer_metric.batch_put_latency_us.observe(2000);  // 2ms

    // Add some RPC data
    std::array<std::string, 1> exist_key_label = {"ExistKey"};
    metrics.master_client_metric.rpc_count.inc(exist_key_label);
    metrics.master_client_metric.rpc_latency.observe(exist_key_label, 180);
    metrics.ObserveTransferOperation(TransferOperationKind::kRead, "get_buffer",
                                     2 * 1024, 220);
    metrics.ObserveTransferOperation(TransferOperationKind::kWrite, "put_batch",
                                     4 * 1024, 420);

    std::string summary = metrics.summary_metrics();

    // Should contain transfer, RPC, and interface metrics
    EXPECT_TRUE(summary.find("Transfer Metrics Summary") != std::string::npos);
    EXPECT_TRUE(summary.find("RPC Metrics Summary") != std::string::npos);
    EXPECT_TRUE(summary.find("Interface Operation Metrics Summary") !=
                std::string::npos);
    EXPECT_TRUE(summary.find("Total Read: 5.00 MB") != std::string::npos);
    EXPECT_TRUE(summary.find("Total Write: 10.00 MB") != std::string::npos);
    EXPECT_TRUE(summary.find("ExistKey: count=1") != std::string::npos);
    EXPECT_TRUE(summary.find("get_buffer: count=1") != std::string::npos);
    EXPECT_TRUE(summary.find("put_batch: count=1") != std::string::npos);

    std::cout << "Full Client Metrics Summary:\n" << summary << std::endl;
}

TEST_F(ClientMetricsTest, ByteFormattingTest) {
    TransferMetric metrics;

    // Test different byte sizes
    metrics.total_read_bytes.inc(512);  // 512 B
    std::string summary = metrics.summary_metrics();
    EXPECT_TRUE(summary.find("512 B") != std::string::npos);

    metrics.total_read_bytes.inc(1024 - 512);  // Total 1024 B = 1 KB
    summary = metrics.summary_metrics();
    EXPECT_TRUE(summary.find("1.00 KB") != std::string::npos);

    metrics.total_read_bytes.inc(1024 * 1024 - 1024);  // Total 1 MB
    summary = metrics.summary_metrics();
    EXPECT_TRUE(summary.find("1.00 MB") != std::string::npos);

    metrics.total_read_bytes.inc(1024LL * 1024 * 1024 -
                                 1024 * 1024);  // Total 1 GB
    summary = metrics.summary_metrics();
    EXPECT_TRUE(summary.find("1.00 GB") != std::string::npos);
}

TEST_F(ClientMetricsTest, CompareWithSerializedMetrics) {
    ClientMetric metrics;

    // Add some data
    metrics.transfer_metric.total_read_bytes.inc(1024 * 1024);
    metrics.transfer_metric.get_latency_us.observe(200);

    std::array<std::string, 1> get_replica_label = {"GetReplicaList"};
    metrics.master_client_metric.rpc_count.inc(get_replica_label);
    metrics.master_client_metric.rpc_latency.observe(get_replica_label, 250);

    // Get both summary and full serialized metrics
    std::string summary = metrics.summary_metrics();
    std::string serialized;
    metrics.serialize(serialized);

    std::cout << "\n=== Summary Metrics ===" << std::endl;
    std::cout << summary << std::endl;

    std::cout << "\n=== Full Serialized Metrics ===" << std::endl;
    std::cout << serialized << std::endl;

    // Summary should be much shorter and more readable
    EXPECT_LT(summary.length(), serialized.length());
    EXPECT_TRUE(summary.find("count=") != std::string::npos);
    EXPECT_TRUE(summary.find("p95<") != std::string::npos ||
                summary.find("No data") != std::string::npos);
    EXPECT_TRUE(summary.find("max<") != std::string::npos ||
                summary.find("No data") != std::string::npos);
}

TEST_F(ClientMetricsTest, HybridHistogramSerializesEachLabelOnce) {
    ylt::metric::hybrid_histogram_1t histogram(
        "request_latency", "Request latency", {10.0, 20.0}, {}, {"route"});
    histogram.observe({"alpha"}, 5);
    histogram.observe({"beta"}, 15);

    std::string serialized;
    histogram.serialize(serialized);

    EXPECT_EQ(
        CountOccurrences(serialized, "request_latency_bucket{route=\"alpha\","),
        3);
    EXPECT_EQ(
        CountOccurrences(serialized, "request_latency_bucket{route=\"beta\","),
        3);
}

TEST_F(ClientMetricsTest, ZeroSumHybridHistogramPreservesExistingMetrics) {
    ylt::metric::hybrid_histogram_1t histogram(
        "request_latency", "Request latency", {10.0}, {}, {"route"});
    histogram.observe({"idle"}, 0);
    std::string serialized = "existing_metric 1\n";

    histogram.serialize(serialized);

    EXPECT_EQ(serialized, "existing_metric 1\n");
}

TEST_F(ClientMetricsTest, BandwidthSummaryRespectsEnvFlag) {
    setenv("MC_STORE_CLIENT_METRIC_BANDWIDTH", "0", 1);
    auto metrics = ClientMetric::Create();
    ASSERT_NE(metrics, nullptr);

    metrics->transfer_metric.total_read_bytes.inc(1024);
    std::string summary = metrics->summary_metrics();
    EXPECT_TRUE(summary.find("Average Read Throughput:") == std::string::npos);

    unsetenv("MC_STORE_CLIENT_METRIC_BANDWIDTH");
}

TEST_F(ClientMetricsTest, SummaryCanOmitMasterRpcMetrics) {
    auto metrics = ClientMetric::Create({}, false);
    ASSERT_NE(metrics, nullptr);

    metrics->ObserveTransferOperation(TransferOperationKind::kRead,
                                      "get_buffer", 1024, 200);
    std::string summary = metrics->summary_metrics();
    std::string serialized;
    metrics->serialize(serialized);

    EXPECT_TRUE(summary.find("RPC Metrics Summary") == std::string::npos);
    EXPECT_TRUE(serialized.find("mooncake_client_rpc_count") ==
                std::string::npos);
}

TEST_F(ClientMetricsTest, SerializeWithDynamicLabels) {
    auto verify = [](const std::string& str) {
        EXPECT_TRUE(str.find("instance_id=\"12345\"") != std::string::npos);
        EXPECT_TRUE(str.find("cluster_id=\"cluster1\"") != std::string::npos);
        EXPECT_TRUE(str.find("replica_id=\"replica1\"") != std::string::npos);
        EXPECT_TRUE(str.find("mount_segment_id=\"mount1\"") !=
                    std::string::npos);
    };

    std::map<std::string, std::string> static_labels = {
        {"instance_id", "12345"},
        {"cluster_id", "cluster1"},
        {"replica_id", "replica1"},
        {"mount_segment_id", "mount1"}};
    std::array<std::string, 1> get_replica_label = {"GetReplicaList"};
    {
        ClientMetric metrics(0, static_labels);
        metrics.transfer_metric.total_read_bytes.inc(1024 * 1024);
        std::string serialized;
        metrics.serialize(serialized);
        verify(serialized);
    }

    {
        ClientMetric metrics(0, static_labels);
        metrics.transfer_metric.get_latency_us.observe(200);
        std::string serialized;
        metrics.serialize(serialized);
        verify(serialized);
    }

    {
        ClientMetric metrics(0, static_labels);
        metrics.master_client_metric.rpc_count.inc(get_replica_label);
        std::string serialized;
        metrics.serialize(serialized);
        verify(serialized);
    }

    {
        ClientMetric metrics(0, static_labels);
        metrics.master_client_metric.rpc_latency.observe(get_replica_label,
                                                         250);
        std::string serialized;
        metrics.serialize(serialized);
        verify(serialized);
    }
}

TEST_F(ClientMetricsTest, SerializeWithoutDynamicLabels) {
    auto verify = [](const std::string& str) {
        EXPECT_TRUE(str.find("instance_id") == std::string::npos);
        EXPECT_TRUE(str.find("cluster_id") == std::string::npos);
        EXPECT_TRUE(str.find("replica_id") == std::string::npos);
        EXPECT_TRUE(str.find("mount_segment_id") == std::string::npos);
    };

    std::array<std::string, 1> get_replica_label = {"GetReplicaList"};
    {
        ClientMetric metrics(0);
        metrics.transfer_metric.total_read_bytes.inc(1024 * 1024);
        std::string serialized;
        metrics.serialize(serialized);
        verify(serialized);
    }

    {
        ClientMetric metrics(0);
        metrics.transfer_metric.get_latency_us.observe(200);
        std::string serialized;
        metrics.serialize(serialized);
        verify(serialized);
    }

    {
        ClientMetric metrics(0);
        metrics.master_client_metric.rpc_count.inc(get_replica_label);
        std::string serialized;
        metrics.serialize(serialized);
        verify(serialized);
    }

    {
        ClientMetric metrics(0);
        metrics.master_client_metric.rpc_latency.observe(get_replica_label,
                                                         250);
        std::string serialized;
        metrics.serialize(serialized);
        verify(serialized);
    }
}

TEST_F(ClientMetricsTest, HttpMetricsEndpointsReturnData) {
    std::unordered_set<int> used_ports;
    int master_rpc_port = GetTestPort(used_ports);
    int master_http_port = GetTestPort(used_ports);
    int http_port = GetTestPort(used_ports);
    int client_port = GetTestPort(used_ports);
    ASSERT_GT(master_rpc_port, 0);
    ASSERT_GT(master_http_port, 0);
    ASSERT_GT(http_port, 0);
    ASSERT_GT(client_port, 0);

    mooncake::testing::InProcMaster master;
    ASSERT_TRUE(master.Start(mooncake::InProcMasterConfigBuilder()
                                 .set_rpc_port(master_rpc_port)
                                 .set_http_metrics_port(master_http_port)
                                 .set_http_metadata_port(0)
                                 .build()));

    auto client = RealClient::create();
    auto setup_result = SetupClientWithHttp(
        client, "127.0.0.1:" + std::to_string(client_port),
        master.master_address(), /*enable_http=*/true, http_port);
    ASSERT_TRUE(setup_result.has_value()) << toString(setup_result.error());

    auto metrics =
        FetchUrl("http://127.0.0.1:" + std::to_string(http_port) + "/metrics");
    EXPECT_EQ(metrics.status, 200);
    EXPECT_EQ(metrics.body.find("metrics not available"), std::string::npos);

    auto summary = FetchUrl("http://127.0.0.1:" + std::to_string(http_port) +
                            "/metrics/summary");
    EXPECT_EQ(summary.status, 200);
    EXPECT_NE(summary.body.find("Client Metrics Summary"), std::string::npos);

    EXPECT_EQ(client->tearDownAll(), 0);
}

TEST_F(ClientMetricsTest, HttpMetricsConfigParserTrimsWhitespace) {
    std::unordered_set<int> used_ports;
    int master_rpc_port = GetTestPort(used_ports);
    int master_http_port = GetTestPort(used_ports);
    int http_port = GetTestPort(used_ports);
    int client_port = GetTestPort(used_ports);
    ASSERT_GT(master_rpc_port, 0);
    ASSERT_GT(master_http_port, 0);
    ASSERT_GT(http_port, 0);
    ASSERT_GT(client_port, 0);

    mooncake::testing::InProcMaster master;
    ASSERT_TRUE(master.Start(mooncake::InProcMasterConfigBuilder()
                                 .set_rpc_port(master_rpc_port)
                                 .set_http_metrics_port(master_http_port)
                                 .set_http_metadata_port(0)
                                 .build()));

    ConfigDict config = {
        {CONFIG_KEY_LOCAL_HOSTNAME, "127.0.0.1:" + std::to_string(client_port)},
        {CONFIG_KEY_METADATA_SERVER, "P2PHANDSHAKE"},
        {CONFIG_KEY_GLOBAL_SEGMENT_SIZE, "0"},
        {CONFIG_KEY_LOCAL_BUFFER_SIZE, "0"},
        {CONFIG_KEY_PROTOCOL, "tcp"},
        {CONFIG_KEY_MASTER_SERVER_ADDR, master.master_address()},
        {CONFIG_KEY_ENABLE_CLIENT_HTTP_SERVER, " true "},
        {CONFIG_KEY_CLIENT_HTTP_PORT, " " + std::to_string(http_port) + " "},
    };

    auto client = RealClient::create();
    auto setup_result = client->setup_internal(config);
    ASSERT_TRUE(setup_result.has_value()) << toString(setup_result.error());

    auto health =
        FetchUrl("http://127.0.0.1:" + std::to_string(http_port) + "/health");
    EXPECT_EQ(health.status, 200);
    EXPECT_NE(health.body.find("\"status\":\"healthy\""), std::string::npos);

    EXPECT_EQ(client->tearDownAll(), 0);
}

TEST_F(ClientMetricsTest, HttpMetricsConfigParserRejectsInvalidIntegers) {
    const char* invalid_ports[] = {
        "9300x",
        "999999999999999999999999",
    };

    for (const char* invalid_port : invalid_ports) {
        ConfigDict config = {
            {CONFIG_KEY_LOCAL_HOSTNAME, "127.0.0.1:1"},
            {CONFIG_KEY_METADATA_SERVER, "P2PHANDSHAKE"},
            {CONFIG_KEY_GLOBAL_SEGMENT_SIZE, "0"},
            {CONFIG_KEY_LOCAL_BUFFER_SIZE, "0"},
            {CONFIG_KEY_PROTOCOL, "tcp"},
            {CONFIG_KEY_ENABLE_CLIENT_HTTP_SERVER, "true"},
            {CONFIG_KEY_CLIENT_HTTP_PORT, invalid_port},
        };

        auto client = RealClient::create();
        auto setup_result = client->setup_internal(config);
        ASSERT_FALSE(setup_result.has_value()) << invalid_port;
        EXPECT_EQ(setup_result.error(), ErrorCode::INVALID_PARAMS)
            << invalid_port;
    }
}

TEST_F(ClientMetricsTest, HttpMetricsEndpointReturns503WhenMetricsDisabled) {
    ScopedEnv metrics_env("MC_STORE_CLIENT_METRIC");
    setenv("MC_STORE_CLIENT_METRIC", "0", 1);

    std::unordered_set<int> used_ports;
    int master_rpc_port = GetTestPort(used_ports);
    int master_http_port = GetTestPort(used_ports);
    int http_port = GetTestPort(used_ports);
    int client_port = GetTestPort(used_ports);
    ASSERT_GT(master_rpc_port, 0);
    ASSERT_GT(master_http_port, 0);
    ASSERT_GT(http_port, 0);
    ASSERT_GT(client_port, 0);

    mooncake::testing::InProcMaster master;
    ASSERT_TRUE(master.Start(mooncake::InProcMasterConfigBuilder()
                                 .set_rpc_port(master_rpc_port)
                                 .set_http_metrics_port(master_http_port)
                                 .set_http_metadata_port(0)
                                 .build()));

    auto client = RealClient::create();
    auto setup_result = SetupClientWithHttp(
        client, "127.0.0.1:" + std::to_string(client_port),
        master.master_address(), /*enable_http=*/true, http_port);
    ASSERT_TRUE(setup_result.has_value()) << toString(setup_result.error());

    auto metrics =
        FetchUrl("http://127.0.0.1:" + std::to_string(http_port) + "/metrics");
    EXPECT_EQ(metrics.status, 503);
    EXPECT_NE(metrics.body.find("metrics not available"), std::string::npos);

    EXPECT_EQ(client->tearDownAll(), 0);
}

TEST_F(ClientMetricsTest, HttpMetricsPortConflictDoesNotFailSetup) {
    std::unordered_set<int> used_ports;
    int master_rpc_port = GetTestPort(used_ports);
    int master_http_port = GetTestPort(used_ports);
    int http_port = GetTestPort(used_ports);
    int first_client_port = GetTestPort(used_ports);
    int second_client_port = GetTestPort(used_ports);
    ASSERT_GT(master_rpc_port, 0);
    ASSERT_GT(master_http_port, 0);
    ASSERT_GT(http_port, 0);
    ASSERT_GT(first_client_port, 0);
    ASSERT_GT(second_client_port, 0);

    mooncake::testing::InProcMaster master;
    ASSERT_TRUE(master.Start(mooncake::InProcMasterConfigBuilder()
                                 .set_rpc_port(master_rpc_port)
                                 .set_http_metrics_port(master_http_port)
                                 .set_http_metadata_port(0)
                                 .build()));

    auto first_client = RealClient::create();
    auto first_setup = SetupClientWithHttp(
        first_client, "127.0.0.1:" + std::to_string(first_client_port),
        master.master_address(), /*enable_http=*/true, http_port);
    ASSERT_TRUE(first_setup.has_value()) << toString(first_setup.error());

    auto second_client = RealClient::create();
    auto second_setup = SetupClientWithHttp(
        second_client, "127.0.0.1:" + std::to_string(second_client_port),
        master.master_address(), /*enable_http=*/true, http_port);
    EXPECT_TRUE(second_setup.has_value()) << toString(second_setup.error());

    EXPECT_EQ(second_client->tearDownAll(), 0);
    EXPECT_EQ(first_client->tearDownAll(), 0);
}

}  // namespace mooncake::test
