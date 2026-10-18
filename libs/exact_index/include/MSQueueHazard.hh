#pragma once

// Educational Michael–Scott MPMC queue + Hazard Pointers.
// Nodes use new/delete (no lock-free pool). Not wired into production code.
//
// Michael & Scott, PODC 1996 — non-blocking concurrent queue.
// Michael, TPDS 2004 — hazard pointers for safe reclamation.

#include <atomic>
#include <cassert>
#include <cstddef>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace exact_index {
namespace educational {

inline constexpr size_t kHpPerThread = 2;
inline constexpr size_t kMaxHpThreads = 128;
// Low threshold so the stress test actually exercises scan/reclaim.
inline constexpr size_t kRetireThreshold = 64;

// ---------------------------------------------------------------------------
// Hazard pointer domain
// ---------------------------------------------------------------------------
//
// Reader: before following a shared Node*, publish it into a hazard slot.
// Retirer: after unlinking a Node*, push it to a retire list; periodically
// scan every slot; free only nodes that appear in no slot.
//
// Teaching limits: fixed thread table, 2 slots/thread (enough for MSQueue).

struct HazardDomain {
    struct Record {
        std::atomic<void*> hazards[kHpPerThread]{};
        std::atomic<bool> active{false};
    };

    Record records[kMaxHpThreads]{};

    Record* acquire() {
        for (auto& r : records) {
            bool expected = false;
            if (r.active.compare_exchange_strong(expected, true,
                                                 std::memory_order_acq_rel)) {
                for (auto& h : r.hazards) {
                    h.store(nullptr, std::memory_order_relaxed);
                }
                return &r;
            }
        }
        return nullptr;
    }

    void release(Record* r) {
        if (!r) {
            return;
        }
        for (auto& h : r->hazards) {
            h.store(nullptr, std::memory_order_release);
        }
        r->active.store(false, std::memory_order_release);
    }

    void collect_protected(std::vector<void*>& out) const {
        out.clear();
        for (const auto& r : records) {
            if (!r.active.load(std::memory_order_acquire)) {
                continue;
            }
            for (const auto& h : r.hazards) {
                if (void* p = h.load(std::memory_order_acquire)) {
                    out.push_back(p);
                }
            }
        }
    }
};

inline HazardDomain& hp_domain() {
    static HazardDomain domain;
    return domain;
}

struct Retired {
    void* ptr = nullptr;
    void (*deleter)(void*) = nullptr;
};

// Nodes a dying thread could not free (still protected) land here; the queue
// destructor drains them once no concurrent users remain.
inline std::mutex& hp_orphan_mu() {
    static std::mutex mu;
    return mu;
}

inline std::vector<Retired>& hp_orphans() {
    static std::vector<Retired> orphans;
    return orphans;
}

inline void hp_scan_free(std::vector<Retired>& list) {
    std::vector<void*> protected_ptrs;
    hp_domain().collect_protected(protected_ptrs);

    std::vector<Retired> kept;
    kept.reserve(list.size());
    for (const Retired& r : list) {
        bool still_protected = false;
        for (void* h : protected_ptrs) {
            if (h == r.ptr) {
                still_protected = true;
                break;
            }
        }
        if (still_protected) {
            kept.push_back(r);
        } else if (r.deleter) {
            r.deleter(r.ptr);
        }
    }
    list.swap(kept);
}

// After all queue users have joined: no live HPs should reference queue nodes.
inline void hp_flush_orphans_unconditional() {
    std::lock_guard<std::mutex> lock(hp_orphan_mu());
    auto& orphans = hp_orphans();
    for (const Retired& r : orphans) {
        if (r.deleter) {
            r.deleter(r.ptr);
        }
    }
    orphans.clear();
}

struct HpThreadState {
    HazardDomain::Record* record = nullptr;
    std::vector<Retired> retire_list;

    HpThreadState() { record = hp_domain().acquire(); }

    ~HpThreadState() {
        if (record) {
            for (auto& h : record->hazards) {
                h.store(nullptr, std::memory_order_release);
            }
        }
        // Free what is no longer protected; ship the rest to the orphan list.
        hp_scan_free(retire_list);
        if (!retire_list.empty()) {
            std::lock_guard<std::mutex> lock(hp_orphan_mu());
            auto& orphans = hp_orphans();
            orphans.insert(orphans.end(), retire_list.begin(), retire_list.end());
            retire_list.clear();
        }
        if (record) {
            hp_domain().release(record);
            record = nullptr;
        }
    }

    void protect(size_t slot, void* p) {
        assert(record && "hazard record table full");
        record->hazards[slot].store(p, std::memory_order_release);
    }

    void clear_all() {
        assert(record && "hazard record table full");
        for (size_t i = 0; i < kHpPerThread; ++i) {
            record->hazards[i].store(nullptr, std::memory_order_release);
        }
    }

    void retire(void* ptr, void (*deleter)(void*)) {
        retire_list.push_back(Retired{ptr, deleter});
        if (retire_list.size() >= kRetireThreshold) {
            hp_scan_free(retire_list);
        }
    }
};

inline HpThreadState& hp_tls() {
    thread_local HpThreadState state;
    return state;
}

// ---------------------------------------------------------------------------
// Michael–Scott queue
// ---------------------------------------------------------------------------
//
// Layout: always one dummy node at head. Real items live in nodes after head.
//
//   head ──► [dummy|next] ──► [A|next] ──► [B|next] ──► null
//                              ^
//                             tail (may lag one step behind the true end)
//
// Enqueue claims the end with CAS(tail->next, null, node), then swings tail.
// Dequeue swings head from dummy to next, returns next->data, retires old head.
// Hazard pointers stop another thread from delete'ing a node we still walk.

template <typename T>
class MsQueue {
public:
    MsQueue() {
        Node* dummy = new Node{};
        head_.store(dummy, std::memory_order_relaxed);
        tail_.store(dummy, std::memory_order_relaxed);
    }

    ~MsQueue() {
        T junk;
        while (try_dequeue(junk)) {
        }
        // Single-threaded teardown from here.
        delete head_.load(std::memory_order_relaxed);
        hp_tls().clear_all();
        hp_scan_free(hp_tls().retire_list);
        hp_flush_orphans_unconditional();
    }

    MsQueue(const MsQueue&) = delete;
    MsQueue& operator=(const MsQueue&) = delete;

    void enqueue(T item) {
        Node* node = new Node{};
        node->data = std::move(item);

        auto& hp = hp_tls();
        for (;;) {
            Node* tail = tail_.load(std::memory_order_acquire);
            hp.protect(/*slot=*/0, tail);
            // Validate after publishing: classic HP "load / protect / reload".
            if (tail != tail_.load(std::memory_order_acquire)) {
                continue;
            }

            Node* next = tail->next.load(std::memory_order_acquire);
            if (tail != tail_.load(std::memory_order_acquire)) {
                continue;
            }

            if (next != nullptr) {
                // Concurrent enqueuer already linked a node but has not swung
                // tail yet — help it, then retry.
                tail_.compare_exchange_weak(tail, next, std::memory_order_acq_rel,
                                            std::memory_order_acquire);
                continue;
            }

            Node* expected = nullptr;
            if (tail->next.compare_exchange_weak(expected, node,
                                                 std::memory_order_release,
                                                 std::memory_order_relaxed)) {
                // Optional swing; if we fail, a helper or later enqueue fixes it.
                tail_.compare_exchange_strong(tail, node, std::memory_order_acq_rel,
                                              std::memory_order_acquire);
                hp.clear_all();
                return;
            }
        }
    }

    bool try_dequeue(T& out) {
        auto& hp = hp_tls();
        for (;;) {
            Node* head = head_.load(std::memory_order_acquire);
            hp.protect(/*slot=*/0, head);
            if (head != head_.load(std::memory_order_acquire)) {
                continue;
            }

            Node* tail = tail_.load(std::memory_order_acquire);
            Node* next = head->next.load(std::memory_order_acquire);
            hp.protect(/*slot=*/1, next);
            if (head != head_.load(std::memory_order_acquire)) {
                continue;
            }

            if (next == nullptr) {
                hp.clear_all();
                return false;  // empty
            }

            if (head == tail) {
                // next != null but head == tail: tail is lagging; help & retry.
                tail_.compare_exchange_weak(tail, next, std::memory_order_acq_rel,
                                            std::memory_order_acquire);
                continue;
            }

            // Swing head first. Only the winner may take next->data; after the
            // CAS, `next` is the new dummy and its data field is ours to move.
            if (head_.compare_exchange_weak(head, next, std::memory_order_acq_rel,
                                            std::memory_order_acquire)) {
                out = std::move(next->data);
                hp.clear_all();
                retire_node(head);
                return true;
            }
        }
    }

private:
    struct Node {
        std::atomic<Node*> next{nullptr};
        T data{};
    };

    static void delete_node(void* p) { delete static_cast<Node*>(p); }

    void retire_node(Node* node) { hp_tls().retire(node, &delete_node); }

    std::atomic<Node*> head_;
    std::atomic<Node*> tail_;
};

}  // namespace educational
}  // namespace exact_index
