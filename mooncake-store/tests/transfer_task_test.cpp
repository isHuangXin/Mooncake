// transfer_task_test.cpp
#include "transfer_task.h"

#include <glog/logging.h>
#include <gtest/gtest.h>

#include <chrono>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#include "types.h"

namespace mooncake {

// Test fixture for TransferTask tests
// TODO: Currently, this test does not cover TransferSubmitter and
// TransferEngine integration. Will add more tests in the future.
class TransferTaskTest : public ::testing::Test {
   protected:
    void SetUp() override {
        // Initialize glog for logging
        google::InitGoogleLogging("TransferTaskTest");
        FLAGS_logtostderr = 1;  // Output logs to stderr
    }

    void TearDown() override {
        // Cleanup glog
        google::ShutdownGoogleLogging();
    }
};

// Test basic MemcpyOperation functionality
TEST_F(TransferTaskTest, MemcpyOperationBasic) {
    const size_t data_size = 1024;
    std::vector<char> src_data(data_size, 'A');
    std::vector<char> dest_data(data_size, 'B');

    // Create memcpy operation
    MemcpyOperation op(dest_data.data(), src_data.data(), data_size);

    // Verify operation parameters
    EXPECT_EQ(op.dest, dest_data.data());
    EXPECT_EQ(op.src, src_data.data());
    EXPECT_EQ(op.size, data_size);

    // Perform memcpy manually to test
    std::memcpy(op.dest, op.src, op.size);

    // Verify data was copied correctly
    EXPECT_EQ(dest_data, src_data);
    for (size_t i = 0; i < data_size; ++i) {
        EXPECT_EQ(dest_data[i], 'A');
    }
}

// Test MemcpyOperationState functionality
TEST_F(TransferTaskTest, MemcpyOperationState) {
    auto state = std::make_shared<MemcpyOperationState>();

    // Initially not completed
    EXPECT_FALSE(state->is_completed());
    EXPECT_EQ(state->get_strategy(), TransferStrategy::LOCAL_MEMCPY);

    // Set completed with success
    state->set_completed(ErrorCode::OK);
    EXPECT_TRUE(state->is_completed());
    EXPECT_EQ(state->get_result(), ErrorCode::OK);
}

// Test MemcpyWorkerPool basic functionality
TEST_F(TransferTaskTest, MemcpyWorkerPoolBasic) {
    MemcpyWorkerPool pool;

    const size_t data_size = 512;
    std::vector<char> src_data(data_size, 'X');
    std::vector<char> dest_data(data_size, 'Y');

    auto state = std::make_shared<MemcpyOperationState>();

    // Create memcpy operations
    std::vector<MemcpyOperation> operations;
    operations.emplace_back(dest_data.data(), src_data.data(), data_size);

    // Create and submit task
    MemcpyTask task(std::move(operations), state);
    pool.submitTask(std::move(task));

    // Wait for completion
    state->wait_for_completion();

    // Verify completion and result
    EXPECT_TRUE(state->is_completed());
    EXPECT_EQ(state->get_result(), ErrorCode::OK);

    // Verify data was copied correctly
    for (size_t i = 0; i < data_size; ++i) {
        EXPECT_EQ(dest_data[i], 'X');
    }
}

// Test multiple memcpy operations in one task
TEST_F(TransferTaskTest, MemcpyWorkerPoolMultipleOperations) {
    MemcpyWorkerPool pool;

    const size_t num_ops = 3;
    const size_t data_size = 256;

    std::vector<std::vector<char>> src_buffers(num_ops);
    std::vector<std::vector<char>> dest_buffers(num_ops);

    // Initialize source buffers with different patterns
    for (size_t i = 0; i < num_ops; ++i) {
        src_buffers[i].resize(data_size, 'A' + i);
        dest_buffers[i].resize(data_size, 'Z');
    }

    auto state = std::make_shared<MemcpyOperationState>();

    // Create multiple memcpy operations
    std::vector<MemcpyOperation> operations;
    for (size_t i = 0; i < num_ops; ++i) {
        operations.emplace_back(dest_buffers[i].data(), src_buffers[i].data(),
                                data_size);
    }

    // Create and submit task
    MemcpyTask task(std::move(operations), state);
    pool.submitTask(std::move(task));

    // Wait for completion
    state->wait_for_completion();

    // Verify completion and result
    EXPECT_TRUE(state->is_completed());
    EXPECT_EQ(state->get_result(), ErrorCode::OK);

    // Verify all data was copied correctly
    for (size_t i = 0; i < num_ops; ++i) {
        for (size_t j = 0; j < data_size; ++j) {
            EXPECT_EQ(dest_buffers[i][j], 'A' + i);
        }
    }
}

// Test TransferStrategy enum and stream operator
TEST_F(TransferTaskTest, TransferStrategyEnum) {
    // Test enum values
    EXPECT_EQ(static_cast<int>(TransferStrategy::LOCAL_MEMCPY), 0);
    EXPECT_EQ(static_cast<int>(TransferStrategy::TRANSFER_ENGINE), 1);

    // Test stream operator
    std::ostringstream oss;
    oss << TransferStrategy::LOCAL_MEMCPY;
    EXPECT_EQ(oss.str(), "LOCAL_MEMCPY");

    oss.str("");
    oss << TransferStrategy::TRANSFER_ENGINE;
    EXPECT_EQ(oss.str(), "TRANSFER_ENGINE");
}

TEST_F(TransferTaskTest, StorageCountersWaitForSuccessfulCompletionExactlyOnce) {
    const auto before = StorageIOMetric::Instance().Snapshot();
    auto state = std::make_shared<MemcpyOperationState>();
    state->ConfigureStorageIO(StorageIOKind::DRAM_WRITE, 4096, 2);
    TransferFuture original(state);
    TransferFuture moved(std::move(original));
    EXPECT_EQ(StorageIOMetric::Instance().Snapshot().totals[1].bytes,
              before.totals[1].bytes);
    EXPECT_FALSE(moved.isReady());
    state->set_completed(ErrorCode::OK);
    EXPECT_TRUE(moved.isReady());
    EXPECT_EQ(moved.get(), ErrorCode::OK);
    EXPECT_EQ(moved.get(), ErrorCode::OK);
    const auto after = StorageIOMetric::Instance().Snapshot();
    EXPECT_EQ(after.totals[1].bytes - before.totals[1].bytes, 4096);
    EXPECT_EQ(after.totals[1].ops - before.totals[1].ops, 2);
}

TEST_F(TransferTaskTest, FailedAndFileTransfersDoNotBecomeSuccessfulDramReads) {
    const auto before = StorageIOMetric::Instance().Snapshot();
    auto failed = std::make_shared<MemcpyOperationState>();
    failed->ConfigureStorageIO(StorageIOKind::DRAM_READ, 4096, 1);
    failed->set_completed(ErrorCode::TRANSFER_FAIL);
    auto file = std::make_shared<FilereadOperationState>();
    file->set_completed(ErrorCode::OK);
    const auto after = StorageIOMetric::Instance().Snapshot();
    EXPECT_EQ(after.totals[0].bytes, before.totals[0].bytes);
    EXPECT_EQ(after.totals[0].ops, before.totals[0].ops);
    EXPECT_EQ(after.totals[0].errors - before.totals[0].errors, 1);
}

}  // namespace mooncake

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
