#pragma once

#include "raft.grpc.pb.h"
#include "raft/RaftLog.hh"
#include "raft/StateMachineApi.hh"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace vdb::raft {

struct PeerInfo {
    int32_t id = 0;        // 从 1 开始
    std::string address;   // 管理端口地址（RaftService 与 AdminService 共用）
};

struct RaftConfig {
    int32_t node_id = 0;
    std::vector<PeerInfo> peers;  // 含自己
    std::string dir;              // RaftLog 目录
    int election_timeout_min_ms = 150;
    int election_timeout_max_ms = 300;
    int heartbeat_ms = 50;
    int rpc_timeout_ms = 200;
    size_t max_batch_entries = 64;
    size_t max_batch_bytes = 1 << 20;
    size_t max_inflight_proposes = 32;  // 背压：同时等待 apply 的 propose 上限
};

enum class Role { Follower, PreCandidate, Candidate, Leader };

// propose() 的结果。code 为 OK 表示命令已提交并 apply，apply 本身的结论在 result 里
struct ProposeResult {
    vectordb::raft::ErrorCode code = vectordb::raft::OK;
    std::string message;
    std::string leader_hint;  // NOT_LEADER 时 Leader 的管理地址（未知则为空）
    ApplyResult result;
};

struct RaftStatus {
    int32_t node_id = 0;
    Role role = Role::Follower;
    int32_t leader_id = 0;
    std::string leader_addr;
    int64_t term = 0;
    int64_t commit_index = 0;
    int64_t last_applied = 0;
    int64_t last_log_index = 0;
};

// RaftNode = 一个节点上的共识核心。
//
// 线程（start() 创建，stop() 回收）：
//   ticker_       ticker_loop()     选举计时；超时则 start_election_locked()
//   peer_workers_ peer_loop(peer)   每个 peer 一个：Candidate 时发 RequestVote，Leader 时发 AppendEntries
//   applier_      apply_loop()      把 [last_applied+1, commit_index] 依次交给状态机
// 另外 gRPC 线程调用 handle_request_vote / handle_append_entries（来自其他节点）
// 和 propose（来自 AdminService）。
//
// 锁：所有成员由 mu_ 保护；RPC 调用和 StateMachineApi::apply 都在释放 mu_ 之后进行。
// 函数名带 _locked 后缀的，调用方必须已持有 mu_。
// 持久化失败（写日志 / HardState）直接终止进程：此时继续运行会破坏安全性。
class RaftNode {
public:
    RaftNode(RaftConfig cfg, StateMachineApi* sm);
    ~RaftNode();

    RaftNode(const RaftNode&) = delete;
    RaftNode& operator=(const RaftNode&) = delete;

    // 加载 RaftLog、为每个 peer 建 Stub、启动全部线程。
    // 输出：成功返回 true；影响：以 Follower 身份开始，commit_index=last_applied=0，
    //       之后随着 leader_commit 推进重放全部已提交日志
    bool start(std::string* err);

    // 停止全部线程；所有等待中的 propose 以 TIMEOUT 返回。可重复调用。
    void stop();

    // 提交一条命令并等待它被 apply。
    // 输出：非 Leader → NOT_LEADER（带 leader_hint）；超时或期间下台 → TIMEOUT（结果未知）；
    //       日志被新 Leader 覆盖 → NOT_LEADER（确定未生效）；否则 OK + 状态机结果
    // 影响：Leader 上追加一条日志并落盘，唤醒 peer 线程复制
    ProposeResult propose(const vectordb::raft::Command& cmd, std::chrono::milliseconds timeout);

    // RaftService 入口（由其他节点调用）
    vectordb::raft::RequestVoteReply handle_request_vote(const vectordb::raft::RequestVoteArgs& args);
    vectordb::raft::AppendEntriesReply handle_append_entries(
        const vectordb::raft::AppendEntriesArgs& args);

    RaftStatus status() const;
    bool is_leader() const;
    std::string leader_addr() const;  // 未知返回空

private:
    // 一个在等 apply 的 propose
    struct Waiter {
        int64_t term = 0;  // 追加时的任期；apply 时与该位置实际条目的任期比较
        std::shared_ptr<std::promise<ProposeResult>> promise;
    };

    // ---- 线程主循环 ----
    void ticker_loop();
    void peer_loop(int32_t peer);
    void apply_loop();

    // ---- 选举（先 PreVote，多数后再正式 RequestVote）----
    void start_prevote_locked();
    void start_election_locked();
    void send_request_vote_locked(int32_t peer, std::unique_lock<std::mutex>& lk, bool pre_vote);
    void on_vote_reply_locked(int32_t peer, int64_t sent_term, bool pre_vote,
                              const vectordb::raft::RequestVoteReply& reply);
    void become_leader_locked();
    void become_follower_locked(int64_t term);

    // ---- 复制 ----
    void send_append_entries_locked(int32_t peer, std::unique_lock<std::mutex>& lk);
    void on_append_reply_locked(int32_t peer, int64_t sent_term, int64_t sent_prev, int64_t sent_n,
                                const vectordb::raft::AppendEntriesReply& reply);
    void advance_commit_locked();

    // ---- 工具 ----
    int64_t append_local_locked(const std::string& command_bytes);  // 返回新条目 index
    void persist_hard_state_locked(int64_t term, int32_t voted_for);
    void reset_election_deadline_locked();
    void complete_waiter_locked(int64_t index, int64_t term, const ApplyResult& result);
    void fail_all_waiters_locked(vectordb::raft::ErrorCode code, const std::string& msg);
    std::string address_of(int32_t id) const;
    int majority() const { return static_cast<int>(cfg_.peers.size()) / 2 + 1; }

    const RaftConfig cfg_;
    StateMachineApi* const sm_;

    mutable std::mutex mu_;
    std::condition_variable tick_cv_;   // 唤醒 ticker：角色变化、停止
    std::condition_variable peer_cv_;   // 唤醒 peer 线程：新日志、角色变化、commit 推进、停止
    std::condition_variable apply_cv_;  // 唤醒 applier：commit_index 推进、停止
    bool stopped_ = false;
    bool started_ = false;

    // 持久化状态（current_term / voted_for / 日志都在 log_ 里）
    RaftLog log_;

    // 易失状态
    Role role_ = Role::Follower;
    int32_t leader_id_ = 0;
    int64_t commit_index_ = 0;
    int64_t last_applied_ = 0;
    std::chrono::steady_clock::time_point election_deadline_;
    std::mt19937 rng_;

    // Candidate
    std::set<int32_t> votes_;
    std::unordered_map<int32_t, int64_t> vote_requested_term_;  // 本任期是否已向该 peer 要过票

    // Leader（每个 peer 一份）
    std::unordered_map<int32_t, int64_t> next_index_;
    std::unordered_map<int32_t, int64_t> match_index_;
    std::unordered_map<int32_t, std::chrono::steady_clock::time_point> last_sent_;
    std::unordered_map<int32_t, std::chrono::steady_clock::time_point> retry_after_;  // RPC 失败后的退避
    std::unordered_map<int32_t, int64_t> sent_commit_;  // 上次发给该 peer 的 leader_commit

    std::map<int64_t, Waiter> waiters_;  // index → 等待者

    std::unordered_map<int32_t, std::unique_ptr<vectordb::raft::RaftService::Stub>> stubs_;
    std::thread ticker_;
    std::thread applier_;
    std::vector<std::thread> peer_workers_;
};

const char* role_name(Role r);

}  // namespace vdb::raft
