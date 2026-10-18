#pragma once

#include "LockFreeQueue.hh"

#include <atomic>
#include <functional>
#include <semaphore.h>
#include <thread>
#include <vector>

namespace exact_index {

// Fixed-size worker pool over a bounded lock-free queue.
// Contract:
//  - Tasks must not throw; an escaping exception calls std::terminate.
//  - submit() must not be called once the destructor has started.
//  - When the queue is full, submit() spins until a worker frees a slot. If every worker
//    is itself blocked in submit() at that moment, nothing drains the queue: keep the
//    number of tasks submitted from inside tasks well below queue_capacity.
class ThreadPool {
public:
    using Task = std::function<void()>;

    explicit ThreadPool(int worker_count, size_t queue_capacity = 4096);
    // Drains all submitted tasks (including ones they submit) before stopping the workers.
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    void submit(Task task);
    // Returns once no task is queued or running.
    void wait_idle();

    int worker_count() const { return static_cast<int>(workers_.size()); }

private:
    void worker_loop();

    MpmcQueue<Task> queue_;
    sem_t has_task_;
    std::vector<std::thread> workers_;
    std::atomic<bool> stop_{false};
    std::atomic<int> pending_{0};
    std::atomic<int> working_{0};
};

}  // namespace exact_index
