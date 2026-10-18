#include "sm/StateMachine.hh"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <iostream>
#include <mutex>

namespace vdb {
namespace {

uint64_t fnv1a64(const std::string& s, uint64_t h) {
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

// 只含标题、链接、正文。embedding 不参与，避免同一请求因向量服务抖动被当成另一篇。
uint64_t add_content_fp(const vectordb::raft::AddBatch& cmd) {
    uint64_t h = 14695981039346656037ull;
    h = fnv1a64(std::to_string(cmd.docs_size()), h);
    for (const auto& d : cmd.docs()) {
        h = fnv1a64(d.title(), h);
        h ^= 0x1f;
        h = fnv1a64(d.link(), h);
        h ^= 0x1f;
        h = fnv1a64(d.content(), h);
        h ^= 0x1f;
    }
    return h;
}

}  // namespace

using raft::ApplyResult;
using json = nlohmann::json;

bool StateMachine::load_base_index(const std::string& dir, BaseIndex* out, std::string* err) {
    out->dir = dir;
    if (!out->docs.load(dir)) {
        *err = "DocStore load failed: " + dir;
        return false;
    }
    if (!out->bm25.load(dir)) {
        *err = "BM25 load failed: " + dir;
        return false;
    }
    if (!out->hnsw.load(dir)) {
        std::cerr << "[sm] no HNSW index in " << dir << ", base vector route disabled\n";
    }
    return true;
}

bool StateMachine::load_base(const std::string& dir, const std::string& content_hash,
                             std::string* err) {
    auto base = std::make_shared<BaseIndex>();
    if (!load_base_index(dir, base.get(), err)) {
        return false;
    }
    std::unique_lock<std::shared_mutex> lk(mu_);
    base_ = std::move(base);
    if (!rebuilding_) {
        staging_ = StagingArea{};
    } else {
        // 新基座已包含 cutoff 及之前的文档，避免和仍留在 staging 里的副本叠在一起
        retain_post_cutoff_locked();
    }
    content_hash_ = content_hash;
    return true;
}

void StateMachine::reset_empty() {
    std::unique_lock<std::shared_mutex> lk(mu_);
    base_ = nullptr;
    staging_ = StagingArea{};
    base_version_ = 0;
    ready_ = false;
    rebuilding_ = false;
    cutoff_index_ = 0;
    content_hash_.clear();
    pre_rebuild_hash_.clear();
    frozen_corpus_.clear();
    frozen_mapping_.clear();
    id_remap_.clear();
    applied_adds_.clear();
}

int64_t StateMachine::base_version() const {
    std::shared_lock<std::shared_mutex> lk(mu_);
    return base_version_;
}

bool StateMachine::ready() const {
    std::shared_lock<std::shared_mutex> lk(mu_);
    return ready_;
}

bool StateMachine::rebuilding() const {
    std::shared_lock<std::shared_mutex> lk(mu_);
    return rebuilding_;
}

int64_t StateMachine::cutoff_index() const {
    std::shared_lock<std::shared_mutex> lk(mu_);
    return cutoff_index_;
}

std::string StateMachine::content_hash() const {
    std::shared_lock<std::shared_mutex> lk(mu_);
    return content_hash_;
}

std::string StateMachine::pre_rebuild_hash() const {
    std::shared_lock<std::shared_mutex> lk(mu_);
    return pre_rebuild_hash_;
}

std::string StateMachine::base_dir() const {
    std::shared_lock<std::shared_mutex> lk(mu_);
    return base_ ? base_->dir : std::string{};
}

StateMachine::ReadView StateMachine::read() const {
    ReadView v{std::shared_lock<std::shared_mutex>(mu_), nullptr, nullptr, 0, false, false};
    v.base = base_;
    v.staging = &staging_;
    v.base_version = base_version_;
    v.ready = ready_;
    v.rebuilding = rebuilding_;
    return v;
}

bool StateMachine::export_rebuild_corpus(std::string* corpus_json,
                                         std::vector<std::pair<int32_t, int32_t>>* mapping,
                                         std::string* err) const {
    const auto view = read();
    if (!view.ready || !view.base || !view.staging) {
        *err = "not ready";
        return false;
    }
    if (!frozen_corpus_.empty()) {
        *corpus_json = frozen_corpus_;
        *mapping = frozen_mapping_;
        return true;
    }
    mapping->clear();
    json arr = json::array();
    int32_t next_id = 1;

    auto all = view.base->docs.list_all();
    std::sort(all.begin(), all.end(),
              [](const StoredDoc& a, const StoredDoc& b) { return a.id < b.id; });
    for (const auto& d : all) {
        if (view.staging->base_tombstones.count(d.id)) {
            continue;
        }
        mapping->emplace_back(d.id, next_id);
        arr.push_back(
            {{"title", d.title}, {"link", d.link}, {"content", d.content}, {"id", next_id}});
        ++next_id;
    }
    // staging: map is ordered by key (-1, -2, ...) — plan says -1..-N which is insertion order
    // by next_seq; map iterates ascending keys so -N ... -1. Plan: "staging_id 从 -1 到 -N"
    // means process in order of absolute value 1..N i.e. -1 first.
    std::vector<int32_t> st_ids;
    for (const auto& [id, _] : view.staging->docs) {
        st_ids.push_back(id);
    }
    std::sort(st_ids.begin(), st_ids.end(),
              [](int32_t a, int32_t b) { return -a < -b; });  // -1, -2, ...
    for (int32_t id : st_ids) {
        const auto& d = view.staging->docs.at(id);
        mapping->emplace_back(id, next_id);
        arr.push_back(
            {{"title", d.title}, {"link", d.link}, {"content", d.content}, {"id", next_id}});
        ++next_id;
    }
    if (arr.empty()) {
        *err = "empty corpus after merge";
        return false;
    }
    *corpus_json = arr.dump();
    return true;
}

void StateMachine::restore_compacted_adds(const vectordb::raft::SnapshotMeta& meta) {
    std::unique_lock<std::shared_mutex> lk(mu_);
    for (const auto& a : meta.applied_adds()) {
        if (a.request_id().empty()) {
            continue;
        }
        AppliedAddRec rec;
        rec.ids.assign(a.doc_ids().begin(), a.doc_ids().end());
        rec.log_index = a.log_index();
        rec.content_fp = a.content_fp();
        applied_adds_[a.request_id()] = std::move(rec);
    }
}

ApplyResult StateMachine::apply(const vectordb::raft::Command& cmd, int64_t log_index) {
    switch (cmd.body_case()) {
        case vectordb::raft::Command::kAdd:
            return apply_add(cmd.add(), log_index);
        case vectordb::raft::Command::kDel:
            return apply_delete(cmd.del(), log_index);
        case vectordb::raft::Command::kRebuild:
            return apply_rebuild(cmd.rebuild(), log_index);
        case vectordb::raft::Command::kInstallBase:
            return apply_install_base(cmd.install_base());
        case vectordb::raft::Command::kNoop: {
            ApplyResult r;
            r.base_version = base_version();
            return r;
        }
        case vectordb::raft::Command::BODY_NOT_SET:
            break;
    }
    ApplyResult r;
    r.code = vectordb::raft::INTERNAL;
    r.message = "empty command";
    return r;
}

ApplyResult StateMachine::apply_install_base(const vectordb::raft::InstallBase& cmd) {
    ApplyResult r;
    std::unique_lock<std::shared_mutex> lk(mu_);
    r.base_version = base_version_;
    if (!base_) {
        r.code = vectordb::raft::INTERNAL;
        r.message = "base not loaded before InstallBase";
        return r;
    }
    if (cmd.content_hash().empty() || cmd.content_hash() != content_hash_) {
        r.code = vectordb::raft::INTERNAL;
        r.message = "content_hash mismatch (have=" + content_hash_ +
                    " want=" + cmd.content_hash() + ")";
        return r;
    }
    if (cmd.base_version() <= 0) {
        r.code = vectordb::raft::INVALID;
        r.message = "InstallBase.base_version must be > 0";
        return r;
    }
    base_version_ = cmd.base_version();
    ready_ = true;
    if (cmd.cutoff_index() > 0) {
        cutoff_index_ = cmd.cutoff_index();
    }
    if (cutoff_index_ > 0) {
        retain_post_cutoff_locked();
        for (auto& [_, rec] : applied_adds_) {
            if (rec.log_index > cutoff_index_) {
                continue;
            }
            for (int32_t& id : rec.ids) {
                const auto mit = id_remap_.find(id);
                if (mit != id_remap_.end()) {
                    id = mit->second;
                }
            }
        }
        for (const auto& [rid, rec] : applied_adds_) {
            if (rec.log_index > cutoff_index_) {
                continue;
            }
            auto& a = r.compacted_adds.emplace_back();
            a.set_request_id(rid);
            a.set_log_index(rec.log_index);
            a.set_content_fp(rec.content_fp);
            for (int32_t id : rec.ids) {
                a.add_doc_ids(id);
            }
        }
    } else {
        staging_ = StagingArea{};
        applied_adds_.clear();
    }
    rebuilding_ = false;
    pre_rebuild_hash_.clear();
    frozen_corpus_.clear();
    frozen_mapping_.clear();
    id_remap_.clear();
    r.base_version = base_version_;
    if (cmd.cutoff_index() > 0) {
        r.compact_through = cmd.cutoff_index();
    }
    return r;
}

ApplyResult StateMachine::apply_add(const vectordb::raft::AddBatch& cmd, int64_t log_index) {
    ApplyResult r;
    std::unique_lock<std::shared_mutex> lk(mu_);
    r.base_version = base_version_;
    if (!ready_) {
        r.code = vectordb::raft::NOT_READY;
        r.message = "cluster not ready";
        return r;
    }
    const uint64_t fp = add_content_fp(cmd);
    if (!cmd.request_id().empty()) {
        const auto it = applied_adds_.find(cmd.request_id());
        if (it != applied_adds_.end()) {
            if (it->second.content_fp != fp ||
                it->second.ids.size() != static_cast<size_t>(cmd.docs_size())) {
                r.code = vectordb::raft::INVALID;
                r.message = "request_id already applied with different docs";
                return r;
            }
            r.staging_ids = it->second.ids;
            r.idempotent = true;
            return r;
        }
    }
    lk.unlock();

    std::vector<StagingDoc> docs;
    docs.reserve(static_cast<size_t>(cmd.docs_size()));
    for (const auto& p : cmd.docs()) {
        StagingDoc d;
        d.title = p.title();
        d.link = p.link();
        d.content = p.content();
        d.embedding.assign(p.embedding().begin(), p.embedding().end());
        for (const auto& [term, c] : tok_->term_freq(d.content)) {
            d.tf.emplace(term, c);
            d.doc_len += c;
        }
        docs.push_back(std::move(d));
    }

    lk.lock();
    if (!ready_) {
        r.code = vectordb::raft::NOT_READY;
        r.message = "cluster not ready";
        return r;
    }
    for (auto& d : docs) {
        d.id = -(staging_.next_seq++);
        d.log_index = log_index;
        r.staging_ids.push_back(d.id);
        const int32_t id = d.id;
        staging_.docs.emplace(id, std::move(d));
    }
    if (!cmd.request_id().empty()) {
        applied_adds_[cmd.request_id()] = AppliedAddRec{r.staging_ids, log_index, fp};
    }
    r.base_version = base_version_;
    return r;
}

ApplyResult StateMachine::apply_delete(const vectordb::raft::DeleteCmd& cmd, int64_t log_index) {
    ApplyResult r;
    std::unique_lock<std::shared_mutex> lk(mu_);
    r.base_version = base_version_;
    if (!ready_) {
        r.code = vectordb::raft::NOT_READY;
        r.message = "cluster not ready";
        return r;
    }
    if (cmd.base_version() != base_version_) {
        r.code = vectordb::raft::STALE_VERSION;
        r.message = "base_version is " + std::to_string(base_version_);
        return r;
    }
    const int32_t id = cmd.id();
    if (staging_.base_tombstones.count(id)) {
        r.code = vectordb::raft::INVALID;
        r.message = "already deleted " + std::to_string(id);
        return r;
    }
    if (id < 0) {
        auto it = staging_.docs.find(id);
        if (it == staging_.docs.end()) {
            if (!(rebuilding_ && id_remap_.count(id))) {
                r.code = vectordb::raft::INVALID;
                r.message = "no staging doc " + std::to_string(id);
                return r;
            }
        } else if (!(rebuilding_ && it->second.log_index > 0 &&
                     it->second.log_index <= cutoff_index_)) {
            staging_.docs.erase(it);
            return r;
        } else {
            staging_.docs.erase(it);
        }
        staging_.base_tombstones[id] = log_index;
        return r;
    }
    const bool old_base_id = rebuilding_ && id_remap_.count(id);
    if (id == 0 || !base_ || (!old_base_id && !base_->docs.contains(id))) {
        r.code = vectordb::raft::INVALID;
        r.message = "no base doc " + std::to_string(id);
        return r;
    }
    staging_.base_tombstones[id] = log_index;
    return r;
}

ApplyResult StateMachine::apply_rebuild(const vectordb::raft::RebuildCmd& cmd, int64_t log_index) {
    ApplyResult r;
    std::unique_lock<std::shared_mutex> lk(mu_);
    r.base_version = base_version_;
    if (!ready_) {
        r.code = vectordb::raft::NOT_READY;
        r.message = "not ready";
        return r;
    }
    if (rebuilding_) {
        r.code = vectordb::raft::REBUILDING;
        r.message = "already rebuilding";
        return r;
    }
    if (cmd.base_version() != base_version_) {
        r.code = vectordb::raft::STALE_VERSION;
        r.message = "base_version is " + std::to_string(base_version_);
        return r;
    }
    std::string frozen_err;
    if (!capture_rebuild_frozen_locked(&frozen_err)) {
        r.code = vectordb::raft::INVALID;
        r.message = frozen_err;
        return r;
    }
    rebuilding_ = true;
    cutoff_index_ = log_index;
    pre_rebuild_hash_ = content_hash_;
    r.cutoff_index = log_index;
    (void)rebuilder_;
    return r;
}

bool StateMachine::capture_rebuild_frozen_locked(std::string* err) {
    if (!base_) {
        *err = "no base";
        return false;
    }
    json arr = json::array();
    frozen_mapping_.clear();
    id_remap_.clear();
    int32_t next_id = 1;
    auto all = base_->docs.list_all();
    std::sort(all.begin(), all.end(),
              [](const StoredDoc& a, const StoredDoc& b) { return a.id < b.id; });
    for (const auto& d : all) {
        if (staging_.base_tombstones.count(d.id)) {
            continue;
        }
        frozen_mapping_.emplace_back(d.id, next_id);
        id_remap_[d.id] = next_id;
        arr.push_back({{"title", d.title}, {"link", d.link}, {"content", d.content}, {"id", next_id}});
        ++next_id;
    }
    std::vector<int32_t> st_ids;
    for (const auto& [id, _] : staging_.docs) {
        st_ids.push_back(id);
    }
    std::sort(st_ids.begin(), st_ids.end(), [](int32_t a, int32_t b) { return -a < -b; });
    for (int32_t id : st_ids) {
        const auto& d = staging_.docs.at(id);
        frozen_mapping_.emplace_back(id, next_id);
        id_remap_[id] = next_id;
        arr.push_back(
            {{"title", d.title}, {"link", d.link}, {"content", d.content}, {"id", next_id}});
        ++next_id;
    }
    if (arr.empty()) {
        frozen_mapping_.clear();
        id_remap_.clear();
        *err = "empty corpus after merge";
        return false;
    }
    frozen_corpus_ = arr.dump();
    return true;
}

void StateMachine::retain_post_cutoff_locked() {
    if (cutoff_index_ <= 0) {
        return;
    }
    for (auto it = staging_.docs.begin(); it != staging_.docs.end();) {
        if (it->second.log_index <= cutoff_index_) {
            it = staging_.docs.erase(it);
        } else {
            ++it;
        }
    }
    std::unordered_map<int32_t, int64_t> kept;
    for (const auto& [id, idx] : staging_.base_tombstones) {
        if (idx <= cutoff_index_) {
            continue;
        }
        const auto mit = id_remap_.find(id);
        if (mit != id_remap_.end()) {
            kept[mit->second] = idx;
        } else if (id > 0) {
            kept[id] = idx;
        }
    }
    staging_.base_tombstones = std::move(kept);
}

}  // namespace vdb
