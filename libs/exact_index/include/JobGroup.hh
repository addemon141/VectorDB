#pragma once

#include <atomic>
#include <functional>
#include <utility>

namespace exact_index {

// Countdown latch: the thread that brings remaining to 0 runs on_complete exactly once.
class JobGroup {
public:
    using Callback = std::function<void()>;

    JobGroup() : remaining_(0) {}

    explicit JobGroup(int n, Callback on_complete = nullptr)
        : remaining_(n), on_complete_(std::move(on_complete)) {}

    void reset(int n, Callback on_complete = nullptr) {
        remaining_.store(n, std::memory_order_relaxed);
        on_complete_ = std::move(on_complete);
    }

    void done() {
        if (remaining_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            if (on_complete_) {
                on_complete_();
            }
        }
    }

    int remaining() const {
        return remaining_.load(std::memory_order_acquire);
    }

private:
    std::atomic<int> remaining_;
    Callback on_complete_;
};

}  // namespace exact_index
