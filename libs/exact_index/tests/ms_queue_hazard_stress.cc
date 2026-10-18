// Correctness stress for educational::MsQueue + hazard pointers.
// Checks: every enqueued value is dequeued exactly once (no loss / no dup).

#include "MSQueueHazard.hh"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

namespace {

constexpr int kProducers = 4;
constexpr int kConsumers = 4;
constexpr int kPerProducer = 50000;
constexpr uint64_t kTotal = static_cast<uint64_t>(kProducers) * kPerProducer;

void fail(const char* msg) {
    std::fprintf(stderr, "FAIL: %s\n", msg);
    std::exit(1);
}

}  // namespace

int main() {
    using exact_index::educational::MsQueue;

    MsQueue<uint64_t> q;
    std::atomic<int> producers_done{0};
    std::atomic<uint64_t> dequeued{0};
    // seen[v] == 1 iff value v was dequeued (values are 0 .. kTotal-1).
    std::vector<std::atomic<uint8_t>> seen(static_cast<size_t>(kTotal));
    for (auto& s : seen) {
        s.store(0, std::memory_order_relaxed);
    }

    std::vector<std::thread> threads;
    threads.reserve(static_cast<size_t>(kProducers + kConsumers));

    for (int p = 0; p < kProducers; ++p) {
        threads.emplace_back([&q, &producers_done, p] {
            const uint64_t base = static_cast<uint64_t>(p) * kPerProducer;
            for (int i = 0; i < kPerProducer; ++i) {
                q.enqueue(base + static_cast<uint64_t>(i));
            }
            producers_done.fetch_add(1, std::memory_order_acq_rel);
        });
    }

    for (int c = 0; c < kConsumers; ++c) {
        threads.emplace_back([&] {
            uint64_t v = 0;
            for (;;) {
                if (q.try_dequeue(v)) {
                    if (v >= kTotal) {
                        fail("dequeued value out of range");
                    }
                    uint8_t prev = seen[static_cast<size_t>(v)].exchange(
                        1, std::memory_order_acq_rel);
                    if (prev != 0) {
                        fail("duplicate dequeue");
                    }
                    dequeued.fetch_add(1, std::memory_order_acq_rel);
                } else if (producers_done.load(std::memory_order_acquire) ==
                               kProducers &&
                           dequeued.load(std::memory_order_acquire) == kTotal) {
                    break;
                } else {
                    std::this_thread::yield();
                }
            }
        });
    }

    const auto t0 = std::chrono::steady_clock::now();
    for (auto& t : threads) {
        t.join();
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();

    if (dequeued.load() != kTotal) {
        fail("lost elements");
    }
    for (uint64_t i = 0; i < kTotal; ++i) {
        if (seen[static_cast<size_t>(i)].load() != 1) {
            fail("missing element");
        }
    }

    // Drain should be empty.
    uint64_t junk = 0;
    if (q.try_dequeue(junk)) {
        fail("queue not empty after test");
    }

    std::printf(
        "ms_queue_hazard_stress OK producers=%d consumers=%d total=%llu ms=%lld\n",
        kProducers, kConsumers, static_cast<unsigned long long>(kTotal),
        static_cast<long long>(ms));
    return 0;
}
