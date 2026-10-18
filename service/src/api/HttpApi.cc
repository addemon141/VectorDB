#include "api/HttpApi.hh"

#include "tlv/hash_util.hh"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <unordered_set>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace vdb {
namespace {

const char* err_name(vectordb::raft::ErrorCode c) {
    switch (c) {
        case vectordb::raft::OK:
            return "OK";
        case vectordb::raft::NOT_LEADER:
            return "NOT_LEADER";
        case vectordb::raft::STALE_VERSION:
            return "STALE_VERSION";
        case vectordb::raft::INVALID:
            return "INVALID";
        case vectordb::raft::TIMEOUT:
            return "TIMEOUT";
        case vectordb::raft::INTERNAL:
            return "INTERNAL";
        case vectordb::raft::NOT_READY:
            return "NOT_READY";
        case vectordb::raft::BUSY:
            return "BUSY";
        case vectordb::raft::REBUILDING:
            return "REBUILDING";
        default:
            return "UNKNOWN";
    }
}

int http_for(vectordb::raft::ErrorCode c) {
    switch (c) {
        case vectordb::raft::OK:
            return 200;
        case vectordb::raft::NOT_LEADER:
            return 421;
        case vectordb::raft::NOT_READY:
        case vectordb::raft::BUSY:
        case vectordb::raft::REBUILDING:
            return 503;
        case vectordb::raft::STALE_VERSION:
        case vectordb::raft::INVALID:
            return 400;
        case vectordb::raft::TIMEOUT:
            return 504;
        default:
            return 500;
    }
}

bool query_has(const std::string& q, const std::string& key) {
    return q.find(key) != std::string::npos;
}

}  // namespace

class HttpServerHolder {
public:
    httplib::Server srv;
};

HttpApi::HttpApi(ServerConfig cfg, raft::RaftNode* raft, StateMachine* sm, QueryEngine* engine,
                 EmbedClient* embed)
    : cfg_(std::move(cfg)),
      raft_(raft),
      sm_(sm),
      engine_(engine),
      embed_(embed),
      dn_ha_(cfg_.datanode_addr, cfg_.datanode_backup),
      holder_(std::make_unique<HttpServerHolder>()) {
    jobs_.set_persist_dir(cfg_.data_dir + "/jobs");
    jobs_.load();
}

HttpApi::~HttpApi() { stop(); }

void HttpApi::stop() {
    stopping_ = true;
    if (holder_) {
        holder_->srv.stop();
    }
    for (auto& t : workers_) {
        if (t.joinable()) {
            t.join();
        }
    }
    workers_.clear();
}

std::string HttpApi::handle_status() {
    const auto s = raft_->status();
    return json{{"ready", sm_->ready()},
                {"rebuilding", sm_->rebuilding()},
                {"role", raft::role_name(s.role)},
                {"base_version", sm_->base_version()},
                {"cutoff_index", sm_->cutoff_index()},
                {"leader_id", s.leader_id},
                {"leader_addr", s.leader_addr},
                {"node_id", s.node_id},
                {"term", s.term},
                {"content_hash", sm_->content_hash()},
                {"datanode", dn_ha_.active_addr()},
                {"init_state", sm_->ready() ? "ready" : (sm_->base_dir().empty() ? "empty" : "loaded")}}
        .dump();
}

bool HttpApi::fetch_and_load(const std::string& datanode, const std::string& expect_hash,
                             std::string* err) {
    const std::string base_dir = cfg_.data_dir + "/base";
    DataNodeClient dn(datanode);
    SnapshotMeta meta;
    if (!dn.download_snapshot(base_dir, &meta, err)) {
        return false;
    }
    if (meta.content_hash != expect_hash) {
        *err = "downloaded hash mismatch";
        return false;
    }
    const std::string local_hash = tlv::snapshot_hash(base_dir, err);
    if (local_hash.empty() || local_hash != expect_hash) {
        *err = "local hash mismatch";
        return false;
    }
    {
        std::ofstream hf(base_dir + "/SNAPSHOT.hash");
        hf << expect_hash << "\n";
    }
    return sm_->load_base(base_dir, expect_hash, err);
}

bool HttpApi::notify_peer_fetch(const std::string& peer_http, const std::string& datanode,
                                const std::string& content_hash, std::string* err) {
    const auto colon = peer_http.rfind(':');
    if (colon == std::string::npos) {
        *err = "bad peer http " + peer_http;
        return false;
    }
    httplib::Client cli(peer_http.substr(0, colon), std::stoi(peer_http.substr(colon + 1)));
    cli.set_connection_timeout(5, 0);
    cli.set_read_timeout(cfg_.admin.fetch_snapshot_timeout_s, 0);
    const std::string payload =
        json{{"datanode", datanode}, {"content_hash", content_hash}}.dump();
    // Follower 可能尚未 apply RebuildCmd，409 时稍等再试。连不上则视为该节点已下线并跳过。
    int conn_fails = 0;
    for (int attempt = 0; attempt < 40; ++attempt) {
        const auto res = cli.Post("/internal/fetch_snapshot", payload, "application/json");
        if (res && res->status == 200) {
            return true;
        }
        if (!res) {
            if (++conn_fails >= 5) {
                std::cerr << "[http] skip unreachable peer " << peer_http << "\n";
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        *err = "peer fetch failed " + peer_http + " status=" + std::to_string(res->status) + " " +
               res->body;
        if (res->status != 409) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

bool HttpApi::run_snapshot_pipeline(const std::string& corpus_json, const std::string& job_id,
                                    int64_t install_version, int64_t cutoff_index,
                                    std::string* hash_out, std::string* err,
                                    const std::string& prepared_hash, bool hold_install) {
    auto try_once = [&](std::string* e) -> bool {
        auto dn = dn_ha_.client();
        std::string hash = prepared_hash;
        int64_t ver = install_version;

        // 查 DataNode STATUS：同 job_id 则从断点续传 / 跳过已完成阶段
        std::string dn_state, dn_hash, job_json;
        uint64_t resume_off = 0;
        bool skip_upload = !prepared_hash.empty();
        bool skip_build = !prepared_hash.empty();
        if (prepared_hash.empty() && dn.status(&dn_state, &dn_hash, e, &job_json)) {
            auto jj = json::parse(job_json, nullptr, false);
            const std::string dn_job = jj.is_discarded() ? "" : jj.value("job_id", "");
            if (!job_id.empty() && dn_job == job_id) {
                resume_off = jj.value("received_offset", 0ull);
                if (dn_state == "ready" && !dn_hash.empty()) {
                    hash = dn_hash;
                    skip_upload = true;
                    skip_build = true;
                } else if (dn_state == "received" || dn_state == "building") {
                    skip_upload = true;
                } else if (dn_state == "receiving" && resume_off > 0 &&
                           resume_off < corpus_json.size()) {
                    // 部分上传
                } else {
                    resume_off = 0;
                }
            }
        } else {
            // STATUS 失败交给外层 failover；此处清空 err 后重试连接
            e->clear();
        }

        if (!skip_upload) {
            jobs_.update(job_id, JobPhase::Uploading);
            if (!dn.put_corpus(corpus_json, e, job_id, resume_off)) {
                return false;
            }
        }
        if (!skip_build) {
            jobs_.update(job_id, JobPhase::Building);
            if (!dn.build(&hash, &ver, e, job_id)) {
                return false;
            }
        }
        jobs_.update(job_id, JobPhase::Distributing);
        if (!fetch_and_load(dn.addr(), hash, e)) {
            return false;
        }
        for (const auto& p : cfg_.peer_http) {
            if (p.id == cfg_.raft.node_id) {
                continue;
            }
            if (!notify_peer_fetch(p.http_addr, dn.addr(), hash, e)) {
                return false;
            }
        }
        jobs_.update(job_id, JobPhase::Installing);
        if (hold_install) {
            while (!stopping_.load() && raft_->is_leader()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            *e = "held before InstallBase";
            return false;
        }
        vectordb::raft::Command cmd;
        cmd.mutable_install_base()->set_content_hash(hash);
        cmd.mutable_install_base()->set_base_version(install_version);
        if (cutoff_index > 0) {
            cmd.mutable_install_base()->set_cutoff_index(cutoff_index);
        }
        const auto pr =
            raft_->propose(cmd, std::chrono::milliseconds(cfg_.admin.propose_timeout_ms));
        if (pr.code != vectordb::raft::OK || pr.result.code != vectordb::raft::OK) {
            *e = std::string(err_name(pr.code != vectordb::raft::OK ? pr.code : pr.result.code)) +
                 ": " + (pr.code != vectordb::raft::OK ? pr.message : pr.result.message);
            return false;
        }
        *hash_out = hash;
        return true;
    };

    if (try_once(err)) {
        return true;
    }
    if (dn_ha_.switch_to_backup()) {
        std::string err2;
        if (try_once(&err2)) {
            return true;
        }
        *err = *err + "; after failover: " + err2;
    }
    return false;
}

bool HttpApi::run_init_job(const std::string& corpus_json, const std::string& job_id,
                           std::string* err) {
    if (sm_->ready()) {
        *err = "already initialized";
        return false;
    }
    std::string hash;
    if (!run_snapshot_pipeline(corpus_json, job_id, 1, 0, &hash, err)) {
        return false;
    }
    jobs_.finish_ok(job_id, hash, sm_->base_version());
    return true;
}

bool HttpApi::resume_rebuild_job(const std::string& job_id, std::string* err, bool hold_install) {
    const int64_t cutoff = sm_->cutoff_index();
    const int64_t new_ver = sm_->base_version() + 1;
    const std::string pre = sm_->pre_rebuild_hash();
    const std::string cur = sm_->content_hash();
    std::string hash;
    std::vector<std::pair<int32_t, int32_t>> mapping;
    // 本机已换上新快照时不能再把新 base 与旧 staging 合并导出
    if (!pre.empty() && cur != pre) {
        if (!run_snapshot_pipeline("", job_id, new_ver, cutoff, &hash, err, cur, hold_install)) {
            return false;
        }
        jobs_.finish_ok(job_id, hash, new_ver, cutoff, {});
        return true;
    }
    std::string corpus;
    if (!sm_->export_rebuild_corpus(&corpus, &mapping, err)) {
        return false;
    }
    if (!run_snapshot_pipeline(corpus, job_id, new_ver, cutoff, &hash, err, {}, hold_install)) {
        return false;
    }
    jobs_.finish_ok(job_id, hash, new_ver, cutoff, std::move(mapping));
    return true;
}

bool HttpApi::run_rebuild_job(int64_t expect_version, const std::string& job_id, std::string* err,
                              bool hold_install) {
    if (!sm_->ready()) {
        *err = "not ready";
        return false;
    }
    if (sm_->rebuilding()) {
        return resume_rebuild_job(job_id, err, hold_install);
    }
    vectordb::raft::Command rcmd;
    rcmd.mutable_rebuild()->set_base_version(expect_version);
    const auto pr =
        raft_->propose(rcmd, std::chrono::milliseconds(cfg_.admin.propose_timeout_ms));
    if (pr.code != vectordb::raft::OK || pr.result.code != vectordb::raft::OK) {
        *err = std::string(err_name(pr.code != vectordb::raft::OK ? pr.code : pr.result.code)) +
               ": " + (pr.code != vectordb::raft::OK ? pr.message : pr.result.message);
        return false;
    }
    const int64_t cutoff = pr.result.cutoff_index > 0 ? pr.result.cutoff_index : sm_->cutoff_index();

    std::string corpus;
    std::vector<std::pair<int32_t, int32_t>> mapping;
    if (!sm_->export_rebuild_corpus(&corpus, &mapping, err)) {
        return false;
    }
    const int64_t new_ver = expect_version + 1;
    std::string hash;
    if (!run_snapshot_pipeline(corpus, job_id, new_ver, cutoff, &hash, err, {}, hold_install)) {
        return false;
    }
    jobs_.finish_ok(job_id, hash, new_ver, cutoff, std::move(mapping));
    return true;
}

std::string HttpApi::handle_job_get(const std::string& id, int* http_status) {
    JobStatus st;
    if (!jobs_.get(id, &st)) {
        *http_status = 404;
        return json{{"code", "INVALID"}, {"message", "unknown job"}}.dump();
    }
    json map = json::array();
    for (const auto& [a, b] : st.mapping) {
        map.push_back({{"old_id", a}, {"new_id", b}});
    }
    *http_status = 200;
    return json{{"code", st.phase == JobPhase::Failed ? "INTERNAL" : "OK"},
                {"job_id", st.id},
                {"type", st.type},
                {"phase", job_phase_name(st.phase)},
                {"error", st.error},
                {"content_hash", st.content_hash},
                {"base_version", st.base_version},
                {"cutoff_index", st.cutoff_index},
                {"mapping", map}}
        .dump();
}

std::string HttpApi::handle_init(const std::string& body, const std::string& query,
                                 int* http_status) {
    if (sm_->ready()) {
        *http_status = 409;
        return json{{"code", "INVALID"}, {"message", "already initialized"}}.dump();
    }
    if (sm_->rebuilding()) {
        *http_status = 503;
        return json{{"code", "REBUILDING"}, {"message", "rebuild in progress"}}.dump();
    }
    if (!raft_->is_leader()) {
        *http_status = 421;
        return json{{"code", "NOT_LEADER"},
                    {"message", "not leader"},
                    {"leader_hint", raft_->leader_addr()}}
            .dump();
    }
    json root = json::parse(body, nullptr, false);
    if (root.is_discarded()) {
        *http_status = 400;
        return json{{"code", "INVALID"}, {"message", "bad json"}}.dump();
    }
    std::string corpus;
    std::string resume_id;
    if (root.is_array()) {
        corpus = body;
    } else if (root.is_object()) {
        resume_id = root.value("job_id", "");
        if (root.contains("docs")) {
            corpus = root["docs"].dump();
        }
    } else {
        *http_status = 400;
        return json{{"code", "INVALID"}, {"message", "need docs array or object"}}.dump();
    }
    // 新 Leader 续传：带同一 job_id；语料可省略则用持久化 corpus
    std::string job_id;
    if (!resume_id.empty()) {
        jobs_.ensure(resume_id, "init");
        job_id = resume_id;
        JobStatus st;
        if (jobs_.get(job_id, &st)) {
            if (st.phase == JobPhase::Done && sm_->ready()) {
                *http_status = 200;
                return json{{"code", "OK"},
                            {"ready", true},
                            {"job_id", job_id},
                            {"base_version", sm_->base_version()},
                            {"content_hash", sm_->content_hash()},
                            {"resumed", true}}
                    .dump();
            }
            if (corpus.empty() && !st.corpus_json.empty()) {
                corpus = st.corpus_json;
            }
        }
    } else {
        job_id = jobs_.create("init");
    }
    if (corpus.empty() || corpus == "[]") {
        *http_status = 400;
        return json{{"code", "INVALID"}, {"message", "empty corpus"}}.dump();
    }
    jobs_.set_corpus(job_id, corpus);
    const bool sync = query_has(query, "sync=1");
    if (sync) {
        std::string err;
        if (!run_init_job(corpus, job_id, &err)) {
            jobs_.finish_err(job_id, err);
            *http_status = 500;
            return json{{"code", "INTERNAL"}, {"message", err}, {"job_id", job_id}}.dump();
        }
        *http_status = 200;
        return json{{"code", "OK"},
                    {"ready", true},
                    {"job_id", job_id},
                    {"base_version", sm_->base_version()},
                    {"content_hash", sm_->content_hash()}}
            .dump();
    }
    workers_.emplace_back([this, corpus, job_id] {
        std::string err;
        if (!run_init_job(corpus, job_id, &err)) {
            jobs_.finish_err(job_id, err);
        }
    });
    *http_status = 202;
    return json{{"code", "OK"}, {"job_id", job_id}, {"phase", "queued"}}.dump();
}

std::string HttpApi::handle_rebuild(const std::string& body, const std::string& query,
                                    int* http_status) {
    if (!sm_->ready()) {
        *http_status = 503;
        return json{{"code", "NOT_READY"}, {"message", "not ready"}}.dump();
    }
    if (!raft_->is_leader()) {
        *http_status = 421;
        return json{{"code", "NOT_LEADER"},
                    {"message", "not leader"},
                    {"leader_hint", raft_->leader_addr()}}
            .dump();
    }
    auto root = json::parse(body.empty() ? "{}" : body, nullptr, false);
    if (root.is_discarded()) {
        *http_status = 400;
        return json{{"code", "INVALID"}, {"message", "bad json"}}.dump();
    }
    const int64_t expect = root.value("base_version", sm_->base_version());
    const std::string job_id = jobs_.create("rebuild");
    const bool sync = query_has(query, "sync=1") || body.find("sync") != std::string::npos;
    // 默认 sync 便于冒烟；异步用 ?async=1。hold_install=1 停在 InstallBase 之前供换主冒烟。
    const bool async = query_has(query, "async=1");
    const bool hold_install = query_has(query, "hold_install=1");
    if (!async) {
        std::string err;
        if (!run_rebuild_job(expect, job_id, &err, hold_install)) {
            jobs_.finish_err(job_id, err);
            *http_status = http_for(vectordb::raft::INTERNAL);
            return json{{"code", "INTERNAL"}, {"message", err}, {"job_id", job_id}}.dump();
        }
        JobStatus st;
        jobs_.get(job_id, &st);
        json map = json::array();
        for (const auto& [a, b] : st.mapping) {
            map.push_back({{"old_id", a}, {"new_id", b}});
        }
        *http_status = 200;
        return json{{"code", "OK"},
                    {"job_id", job_id},
                    {"new_version", st.base_version},
                    {"cutoff_index", st.cutoff_index},
                    {"content_hash", st.content_hash},
                    {"mapping", map}}
            .dump();
    }
    (void)sync;
    workers_.emplace_back([this, expect, job_id, hold_install] {
        std::string err;
        if (!run_rebuild_job(expect, job_id, &err, hold_install)) {
            jobs_.finish_err(job_id, err);
        }
    });
    *http_status = 202;
    return json{{"code", "OK"}, {"job_id", job_id}, {"phase", "queued"}}.dump();
}

std::string HttpApi::handle_fetch_snapshot(const std::string& body, int* http_status) {
    auto root = json::parse(body, nullptr, false);
    if (root.is_discarded() || !root.contains("content_hash")) {
        *http_status = 400;
        return json{{"code", "INVALID"}, {"message", "need content_hash"}}.dump();
    }
    const std::string expect_hash = root["content_hash"].get<std::string>();
    const std::string datanode = root.value("datanode", cfg_.datanode_addr);
    if (sm_->ready() && !sm_->rebuilding()) {
        if (sm_->content_hash() == expect_hash) {
            *http_status = 200;
            return json{{"code", "OK"}, {"content_hash", expect_hash}, {"idempotent", true}}.dump();
        }
        *http_status = 409;
        return json{{"code", "INVALID"}, {"message", "already ready with different hash"}}.dump();
    }
    if (sm_->ready() && sm_->rebuilding() && sm_->content_hash() == expect_hash) {
        *http_status = 200;
        return json{{"code", "OK"}, {"content_hash", expect_hash}, {"idempotent", true}}.dump();
    }
    if (!sm_->base_dir().empty() && sm_->content_hash() == expect_hash) {
        *http_status = 200;
        return json{{"code", "OK"}, {"content_hash", expect_hash}, {"idempotent", true}}.dump();
    }
    std::string err;
    if (!fetch_and_load(datanode, expect_hash, &err)) {
        *http_status = 500;
        return json{{"code", "INTERNAL"}, {"message", err}}.dump();
    }
    *http_status = 200;
    return json{{"code", "OK"}, {"content_hash", expect_hash}}.dump();
}

std::string HttpApi::handle_add(const std::string& body, int* http_status) {
    if (!sm_->ready()) {
        *http_status = 503;
        return json{{"code", "NOT_READY"}, {"message", "not ready"}}.dump();
    }
    if (!raft_->is_leader()) {
        *http_status = 421;
        return json{{"code", "NOT_LEADER"},
                    {"message", "not leader"},
                    {"leader_hint", raft_->leader_addr()}}
            .dump();
    }
    auto root = json::parse(body, nullptr, false);
    if (root.is_discarded() || !root.contains("docs") || !root["docs"].is_array()) {
        *http_status = 400;
        return json{{"code", "INVALID"}, {"message", "need {docs:[...]}"}}.dump();
    }
    const auto& docs = root["docs"];
    if (docs.empty()) {
        *http_status = 400;
        return json{{"code", "INVALID"}, {"message", "empty batch"}}.dump();
    }
    std::unordered_set<int32_t> seen;
    for (const auto& d : docs) {
        const int32_t tid = d.value("client_temp_id", 0);
        if (!seen.insert(tid).second) {
            *http_status = 400;
            return json{{"code", "INVALID"}, {"message", "duplicate client_temp_id"}}.dump();
        }
        const std::string content = d.value("content", "");
        if (content.empty() || content.size() > cfg_.admin.max_content_bytes) {
            *http_status = 400;
            return json{{"code", "INVALID"}, {"message", "bad content size"}}.dump();
        }
    }

    const std::string request_id = root.value("request_id", "");
    json ids = json::array();
    int64_t base_version = sm_->base_version();
    bool all_idempotent = !request_id.empty();
    size_t i = 0;
    while (i < docs.size()) {
        vectordb::raft::Command cmd;
        auto* batch = cmd.mutable_add();
        if (!request_id.empty()) {
            batch->set_request_id(request_id + "#" + std::to_string(i));
        }
        size_t batch_bytes = 0;
        size_t n = 0;
        std::vector<int32_t> temp_ids;
        while (i + n < docs.size() && n < cfg_.admin.max_batch_docs) {
            const auto& d = docs[i + n];
            const std::string content = d.value("content", "");
            if (n > 0 && batch_bytes + content.size() > cfg_.admin.max_batch_bytes) {
                break;
            }
            auto vec = embed_->embed(content, cfg_.admin.embed_timeout_s);
            if (vec.empty()) {
                *http_status = 500;
                return json{{"code", "INTERNAL"}, {"message", "embedding failed"}}.dump();
            }
            auto* p = batch->add_docs();
            p->set_title(d.value("title", ""));
            p->set_link(d.value("link", ""));
            p->set_content(content);
            p->mutable_embedding()->Assign(vec.begin(), vec.end());
            temp_ids.push_back(d.value("client_temp_id", 0));
            batch_bytes += content.size();
            ++n;
        }
        const auto pr =
            raft_->propose(cmd, std::chrono::milliseconds(cfg_.admin.propose_timeout_ms));
        const auto code = pr.code != vectordb::raft::OK ? pr.code : pr.result.code;
        if (code != vectordb::raft::OK) {
            *http_status = http_for(code);
            return json{{"code", err_name(code)},
                        {"message", pr.code != vectordb::raft::OK ? pr.message : pr.result.message},
                        {"leader_hint", pr.leader_hint},
                        {"ids", ids}}
                .dump();
        }
        base_version = pr.result.base_version;
        all_idempotent = all_idempotent && pr.result.idempotent;
        for (size_t k = 0; k < temp_ids.size(); ++k) {
            ids.push_back({{"client_temp_id", temp_ids[k]},
                           {"staging_id", pr.result.staging_ids.at(k)}});
        }
        i += n;
    }
    *http_status = 200;
    json resp{{"code", "OK"}, {"message", ""}, {"base_version", base_version}, {"ids", ids}};
    if (all_idempotent) {
        resp["idempotent"] = true;
    }
    return resp.dump();
}

std::string HttpApi::handle_delete(const std::string& body, int* http_status) {
    if (!sm_->ready()) {
        *http_status = 503;
        return json{{"code", "NOT_READY"}, {"message", "not ready"}}.dump();
    }
    auto root = json::parse(body, nullptr, false);
    if (root.is_discarded() || !root.contains("id") || !root.contains("base_version")) {
        *http_status = 400;
        return json{{"code", "INVALID"}, {"message", "need id and base_version"}}.dump();
    }
    vectordb::raft::Command cmd;
    cmd.mutable_del()->set_base_version(root["base_version"].get<int64_t>());
    cmd.mutable_del()->set_id(root["id"].get<int32_t>());
    const auto pr =
        raft_->propose(cmd, std::chrono::milliseconds(cfg_.admin.propose_timeout_ms));
    const auto code = pr.code != vectordb::raft::OK ? pr.code : pr.result.code;
    *http_status = http_for(code);
    return json{{"code", err_name(code)},
                {"message", pr.code != vectordb::raft::OK ? pr.message : pr.result.message},
                {"base_version", pr.code == vectordb::raft::OK ? pr.result.base_version
                                                               : sm_->base_version()},
                {"leader_hint", pr.leader_hint}}
        .dump();
}

std::string HttpApi::handle_search(const std::string& body, int* http_status) {
    if (!sm_->ready()) {
        *http_status = 503;
        return json{{"code", "NOT_READY"}, {"message", "not ready"}}.dump();
    }
    auto root = json::parse(body, nullptr, false);
    if (root.is_discarded() || !root.contains("query")) {
        *http_status = 400;
        return json{{"code", "INVALID"}, {"message", "need query"}}.dump();
    }
    const std::string q = root["query"].get<std::string>();
    if (q.empty()) {
        *http_status = 400;
        return json{{"code", "INVALID"}, {"message", "empty query"}}.dump();
    }
    const auto hits = engine_->search(q);
    json arr = json::array();
    for (const auto& h : hits) {
        arr.push_back({{"id", h.doc.id},
                       {"title", h.doc.title},
                       {"link", h.doc.link},
                       {"content", h.doc.content},
                       {"source", h.source},
                       {"score", h.score}});
    }
    *http_status = 200;
    return json{{"code", "OK"}, {"hits", arr}, {"base_version", sm_->base_version()}}.dump();
}

void HttpApi::start() {
    auto& srv = holder_->srv;
    srv.Get("/admin/status", [this](const httplib::Request&, httplib::Response& res) {
        res.set_content(handle_status(), "application/json");
    });
    srv.Get(R"(/admin/init/(.+))", [this](const httplib::Request& req, httplib::Response& res) {
        int st = 200;
        auto body = handle_job_get(req.matches[1], &st);
        res.status = st;
        res.set_content(body, "application/json");
    });
    srv.Get(R"(/admin/jobs/(.+))", [this](const httplib::Request& req, httplib::Response& res) {
        int st = 200;
        auto body = handle_job_get(req.matches[1], &st);
        res.status = st;
        res.set_content(body, "application/json");
    });
    srv.Post("/admin/init", [this](const httplib::Request& req, httplib::Response& res) {
        int st = 200;
        auto body = handle_init(req.body, req.target, &st);
        res.status = st;
        res.set_content(body, "application/json");
    });
    srv.Post("/admin/add", [this](const httplib::Request& req, httplib::Response& res) {
        int st = 200;
        auto body = handle_add(req.body, &st);
        res.status = st;
        res.set_content(body, "application/json");
    });
    srv.Post("/admin/delete", [this](const httplib::Request& req, httplib::Response& res) {
        int st = 200;
        auto body = handle_delete(req.body, &st);
        res.status = st;
        res.set_content(body, "application/json");
    });
    srv.Post("/admin/rebuild", [this](const httplib::Request& req, httplib::Response& res) {
        int st = 200;
        auto body = handle_rebuild(req.body, req.target, &st);
        res.status = st;
        res.set_content(body, "application/json");
    });
    srv.Post("/query/search", [this](const httplib::Request& req, httplib::Response& res) {
        int st = 200;
        auto body = handle_search(req.body, &st);
        res.status = st;
        res.set_content(body, "application/json");
    });
    srv.Post("/internal/fetch_snapshot",
             [this](const httplib::Request& req, httplib::Response& res) {
                 int st = 200;
                 auto body = handle_fetch_snapshot(req.body, &st);
                 res.status = st;
                 res.set_content(body, "application/json");
             });
    srv.new_task_queue = [this] { return new httplib::ThreadPool(cfg_.http_threads); };

    const auto colon = cfg_.http_addr.rfind(':');
    const std::string host = cfg_.http_addr.substr(0, colon);
    const int port = std::stoi(cfg_.http_addr.substr(colon + 1));
    std::cerr << "[http] listen " << cfg_.http_addr << "\n";
    if (!srv.listen(host.c_str(), port)) {
        std::cerr << "[http] listen failed\n";
    }
}

}  // namespace vdb
