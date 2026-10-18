#pragma once

#include <atomic>
#include <cstddef>
#include <new>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

namespace exact_index {

// Bounded MPMC ring buffer (Dmitry Vyukov style).
template <typename T>
class MpmcQueue {
public:
    explicit MpmcQueue(size_t capacity_pow2)
        : capacity_(next_pow2(capacity_pow2 < 2 ? 2 : capacity_pow2)),
          mask_(capacity_ - 1),
          buffer_(capacity_) {
        for (size_t i = 0; i < capacity_; ++i) {
            buffer_[i].sequence.store(i, std::memory_order_relaxed);
        }
        enqueue_pos_.store(0, std::memory_order_relaxed);
        dequeue_pos_.store(0, std::memory_order_relaxed);
    }

    bool try_enqueue(T&& item) {
        Cell* cell = nullptr;
        size_t pos = enqueue_pos_.load(std::memory_order_relaxed);
        for (;;) {
            cell = &buffer_[pos & mask_];
            size_t seq = cell->sequence.load(std::memory_order_acquire);
            intptr_t dif = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);
            if (dif == 0) {
                if (enqueue_pos_.compare_exchange_weak(
                        pos, pos + 1, std::memory_order_relaxed)) {
                    break;
                }
            } else if (dif < 0) {
                return false;
            } else {
                pos = enqueue_pos_.load(std::memory_order_relaxed);
            }
        }
        cell->data = std::move(item);
        cell->sequence.store(pos + 1, std::memory_order_release);
        return true;
    }
    
    //在enqueue_pos位置：
    //先试图获取到enqueue_pos处的槽位。
    //然后把该槽位中的seq加载出来,将该槽位的<seq>与之前自己取到的pos做对比，如果相等就说明这个槽位仍为空，

    bool try_dequeue(T& item) {
        Cell* cell = nullptr;
        size_t pos = dequeue_pos_.load(std::memory_order_relaxed);
        for (;;) {
            cell = &buffer_[pos & mask_];
            size_t seq = cell->sequence.load(std::memory_order_acquire);
            intptr_t dif = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);
            if (dif == 0) {
                if (dequeue_pos_.compare_exchange_weak(
                        pos, pos + 1, std::memory_order_relaxed)) {
                    break;
                }
            } else if (dif < 0) {
                return false;
            } else {
                pos = dequeue_pos_.load(std::memory_order_relaxed);
            }
        }
        item = std::move(cell->data);
        cell->sequence.store(pos + mask_ + 1, std::memory_order_release);
        return true;
    }

    size_t capacity() const { return capacity_; }

private:
    struct Cell {
        std::atomic<size_t> sequence;
        T data;
    };

    static size_t next_pow2(size_t v) {
        size_t p = 1;
        while (p < v) {
            p <<= 1;
        }
        return p;
    }

    const size_t capacity_;
    const size_t mask_;
    std::vector<Cell> buffer_;
    alignas(64) std::atomic<size_t> enqueue_pos_;
    alignas(64) std::atomic<size_t> dequeue_pos_;
};

}  // namespace exact_index
