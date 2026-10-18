// Stress test for ThreadPool shutdown and token accounting.
// Each round mixes main-thread submits, submits from inside tasks and concurrent external
// producers; the last task signals completion (JobGroup -> promise) and the pool is
// destroyed right after, mirroring build_exact_index_mt.

#include "JobGroup.hh"
#include "ThreadPool.hh"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int kWorkers = 4;
constexpr int kMainTasks = 64;
constexpr int kNestedTasks = kMainTasks / 2;
constexpr int kProducers = 4;
constexpr int kProducerTasks = 64;
constexpr int kTotalTasks = kMainTasks + kNestedTasks + kProducers * kProducerTasks;

std::atomic<int> g_progress{0};

bool run_round() {
    std::atomic<int> executed{0};
    std::promise<void> all_done;
    auto all_done_future = all_done.get_future();
    auto group =
        std::make_shared<exact_index::JobGroup>(kTotalTasks, [&all_done] { all_done.set_value(); });
    std::vector<std::thread> producers;

    {
        exact_index::ThreadPool pool(kWorkers);
        auto leaf = [&executed, group] {
            executed.fetch_add(1, std::memory_order_relaxed);
            group->done();
        };

        for (int p = 0; p < kProducers; ++p) {
            producers.emplace_back([&pool, leaf] {
                for (int i = 0; i < kProducerTasks; ++i) {
                    pool.submit(leaf);
                }
            });
        }
        for (int i = 0; i < kMainTasks; ++i) {
            if (i % 2 == 0) {
                pool.submit([&pool, leaf] {
                    pool.submit(leaf);
                    leaf();
                });
            } else {
                pool.submit(leaf);
            }
        }

        all_done_future.wait();
        for (auto& t : producers) {
            t.join();
        }
    }

    const int got = executed.load();
    if (got != kTotalTasks) {
        std::fprintf(stderr, "executed %d tasks, expected %d\n", got, kTotalTasks);
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    const int rounds = argc > 1 ? std::atoi(argv[1]) : 2000;

    std::atomic<bool> finished{false};
    std::thread watchdog([&finished] {
        int last = -1;
        while (!finished.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(10));
            const int now = g_progress.load();
            if (!finished.load() && now == last) {
                std::fprintf(stderr, "no progress for 10s at round %d: pool hung\n", now);
                std::_Exit(1);
            }
            last = now;
        }
    });

    for (int r = 0; r < rounds; ++r) {
        if (!run_round()) {
            std::_Exit(1);
        }
        g_progress.store(r + 1);
    }
    finished.store(true);
    watchdog.join();
    std::printf("thread_pool_stress OK rounds=%d tasks_per_round=%d\n", rounds, kTotalTasks);
    return 0;
}
