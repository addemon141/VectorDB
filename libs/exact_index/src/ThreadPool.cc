#include "ThreadPool.hh"

#include <cassert>
#include <cerrno>
#include <chrono>
#include <stdexcept>
#include <thread>

namespace exact_index {

ThreadPool::ThreadPool(int worker_count, size_t queue_capacity)
    : queue_(queue_capacity) {
    if (worker_count <= 0) {
        worker_count = 1;
    }
    if (sem_init(&has_task_, 0, 0) != 0) {
        throw std::runtime_error("sem_init has_task_ failed");
    }
    workers_.reserve(static_cast<size_t>(worker_count));
    try {
        for (int i = 0; i < worker_count; ++i) {
            workers_.emplace_back([this] { worker_loop(); });
        }
    } catch (...) {
        // The destructor will not run; stop the workers already started, or destroying
        // their joinable std::thread objects would call std::terminate.
        stop_.store(true, std::memory_order_release);
        for (size_t i = 0; i < workers_.size(); ++i) {
            sem_post(&has_task_);
        }
        for (auto& w : workers_) {
            w.join();
        }
        sem_destroy(&has_task_);
        throw;
    }
}

ThreadPool::~ThreadPool() {
    // Precondition: no submit() once destruction starts. After draining, every task token
    // has been consumed, so the tokens posted below can only be stop tokens.
    wait_idle();
    stop_.store(true, std::memory_order_release);
    for (size_t i = 0; i < workers_.size(); ++i) {
        sem_post(&has_task_);
    }
    for (auto& w : workers_) {
        if (w.joinable()) {
            w.join();
        }
    }
    sem_destroy(&has_task_);
}

void ThreadPool::submit(Task task) {
    assert(!stop_.load(std::memory_order_acquire) && "submit() after destruction started");
    pending_.fetch_add(1, std::memory_order_acq_rel);
    for (;;) {
        Task tmp = std::move(task);
        if (queue_.try_enqueue(std::move(tmp))) {
            sem_post(&has_task_);
            return;
        }
        task = std::move(tmp);
        std::this_thread::yield();
    }
}

// One semaphore token per submitted task, plus one stop token per worker at shutdown.
void ThreadPool::worker_loop() {
    while (true) {
        while (sem_wait(&has_task_) != 0 && errno == EINTR) {
        }
        if (stop_.load(std::memory_order_acquire)) {
            break;
        }
        // The token guarantees an item; dequeue can still fail briefly while another
        // producer has claimed the head slot but not yet published it.
        Task task;
        while (!queue_.try_dequeue(task)) {
            std::this_thread::yield();
        }
        working_.fetch_add(1, std::memory_order_acq_rel);
        if (task) {
            task();
        }
        working_.fetch_sub(1, std::memory_order_acq_rel);
        pending_.fetch_sub(1, std::memory_order_acq_rel);
    }
}

void ThreadPool::wait_idle() {
    using namespace std::chrono_literals;
    while (pending_.load(std::memory_order_acquire) > 0 ||
           working_.load(std::memory_order_acquire) > 0) {
        std::this_thread::sleep_for(1ms);
    }
}

}  // namespace exact_index
