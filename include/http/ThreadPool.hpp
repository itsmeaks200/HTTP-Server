#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace http {

// Fixed-size pool of worker threads that pull tasks off a shared queue.
//
// Backpressure: if the pending queue grows beyond kMaxQueueDepth, Enqueue()
// returns false immediately and the caller should send a 503 and close the
// connection — this bounds how many connections can pile up waiting for a
// free worker even when all workers are busy with slow clients.
//
// Known tradeoff: each in-flight task (connection) may block its worker thread
// for up to kIdleTimeoutSeconds in recv().  With a bounded pool this directly
// caps maximum concurrent capacity to thread_count connections fully in use,
// with up to kMaxQueueDepth more waiting.  Accepted for this milestone.
class ThreadPool {
public:
    // thread_count: number of permanent worker threads to launch.
    explicit ThreadPool(std::size_t thread_count) {
        workers_.reserve(thread_count);
        for (std::size_t i = 0; i < thread_count; ++i) {
            workers_.emplace_back([this] { WorkerLoop(); });
        }
    }

    ~ThreadPool() { Shutdown(); }

    // Not copyable or movable — holds live threads.
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // Add a task to the queue.  Returns false (and does NOT enqueue) if the
    // queue is already at capacity — the caller must handle the rejection
    // (e.g. send 503 and close the fd) before calling Enqueue again.
    bool Enqueue(std::function<void()> task) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (stop_ || queue_.size() >= kMaxQueueDepth) {
                return false;
            }
            queue_.push(std::move(task));
        }
        cond_.notify_one();
        return true;
    }

    // Signal all workers to finish their current task then exit, then join.
    // Safe to call more than once (idempotent after the first call).
    void Shutdown() {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (stop_) {
                return;
            }
            stop_ = true;
        }
        cond_.notify_all();
        for (auto& t : workers_) {
            if (t.joinable()) {
                t.join();
            }
        }
    }

private:
    // How many pending (not yet started) connections we will queue up before
    // refusing new ones with 503.  Beyond this the OS listen backlog itself
    // is the buffer — we prefer a fast 503 to unbounded memory growth.
    static constexpr std::size_t kMaxQueueDepth = 256;

    void WorkerLoop() {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                // Wait until there is work to do or we have been told to stop.
                cond_.wait(lock, [this] { return stop_ || !queue_.empty(); });
                if (stop_ && queue_.empty()) {
                    return;
                }
                task = std::move(queue_.front());
                queue_.pop();
            }
            task();
        }
    }

    std::mutex mutex_;
    std::condition_variable cond_;
    std::queue<std::function<void()>> queue_;
    std::vector<std::thread> workers_;
    bool stop_ = false;
};

}  // namespace http
