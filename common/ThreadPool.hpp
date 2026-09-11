#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace forgefs::common {

// Fixed-size pool of worker threads pulling tasks off a shared queue,
// guarded by a mutex/condition_variable rather than spinning or creating a
// thread per connection. Used by both the coordinator and storage node to
// serve multiple connections concurrently.
class ThreadPool {
public:
    explicit ThreadPool(size_t thread_count);
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    void Enqueue(std::function<void()> task);

private:
    void WorkerLoop();

    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_ = false;
};

}  // namespace forgefs::common
