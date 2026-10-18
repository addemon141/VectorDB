#pragma once

#include "raft.pb.h"

#include <cstdint>
#include <string>
#include <vector>

namespace vdb::raft {

// 持久化：hard_state.pb + log.bin + snapshot_meta.pb（裁剪前缀后）
// entries_ 对应下标从 first_index_ 起的连续日志；first_index_ 之前由 SnapshotMeta 描述。
class RaftLog {
public:
    explicit RaftLog(std::string dir);
    ~RaftLog();

    RaftLog(const RaftLog&) = delete;
    RaftLog& operator=(const RaftLog&) = delete;

    bool load(std::string* err);

    int64_t current_term() const { return hard_.current_term(); }
    int32_t voted_for() const { return hard_.voted_for(); }
    bool save_hard_state(int64_t term, int32_t voted_for);

    int64_t first_index() const { return first_index_; }
    int64_t last_index() const;
    int64_t last_term() const;
    int64_t term_at(int64_t index) const;

    const vectordb::raft::SnapshotMeta& snapshot_meta() const { return snap_; }
    bool has_snapshot() const { return snap_.last_included_index() > 0; }

    // 取条目；要求 first_index_ <= index <= last_index() 且 index > snap.last_included
    const vectordb::raft::LogEntry& at(int64_t index) const;

    bool append(const std::vector<vectordb::raft::LogEntry>& entries);
    bool truncate_from(int64_t index);

    // 丢弃 index <= through 的日志，写入 SnapshotMeta；through 须 <= last_index()
    // applied：本次快照要保存的去重记录。空向量会清掉上一份，避免裁剪后仍引用旧 id。
    bool compact_through(int64_t through, const std::string& content_hash,
                         const std::vector<vectordb::raft::AppliedAdd>& applied = {});

    std::vector<vectordb::raft::LogEntry> slice(int64_t from, size_t max_n, size_t max_bytes) const;
    int64_t first_index_of_term(int64_t term) const;
    int64_t last_index_of_term(int64_t term) const;

private:
    bool rewrite_log_file(std::string* err);
    bool save_snapshot_meta();

    std::string dir_;
    std::string log_path_;
    std::string hard_path_;
    std::string snap_path_;
    int fd_ = -1;
    vectordb::raft::HardState hard_;
    vectordb::raft::SnapshotMeta snap_;
    int64_t first_index_ = 1;
    std::vector<vectordb::raft::LogEntry> entries_;
    std::vector<uint64_t> offsets_;
    uint64_t end_offset_ = 0;
};

}  // namespace vdb::raft
