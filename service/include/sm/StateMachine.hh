#pragma once

#include "raft/StateMachineApi.hh"
#include "sm/Rebuilder.hh"
#include "sm/Tokenizer.hh"
#include "sm/Types.hh"

#include <memory>
#include <shared_mutex>
#include <string>
#include <utility>
#include <vector>

namespace vdb {

class StateMachine final : public raft::StateMachineApi {
public:
    explicit StateMachine(const Tokenizer* tokenizer) : tok_(tokenizer) {}

    bool load_base(const std::string& dir, const std::string& content_hash, std::string* err);
    void reset_empty();

    raft::ApplyResult apply(const vectordb::raft::Command& cmd, int64_t log_index) override;
    void restore_compacted_adds(const vectordb::raft::SnapshotMeta& meta) override;

    void set_rebuilder(Rebuilder* r) { rebuilder_ = r; }

    int64_t base_version() const;
    bool ready() const;
    bool rebuilding() const;
    int64_t cutoff_index() const;
    std::string content_hash() const;
    // RebuildCmd apply 时的基座 hash；load_base 换上新快照后与 content_hash 不同
    std::string pre_rebuild_hash() const;
    std::string base_dir() const;

    // 导出 Rebuild 语料：基座保留文档 id 升序 + staging -1..-N → 新 id 1..K
    bool export_rebuild_corpus(std::string* corpus_json,
                               std::vector<std::pair<int32_t, int32_t>>* mapping,
                               std::string* err) const;

    struct ReadView {
        std::shared_lock<std::shared_mutex> lock;
        std::shared_ptr<const BaseIndex> base;
        const StagingArea* staging = nullptr;
        int64_t base_version = 0;
        bool ready = false;
        bool rebuilding = false;
    };
    ReadView read() const;

private:
    raft::ApplyResult apply_add(const vectordb::raft::AddBatch& cmd, int64_t log_index);
    raft::ApplyResult apply_delete(const vectordb::raft::DeleteCmd& cmd, int64_t log_index);
    // 调用方已持有 mu_。记下 cutoff 时刻的合并结果，供导出与 InstallBase 改写删除。
    bool capture_rebuild_frozen_locked(std::string* err);
    // 调用方已持有 mu_。去掉 log_index <= cutoff 的暂存项，并把此后对旧 id 的删除改记到新 id。
    void retain_post_cutoff_locked();
    raft::ApplyResult apply_rebuild(const vectordb::raft::RebuildCmd& cmd, int64_t log_index);
    raft::ApplyResult apply_install_base(const vectordb::raft::InstallBase& cmd);

    static bool load_base_index(const std::string& dir, BaseIndex* out, std::string* err);

    const Tokenizer* tok_;
    Rebuilder* rebuilder_ = nullptr;

    mutable std::shared_mutex mu_;
    std::shared_ptr<const BaseIndex> base_;
    StagingArea staging_;
    int64_t base_version_ = 0;
    bool ready_ = false;
    bool rebuilding_ = false;
    int64_t cutoff_index_ = 0;
    std::string content_hash_;
    std::string pre_rebuild_hash_;
    std::string frozen_corpus_;
    std::vector<std::pair<int32_t, int32_t>> frozen_mapping_;
    std::unordered_map<int32_t, int32_t> id_remap_;

    struct AppliedAddRec {
        std::vector<int32_t> ids;
        int64_t log_index = 0;
        uint64_t content_fp = 0;
    };
    // request_id → 第一次 apply 的结果。log_index <= cutoff 的项在裁剪时写入 SnapshotMeta。
    std::unordered_map<std::string, AppliedAddRec> applied_adds_;
};

}  // namespace vdb
