#include <glog/logging.h>
#include <gtest/gtest.h>

#include <cstdlib>
#include <string>

#include "client_metric.h"

namespace mooncake::test {

class ClientMetricsTest : public ::testing::Test {
   protected:
    void SetUp() override {
        google::InitGoogleLogging("ClientMetricsTest");
        FLAGS_logtostderr = true;
    }

    void TearDown() override { google::ShutdownGoogleLogging(); }
};

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

    std::string summary = metrics.summary_metrics();

    // Should contain both transfer and RPC metrics
    EXPECT_TRUE(summary.find("Transfer Metrics Summary") != std::string::npos);
    EXPECT_TRUE(summary.find("RPC Metrics Summary") != std::string::npos);
    EXPECT_TRUE(summary.find("Total Read: 5.00 MB") != std::string::npos);
    EXPECT_TRUE(summary.find("Total Write: 10.00 MB") != std::string::npos);
    EXPECT_TRUE(summary.find("ExistKey: count=1") != std::string::npos);

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

TEST_F(ClientMetricsTest, StorageWindowUsesCompletedBucketsWithoutResettingTotals) {
    static int64_t now = 1000000000;
    now = 1000000000;
    StorageIOMetric metric(true, [] { return now; });
    metric.Record(StorageIOKind::SSD_WRITE, 9000, 1);
    auto id = metric.BeginWindow();
    ASSERT_TRUE(id.has_value());
    EXPECT_FALSE(metric.BeginWindow().has_value());
    metric.Record(StorageIOKind::SSD_WRITE, 1000, 1);
    now += 99000000;
    metric.Record(StorageIOKind::SSD_WRITE, 2000, 1);
    now += 1000000;
    metric.Record(StorageIOKind::SSD_WRITE, 2500, 1);
    now += 400000000;
    metric.Record(StorageIOKind::SSD_WRITE, 0, 0, 1);
    auto snapshot = metric.Snapshot();
    EXPECT_EQ(snapshot.window[3].bytes, 5500);
    EXPECT_EQ(snapshot.window[3].ops, 3);
    EXPECT_EQ(snapshot.window[3].errors, 1);
    EXPECT_EQ(snapshot.peak_window_bytes[3], 3000);
    EXPECT_EQ(snapshot.totals[3].bytes, 14500);
    EXPECT_EQ(metric.Snapshot().totals[3].bytes, 14500);
    EXPECT_FALSE(metric.EndWindow(*id + 1));
    EXPECT_TRUE(metric.EndWindow(*id));
    EXPECT_FALSE(metric.EndWindow(*id));
    metric.Record(StorageIOKind::SSD_WRITE, 500, 1);
    EXPECT_EQ(metric.Snapshot().window[3].bytes, 5500);
    EXPECT_EQ(metric.Snapshot().totals[3].bytes, 15000);
    auto next = metric.BeginWindow();
    ASSERT_TRUE(next.has_value());
    EXPECT_EQ(*next, *id + 1);
    EXPECT_EQ(metric.Snapshot().peak_window_bytes[3], 0);
    EXPECT_EQ(metric.Snapshot().totals[3].bytes, 15000);
    EXPECT_TRUE(metric.EndWindow(*next));
}

TEST_F(ClientMetricsTest, StorageWindowAggregatesThreadsAndKeepsDirectionsSeparate) {
    static int64_t now = 2000000000;
    now = 2000000000;
    StorageIOMetric metric(true, [] { return now; });
    auto id = metric.BeginWindow();
    ASSERT_TRUE(id.has_value());
    std::vector<std::thread> workers;
    for (int i = 0; i < 4; ++i) {
        workers.emplace_back([&metric] {
            for (int j = 0; j < 100; ++j) {
                metric.Record(StorageIOKind::SSD_READ, 4096, 1);
                metric.Record(StorageIOKind::DRAM_READ, 2048, 1);
            }
        });
    }
    for (auto& worker : workers) worker.join();
    now += 20000000;
    ASSERT_TRUE(metric.EndWindow(*id));
    auto snapshot = metric.Snapshot();
    EXPECT_EQ(snapshot.window[2].ops, 400);
    EXPECT_EQ(snapshot.peak_window_bytes[2], 400 * 4096);
    EXPECT_EQ(snapshot.window[0].bytes, 400 * 2048);
    EXPECT_EQ(snapshot.window[1].bytes, 0);
    EXPECT_EQ(snapshot.window[3].bytes, 0);
    EXPECT_EQ(snapshot.window_end_ns - snapshot.window_start_ns, 20000000);
    const auto json = metric.SnapshotJson();
    EXPECT_NE(json.find("\"bucket_ns\":100000000"), std::string::npos);
    EXPECT_NE(json.find("\"active\":false"), std::string::npos);
    std::string prometheus;
    metric.Serialize(prometheus);
    EXPECT_NE(prometheus.find("mooncake_storage_io_completed_bytes_total{path=\"ssd_read\"} 1638400"), std::string::npos);
}

TEST_F(ClientMetricsTest, AlignedBucketsUseCompletionTimeAndKeepWarmupOutside) {
    static int64_t now = 1000000000;
    now = 1000000000;
    StorageIOMetric metric(true, [] { return now; });
    metric.Record(StorageIOKind::SSD_READ, 900, 1);
    ASSERT_TRUE(metric.BeginWindow(123, now, true));
    EXPECT_TRUE(metric.BeginWindow(123, now, true));
    EXPECT_FALSE(metric.BeginWindow(124, now, true));
    now = 1250000000;
    metric.Record(StorageIOKind::SSD_READ, 20, 1, 0, 1100000000);
    metric.Record(StorageIOKind::SSD_READ, 10, 1, 0, 1099999999);
    metric.Record(StorageIOKind::SSD_READ, 5, 1, 0, 1000000001);
    metric.Record(StorageIOKind::SSD_READ, 99, 1, 0, 999999999);
    metric.Record(StorageIOKind::SSD_READ, 0, 0, 2);
    EXPECT_FALSE(metric.EndWindow(124, now, false));
    EXPECT_FALSE(metric.EndWindow(123, 1099999999, false));
    ASSERT_TRUE(metric.EndWindow(123, now, false));
    auto snapshot = metric.Snapshot();
    ASSERT_EQ(snapshot.buckets.size(), 2);
    EXPECT_EQ(snapshot.buckets[0].index, 0);
    EXPECT_EQ(snapshot.buckets[0].bytes[2], 15);
    EXPECT_EQ(snapshot.buckets[1].index, 1);
    EXPECT_EQ(snapshot.buckets[1].bytes[2], 20);
    EXPECT_EQ(snapshot.window[2].bytes, 35);
    EXPECT_EQ(snapshot.window[2].ops, 3);
    EXPECT_EQ(snapshot.window[2].errors, 2);
    EXPECT_EQ(snapshot.totals[2].bytes, 1034);
    EXPECT_EQ(snapshot.peak_window_bytes[2], 20);
    EXPECT_TRUE(metric.EndWindow(123, now + 1, false));
    EXPECT_EQ(metric.Snapshot().window_end_ns, now);
    EXPECT_FALSE(metric.BeginWindow(123, now, true));
    metric.Record(StorageIOKind::SSD_READ, 500, 1);
    EXPECT_EQ(metric.Snapshot().window[2].bytes, 35);
    EXPECT_EQ(metric.Snapshot().totals[2].bytes, 1534);
    EXPECT_NE(metric.SnapshotJson().find("\"capture_buckets\":true"), std::string::npos);
}

TEST_F(ClientMetricsTest, AlignedWindowsAbortAndBoundBucketHistory) {
    static int64_t now = 1000000000;
    now = 1000000000;
    StorageIOMetric metric(true, [] { return now; });
    EXPECT_FALSE(metric.BeginWindow(0, now, true));
    EXPECT_FALSE(metric.BeginWindow(1, now + 1, true));
    ASSERT_TRUE(metric.BeginWindow(1, now, true));
    now += StorageIOMetric::kWindowNs * StorageIOMetric::kMaxBuckets;
    metric.Record(StorageIOKind::SSD_READ, 100, 1);
    EXPECT_TRUE(metric.Snapshot().window_overflowed);
    EXPECT_TRUE(metric.Snapshot().buckets.empty());
    EXPECT_EQ(metric.Snapshot().window[2].bytes, 100);
    ASSERT_TRUE(metric.EndWindow(1, now, true));
    EXPECT_TRUE(metric.Snapshot().window_aborted);
    ASSERT_TRUE(metric.BeginWindow(2, now, true));
    EXPECT_FALSE(metric.Snapshot().window_aborted);
    EXPECT_FALSE(metric.Snapshot().window_overflowed);
    EXPECT_EQ(metric.Snapshot().totals[2].bytes, 100);
    EXPECT_EQ(metric.Snapshot().window[2].bytes, 0);
    now += 1000000;
    EXPECT_TRUE(metric.EndWindow(2, now, false));
    EXPECT_FALSE(metric.EndWindow(1, now, true));
    EXPECT_FALSE(metric.Snapshot().window_aborted);
}

TEST_F(ClientMetricsTest, AlignedBucketsAggregateConcurrentCompletions) {
    static int64_t now = 1000000000;
    now = 1000000000;
    StorageIOMetric metric(true, [] { return now; });
    ASSERT_TRUE(metric.BeginWindow(7, now, true));
    now += 200000000;
    std::vector<std::thread> workers;
    for (int i = 0; i < 4; ++i) workers.emplace_back([&metric, i] {
        for (int j = 0; j < 100; ++j)
            metric.Record(StorageIOKind::SSD_READ, 4096, 1, 0, 1000000000 + (i % 2) * 100000000);
    });
    for (auto& worker : workers) worker.join();
    ASSERT_TRUE(metric.EndWindow(7, now, false));
    auto snapshot = metric.Snapshot();
    ASSERT_EQ(snapshot.buckets.size(), 2);
    EXPECT_EQ(snapshot.buckets[0].bytes[2], 200 * 4096);
    EXPECT_EQ(snapshot.buckets[1].bytes[2], 200 * 4096);
    EXPECT_EQ(snapshot.window[2].ops, 400);
}

TEST_F(ClientMetricsTest, DisabledStorageMetricsAreNotEmptyValidMeasurements) {
    StorageIOMetric metric(false);
    metric.Record(StorageIOKind::SSD_READ, 4096, 1);
    EXPECT_FALSE(metric.BeginWindow().has_value());
    EXPECT_FALSE(metric.Snapshot().enabled);
    EXPECT_EQ(metric.Snapshot().totals[2].bytes, 0);
}

}  // namespace mooncake::test
