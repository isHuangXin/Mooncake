// ThreadPool.h
#pragma once

#include <vector>
#include <queue>
#include <functional>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <future>
namespace mooncake {
/**
 * @class ThreadPool
 * @brief A thread pool implementation for concurrent task execution.
 *
 * Manages a fixed number of worker threads that process tasks from a queue.
 * Supports task enqueueing and graceful shutdown.
 */
class ThreadPool {
   public:
    /// Constructs a thread pool with specified number of worker threads
    explicit ThreadPool(size_t num_threads);

    /// Destructor (stops all threads and completes pending tasks)
    ~ThreadPool();

    /**
     * @brief Enqueues a task for execution
     * @tparam F Callable type
     * @tparam Args Argument types
     * @param f Callable object to execute
     * @param args Arguments to forward to the callable
     * @throws std::runtime_error if enqueued after stop
     */
    template <class F, class... Args>
    void enqueue(F&& f, Args&&... args);

    // MOONCAKE_SSD_OPT: submit() returns std::future so the caller can wait
    // for task completion and retrieve the result. Used by OffloadObjects()
    // to dispatch parallel bucket prepare work and collect results.
    template <class F, class... Args>
    auto submit(F&& f, Args&&... args)
        -> std::future<std::invoke_result_t<F, Args...>>;

    /// Stops the thread pool (waits for current tasks to complete)
    void stop();

   private:
    std::vector<std::thread> workers;         ///< Worker thread pool
    std::queue<std::function<void()>> tasks;  ///< Task queue

    std::mutex queue_mutex;             ///< Protects task queue access
    std::condition_variable condition;  ///< Synchronizes task assignment
    std::atomic<bool> stop_flag;        ///< Termination signal
};

/**
 * @brief Enqueues a task by wrapping it in a void() function object
 * @details Locks the queue, checks stop condition, and notifies a worker
 */
template <class F, class... Args>
void ThreadPool::enqueue(F&& f, Args&&... args) {
    auto task = std::make_shared<std::function<void()>>(
        [f = std::forward<F>(f),
         ... args = std::forward<Args>(args)]() mutable {
            std::invoke(f, args...);
        });
    {
        std::unique_lock<std::mutex> lock(queue_mutex);
        if (stop_flag) {
            throw std::runtime_error("enqueue on stopped ThreadPool");
        }
        tasks.emplace([task] { (*task)(); });
    }
    condition.notify_one();  ///< Wake one waiting worker
}

/**
 * @brief Submits a task and returns a std::future for its result.
 * @details Unlike enqueue(), the caller can wait on the returned future to
 *          retrieve the task's return value. Used by OffloadObjects() to
 *          dispatch parallel CPU work (BuildBucket + PrepareWriteBucket) and
 *          collect results before the io_uring batch-write phase.
 */
template <class F, class... Args>
auto ThreadPool::submit(F&& f, Args&&... args)
    -> std::future<std::invoke_result_t<F, Args...>> {
    using return_type = std::invoke_result_t<F, Args...>;
    auto task = std::make_shared<std::packaged_task<return_type()>>(
        std::bind(std::forward<F>(f), std::forward<Args>(args)...));
    std::future<return_type> result = task->get_future();
    {
        std::unique_lock<std::mutex> lock(queue_mutex);
        if (stop_flag) {
            throw std::runtime_error("submit on stopped ThreadPool");
        }
        tasks.emplace([task]() { (*task)(); });
    }
    condition.notify_one();
    return result;
}
}  // namespace mooncake
