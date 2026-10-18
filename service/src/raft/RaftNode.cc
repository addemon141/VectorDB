#include "raft/RaftNode.hh"

#include <grpcpp/grpcpp.h>

#include <algorithm>
#include <cstdlib>
#include <iostream>

namespace vdb::raft {

using std::chrono::milliseconds;
using std::chrono::steady_clock;
using vectordb::raft::AppendEntriesArgs;
using vectordb::raft::AppendEntriesReply;
using vectordb::raft::Command;
using vectordb::raft::LogEntry;
using vectordb::raft::RequestVoteArgs;
using vectordb::raft::RequestVoteReply;

namespace {

[[noreturn]] void fatal(const std::string& msg) {
    std::cerr << "[raft] FATAL: " << msg << std::endl;
    std::abort();
}

}  // namespace

const char* role_name(Role r) {
    switch (r) {
        case Role::Follower:
            return "follower";
        case Role::PreCandidate:
            return "pre_candidate";
        case Role::Candidate:
            return "candidate";
        case Role::Leader:
            return "leader";
    }
    return "?";
}

RaftNode::RaftNode(RaftConfig cfg, StateMachineApi* sm)
    : cfg_(std::move(cfg)),
      sm_(sm),
      log_(cfg_.dir),
      rng_(std::random_device{}() ^ static_cast<uint32_t>(cfg_.node_id)) {}

RaftNode::~RaftNode() { stop(); }

bool RaftNode::start(std::string* err) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (started_) {
            *err = "already started";
            return false;
        }
        if (address_of(cfg_.node_id).empty()) {
            *err = "node_id not in peers";
            return false;
        }
        if (!log_.load(err)) {
            return false;
        }
        // 已裁剪前缀：从 snapshot 之后重放
        last_applied_ = log_.snapshot_meta().last_included_index();
        commit_index_ = last_applied_;
        sm_->restore_compacted_adds(log_.snapshot_meta());
        for (const auto& p : cfg_.peers) {
            if (p.id == cfg_.node_id) {
                continue;
            }
            // 重连退避上限必须小于选举超时下限（默认最长 120s）：否则 peer 重启后，
            // Leader 的心跳在退避期间全部立即失败，新节点收不到心跳就会发起选举把 Leader 拉下台
            grpc::ChannelArguments args;
            args.SetInt(GRPC_ARG_INITIAL_RECONNECT_BACKOFF_MS, 50);
            args.SetInt(GRPC_ARG_MIN_RECONNECT_BACKOFF_MS, 50);
            args.SetInt(GRPC_ARG_MAX_RECONNECT_BACKOFF_MS, 100);
            stubs_[p.id] = vectordb::raft::RaftService::NewStub(
                grpc::CreateCustomChannel(p.address, grpc::InsecureChannelCredentials(), args));
        }
        reset_election_deadline_locked();
        started_ = true;
        std::cerr << "[raft n" << cfg_.node_id << "] start term=" << log_.current_term()
                  << " last_index=" << log_.last_index() << std::endl;
    }
    ticker_ = std::thread(&RaftNode::ticker_loop, this);
    applier_ = std::thread(&RaftNode::apply_loop, this);
    for (const auto& p : cfg_.peers) {
        if (p.id != cfg_.node_id) {
            peer_workers_.emplace_back(&RaftNode::peer_loop, this, p.id);
        }
    }
    return true;
}

void RaftNode::stop() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (stopped_) {
            return;
        }
        stopped_ = true;
        fail_all_waiters_locked(vectordb::raft::TIMEOUT, "node stopping; outcome unknown");
    }
    tick_cv_.notify_all();
    peer_cv_.notify_all();
    apply_cv_.notify_all();
    if (ticker_.joinable()) {
        ticker_.join();
    }
    if (applier_.joinable()) {
        applier_.join();
    }
    for (auto& t : peer_workers_) {
        if (t.joinable()) {
            t.join();
        }
    }
    peer_workers_.clear();
}

// ---------------------------------------------------------------------------
// 线程主循环
// ---------------------------------------------------------------------------

void RaftNode::ticker_loop() {
    std::unique_lock<std::mutex> lk(mu_);
    while (!stopped_) {
        if (role_ == Role::Leader) {
            tick_cv_.wait(lk, [&] { return stopped_ || role_ != Role::Leader; });
            continue;
        }
        // 心跳只往后推 election_deadline_，不 notify；醒来后按最新截止时间重新判断
        tick_cv_.wait_until(lk, election_deadline_);
        if (stopped_ || role_ == Role::Leader) {
            continue;
        }
        if (steady_clock::now() >= election_deadline_) {
            start_prevote_locked();
        }
    }
}

void RaftNode::peer_loop(int32_t peer) {
    std::unique_lock<std::mutex> lk(mu_);
    while (!stopped_) {
        const auto now = steady_clock::now();
        if (now < retry_after_[peer]) {
            peer_cv_.wait_until(lk, retry_after_[peer]);
            continue;
        }
        if (role_ == Role::PreCandidate || role_ == Role::Candidate) {
            const int64_t mark = (role_ == Role::PreCandidate) ? (log_.current_term() + 1)
                                                              : log_.current_term();
            if (vote_requested_term_[peer] != mark) {
                send_request_vote_locked(peer, lk, role_ == Role::PreCandidate);
            } else {
                peer_cv_.wait(lk);
            }
            continue;
        }
        if (role_ == Role::Leader) {
            const bool has_new = next_index_[peer] <= log_.last_index();
            const bool commit_behind = sent_commit_[peer] < commit_index_;
            const auto hb_due = last_sent_[peer] + milliseconds(cfg_.heartbeat_ms);
            if (has_new || commit_behind || now >= hb_due) {
                send_append_entries_locked(peer, lk);
            } else {
                peer_cv_.wait_until(lk, hb_due);
            }
            continue;
        }
        peer_cv_.wait(lk);
    }
}

void RaftNode::apply_loop() {
    std::unique_lock<std::mutex> lk(mu_);
    while (true) {
        apply_cv_.wait(lk, [&] { return stopped_ || last_applied_ < commit_index_; });
        if (stopped_) {
            return;
        }
        const int64_t index = last_applied_ + 1;
        if (index < log_.first_index()) {
            last_applied_ = log_.first_index() - 1;
            continue;
        }
        const LogEntry entry = log_.at(index);
        lk.unlock();

        ApplyResult result;
        Command cmd;
        if (!cmd.ParseFromString(entry.command())) {
            result.code = vectordb::raft::INTERNAL;
            result.message = "corrupt command at index " + std::to_string(index);
        } else {
            result = sm_->apply(cmd, index);
        }

        lk.lock();
        last_applied_ = index;
        if (result.code == vectordb::raft::OK && result.compact_through > 0) {
            std::string hash;
            Command c2;
            if (c2.ParseFromString(entry.command()) && c2.has_install_base()) {
                hash = c2.install_base().content_hash();
            }
            if (!log_.compact_through(result.compact_through, hash, result.compacted_adds)) {
                fatal("compact_through failed at " + std::to_string(result.compact_through));
            }
        }
        complete_waiter_locked(index, entry.term(), result);
    }
}

// ---------------------------------------------------------------------------
// 选举
// ---------------------------------------------------------------------------

void RaftNode::start_prevote_locked() {
    role_ = Role::PreCandidate;
    leader_id_ = 0;
    votes_ = {cfg_.node_id};
    vote_requested_term_.clear();
    reset_election_deadline_locked();
    std::cerr << "[raft n" << cfg_.node_id << "] prevote for term=" << (log_.current_term() + 1)
              << std::endl;
    if (static_cast<int>(votes_.size()) >= majority()) {
        start_election_locked();
        return;
    }
    peer_cv_.notify_all();
}

void RaftNode::start_election_locked() {
    const int64_t term = log_.current_term() + 1;
    persist_hard_state_locked(term, cfg_.node_id);
    role_ = Role::Candidate;
    leader_id_ = 0;
    votes_ = {cfg_.node_id};
    vote_requested_term_.clear();
    reset_election_deadline_locked();
    std::cerr << "[raft n" << cfg_.node_id << "] election term=" << term << std::endl;
    if (static_cast<int>(votes_.size()) >= majority()) {
        become_leader_locked();
        return;
    }
    peer_cv_.notify_all();
}

void RaftNode::send_request_vote_locked(int32_t peer, std::unique_lock<std::mutex>& lk,
                                        bool pre_vote) {
    const int64_t term = pre_vote ? (log_.current_term() + 1) : log_.current_term();
    vote_requested_term_[peer] = term;
    RequestVoteArgs args;
    args.set_term(term);
    args.set_candidate_id(cfg_.node_id);
    args.set_last_log_index(log_.last_index());
    args.set_last_log_term(log_.last_term());
    args.set_pre_vote(pre_vote);
    auto* stub = stubs_.at(peer).get();

    lk.unlock();
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + milliseconds(cfg_.rpc_timeout_ms));
    ctx.set_wait_for_ready(true);
    RequestVoteReply reply;
    const grpc::Status st = stub->RequestVote(&ctx, args, &reply);
    lk.lock();

    if (!st.ok()) {
        if (vote_requested_term_[peer] == term) {
            vote_requested_term_[peer] = 0;
        }
        retry_after_[peer] = steady_clock::now() + milliseconds(cfg_.heartbeat_ms);
        return;
    }
    on_vote_reply_locked(peer, term, pre_vote, reply);
}

void RaftNode::on_vote_reply_locked(int32_t peer, int64_t sent_term, bool pre_vote,
                                    const RequestVoteReply& reply) {
    if (!pre_vote && reply.term() > log_.current_term()) {
        become_follower_locked(reply.term());
        return;
    }
    if (pre_vote) {
        if (role_ != Role::PreCandidate || sent_term != log_.current_term() + 1) {
            return;
        }
        if (reply.term() > log_.current_term() + 1) {
            become_follower_locked(reply.term());
            return;
        }
        if (reply.vote_granted()) {
            votes_.insert(peer);
            if (static_cast<int>(votes_.size()) >= majority()) {
                start_election_locked();
            }
        }
        return;
    }
    if (role_ != Role::Candidate || log_.current_term() != sent_term) {
        return;
    }
    if (reply.vote_granted()) {
        votes_.insert(peer);
        if (static_cast<int>(votes_.size()) >= majority()) {
            become_leader_locked();
        }
    }
}

void RaftNode::become_leader_locked() {
    role_ = Role::Leader;
    leader_id_ = cfg_.node_id;
    votes_.clear();
    for (const auto& p : cfg_.peers) {
        if (p.id == cfg_.node_id) {
            continue;
        }
        next_index_[p.id] = log_.last_index() + 1;
        match_index_[p.id] = 0;
        last_sent_[p.id] = steady_clock::time_point{};  // 立即发一轮
        retry_after_[p.id] = steady_clock::time_point{};
        sent_commit_[p.id] = -1;
    }
    std::cerr << "[raft n" << cfg_.node_id << "] leader term=" << log_.current_term() << std::endl;

    // no-op：只有当前任期的条目能靠副本数提交，借它把此前任期遗留的条目一起提交
    Command noop;
    noop.mutable_noop();
    std::string bytes;
    noop.SerializeToString(&bytes);
    append_local_locked(bytes);
    advance_commit_locked();  // 单节点时立即提交

    tick_cv_.notify_all();
    peer_cv_.notify_all();
}

void RaftNode::become_follower_locked(int64_t term) {
    const bool was_leader = role_ == Role::Leader;
    const bool role_changed = role_ != Role::Follower;
    if (term > log_.current_term()) {
        persist_hard_state_locked(term, 0);
        leader_id_ = 0;
    }
    role_ = Role::Follower;
    if (was_leader) {
        // 下台前追加的条目仍可能被新 Leader 提交，所以结果未知
        fail_all_waiters_locked(vectordb::raft::TIMEOUT, "leadership lost; outcome unknown");
        std::cerr << "[raft n" << cfg_.node_id << "] step down term=" << log_.current_term()
                  << std::endl;
    }
    if (role_changed) {
        reset_election_deadline_locked();
        tick_cv_.notify_all();
        peer_cv_.notify_all();
    }
}

vectordb::raft::RequestVoteReply RaftNode::handle_request_vote(const RequestVoteArgs& args) {
    std::lock_guard<std::mutex> lk(mu_);
    RequestVoteReply reply;
    reply.set_vote_granted(false);
    reply.set_term(log_.current_term());

    if (args.pre_vote()) {
        // PreVote：不改 term / voted_for；仅判断「若进入该 term 是否会投票」
        if (args.term() < log_.current_term()) {
            return reply;
        }
        const bool up_to_date =
            args.last_log_term() > log_.last_term() ||
            (args.last_log_term() == log_.last_term() && args.last_log_index() >= log_.last_index());
        if (up_to_date) {
            reply.set_vote_granted(true);
        }
        reply.set_term(std::max(log_.current_term(), args.term()));
        return reply;
    }

    if (args.term() < log_.current_term()) {
        reply.set_term(log_.current_term());
        return reply;
    }
    if (args.term() > log_.current_term()) {
        become_follower_locked(args.term());
    }
    const bool up_to_date =
        args.last_log_term() > log_.last_term() ||
        (args.last_log_term() == log_.last_term() && args.last_log_index() >= log_.last_index());
    const int32_t voted = log_.voted_for();
    if ((voted == 0 || voted == args.candidate_id()) && up_to_date) {
        if (voted == 0) {
            persist_hard_state_locked(log_.current_term(), args.candidate_id());
        }
        reset_election_deadline_locked();
        reply.set_vote_granted(true);
    }
    reply.set_term(log_.current_term());
    return reply;
}

// ---------------------------------------------------------------------------
// 复制（心跳 = entries 为空的 AppendEntries）
// ---------------------------------------------------------------------------

void RaftNode::send_append_entries_locked(int32_t peer, std::unique_lock<std::mutex>& lk) {
    const int64_t term = log_.current_term();
    const int64_t next = next_index_[peer];
    const int64_t prev = next - 1;
    AppendEntriesArgs args;
    args.set_term(term);
    args.set_leader_id(cfg_.node_id);
    args.set_prev_log_index(prev);
    args.set_prev_log_term(log_.term_at(prev));
    args.set_leader_commit(commit_index_);
    for (auto& e : log_.slice(next, cfg_.max_batch_entries, cfg_.max_batch_bytes)) {
        *args.add_entries() = std::move(e);
    }
    const int64_t n = args.entries_size();
    last_sent_[peer] = steady_clock::now();
    sent_commit_[peer] = commit_index_;
    auto* stub = stubs_.at(peer).get();

    lk.unlock();
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + milliseconds(cfg_.rpc_timeout_ms));
    ctx.set_wait_for_ready(true);
    AppendEntriesReply reply;
    const grpc::Status st = stub->AppendEntries(&ctx, args, &reply);
    lk.lock();

    if (!st.ok()) {
        retry_after_[peer] = steady_clock::now() + milliseconds(cfg_.heartbeat_ms);
        return;
    }
    on_append_reply_locked(peer, term, prev, n, reply);
}

void RaftNode::on_append_reply_locked(int32_t peer, int64_t sent_term, int64_t sent_prev,
                                      int64_t sent_n, const AppendEntriesReply& reply) {
    if (reply.term() > log_.current_term()) {
        become_follower_locked(reply.term());
        return;
    }
    if (role_ != Role::Leader || log_.current_term() != sent_term) {
        return;  // 过期的回复：发出后本节点已换任期或下台
    }
    if (reply.success()) {
        match_index_[peer] = std::max(match_index_[peer], sent_prev + sent_n);
        next_index_[peer] = match_index_[peer] + 1;
        advance_commit_locked();
        return;
    }
    // 回退 next_index：Leader 有冲突任期就跳到该任期末尾之后，否则跳到 Follower 给的位置
    int64_t ni = reply.conflict_index();
    if (reply.conflict_term() > 0) {
        const int64_t last_of_term = log_.last_index_of_term(reply.conflict_term());
        if (last_of_term > 0) {
            ni = last_of_term + 1;
        }
    }
    if (ni >= next_index_[peer]) {
        ni = next_index_[peer] - 1;  // 保证每次失败至少回退一格
    }
    ni = std::max<int64_t>(ni, match_index_[peer] + 1);
    ni = std::min<int64_t>(ni, log_.last_index() + 1);
    next_index_[peer] = std::max<int64_t>(ni, 1);
}

void RaftNode::advance_commit_locked() {
    if (role_ != Role::Leader) {
        return;
    }
    std::vector<int64_t> matched{log_.last_index()};
    for (const auto& [_, m] : match_index_) {
        matched.push_back(m);
    }
    std::sort(matched.begin(), matched.end(), std::greater<int64_t>());
    // 降序第 majority 个：至少 majority 个节点（含自己）已拥有到该 index
    const int64_t n = matched[static_cast<size_t>(majority() - 1)];
    if (n > commit_index_ && log_.term_at(n) == log_.current_term()) {
        commit_index_ = n;
        apply_cv_.notify_one();
        peer_cv_.notify_all();  // 尽快把新的 leader_commit 带给 Follower
    }
}

vectordb::raft::AppendEntriesReply RaftNode::handle_append_entries(const AppendEntriesArgs& args) {
    std::lock_guard<std::mutex> lk(mu_);
    AppendEntriesReply reply;
    reply.set_success(false);
    if (args.term() < log_.current_term()) {
        reply.set_term(log_.current_term());
        return reply;
    }
    // 1-2. 来自当前合法 Leader：转为 Follower、记录 Leader、重置计时（一致性检查失败也要重置）
    if (args.term() > log_.current_term() || role_ != Role::Follower) {
        become_follower_locked(args.term());
    }
    leader_id_ = args.leader_id();
    reset_election_deadline_locked();
    reply.set_term(log_.current_term());

    // 3. 一致性检查（兼容日志前缀已裁剪）
    const int64_t prev = args.prev_log_index();
    if (prev > log_.last_index()) {
        reply.set_conflict_index(log_.last_index() + 1);
        reply.set_conflict_term(0);
        return reply;
    }
    if (prev > 0 && prev < log_.first_index() && prev != log_.snapshot_meta().last_included_index()) {
        reply.set_conflict_index(log_.first_index());
        reply.set_conflict_term(0);
        return reply;
    }
    if (prev > 0 && log_.term_at(prev) != args.prev_log_term()) {
        const int64_t ct = log_.term_at(prev);
        reply.set_conflict_term(ct);
        reply.set_conflict_index(std::max(log_.first_index_of_term(ct), log_.first_index()));
        return reply;
    }

    // 4. 逐条比较：已有且任期相同的跳过；遇到冲突才截断；之后的全部追加
    const int n = args.entries_size();
    int k = 0;
    for (; k < n; ++k) {
        const int64_t idx = prev + 1 + k;
        if (idx > log_.last_index()) {
            break;
        }
        if (log_.term_at(idx) != args.entries(k).term()) {
            if (idx <= commit_index_) {
                fatal("conflict at committed index " + std::to_string(idx));
            }
            if (!log_.truncate_from(idx)) {
                fatal("truncate log failed");
            }
            break;
        }
    }
    if (k < n) {
        std::vector<LogEntry> rest(args.entries().begin() + k, args.entries().end());
        if (!log_.append(rest)) {
            fatal("append log failed");
        }
    }

    // 5. 提交上界是本次确认过的最后一条，而不是本地日志末尾
    const int64_t verified = prev + n;
    const int64_t new_commit = std::min(args.leader_commit(), verified);
    if (new_commit > commit_index_) {
        commit_index_ = new_commit;
        apply_cv_.notify_one();
    }
    reply.set_success(true);
    return reply;
}

// ---------------------------------------------------------------------------
// propose 与 waiter
// ---------------------------------------------------------------------------

ProposeResult RaftNode::propose(const Command& cmd, milliseconds timeout) {
    std::future<ProposeResult> fut;
    int64_t index = 0;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (stopped_ || role_ != Role::Leader) {
            ProposeResult r;
            r.code = vectordb::raft::NOT_LEADER;
            r.message = "not leader";
            r.leader_hint = address_of(leader_id_);
            return r;
        }
        if (waiters_.size() >= cfg_.max_inflight_proposes) {
            ProposeResult r;
            r.code = vectordb::raft::BUSY;
            r.message = "too many inflight proposes";
            return r;
        }
        std::string bytes;
        if (!cmd.SerializeToString(&bytes)) {
            ProposeResult r;
            r.code = vectordb::raft::INTERNAL;
            r.message = "serialize command failed";
            return r;
        }
        index = append_local_locked(bytes);
        auto promise = std::make_shared<std::promise<ProposeResult>>();
        fut = promise->get_future();
        waiters_[index] = Waiter{log_.current_term(), std::move(promise)};
        advance_commit_locked();  // 单节点时立即提交
        peer_cv_.notify_all();
    }
    if (fut.wait_for(timeout) == std::future_status::ready) {
        return fut.get();
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (fut.wait_for(milliseconds(0)) != std::future_status::ready) {
            waiters_.erase(index);
            ProposeResult r;
            r.code = vectordb::raft::TIMEOUT;
            r.message = "commit timeout; outcome unknown";
            return r;
        }
    }
    return fut.get();
}

void RaftNode::complete_waiter_locked(int64_t index, int64_t term, const ApplyResult& result) {
    auto it = waiters_.find(index);
    if (it == waiters_.end()) {
        return;
    }
    ProposeResult r;
    if (it->second.term == term) {
        r.code = vectordb::raft::OK;
        r.result = result;
    } else {
        r.code = vectordb::raft::NOT_LEADER;
        r.message = "entry overwritten by a newer leader; not applied";
        r.leader_hint = address_of(leader_id_);
    }
    it->second.promise->set_value(std::move(r));
    waiters_.erase(it);
}

void RaftNode::fail_all_waiters_locked(vectordb::raft::ErrorCode code, const std::string& msg) {
    for (auto& [_, w] : waiters_) {
        ProposeResult r;
        r.code = code;
        r.message = msg;
        w.promise->set_value(std::move(r));
    }
    waiters_.clear();
}

// ---------------------------------------------------------------------------
// 工具
// ---------------------------------------------------------------------------

int64_t RaftNode::append_local_locked(const std::string& command_bytes) {
    LogEntry e;
    e.set_term(log_.current_term());
    e.set_index(log_.last_index() + 1);
    e.set_command(command_bytes);
    if (!log_.append({e})) {
        fatal("append log failed");
    }
    return e.index();
}

void RaftNode::persist_hard_state_locked(int64_t term, int32_t voted_for) {
    if (!log_.save_hard_state(term, voted_for)) {
        fatal("save hard state failed");
    }
}

void RaftNode::reset_election_deadline_locked() {
    std::uniform_int_distribution<int> dist(cfg_.election_timeout_min_ms,
                                            cfg_.election_timeout_max_ms);
    election_deadline_ = steady_clock::now() + milliseconds(dist(rng_));
}

std::string RaftNode::address_of(int32_t id) const {
    for (const auto& p : cfg_.peers) {
        if (p.id == id) {
            return p.address;
        }
    }
    return {};
}

RaftStatus RaftNode::status() const {
    std::lock_guard<std::mutex> lk(mu_);
    RaftStatus s;
    s.node_id = cfg_.node_id;
    s.role = role_;
    s.leader_id = leader_id_;
    s.leader_addr = address_of(leader_id_);
    s.term = log_.current_term();
    s.commit_index = commit_index_;
    s.last_applied = last_applied_;
    s.last_log_index = log_.last_index();
    return s;
}

bool RaftNode::is_leader() const {
    std::lock_guard<std::mutex> lk(mu_);
    return role_ == Role::Leader;
}

std::string RaftNode::leader_addr() const {
    std::lock_guard<std::mutex> lk(mu_);
    return address_of(leader_id_);
}

}  // namespace vdb::raft
