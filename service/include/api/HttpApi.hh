#pragma once

#include "ServerConfig.hh"
#include "index/EmbedClient.hh"
#include "job/ClusterJob.hh"
#include "net/DataNodeClient.hh"
#include "raft/RaftNode.hh"
#include "sm/QueryEngine.hh"
#include "sm/StateMachine.hh"

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace vdb {

class HttpApi {
public:
    HttpApi(ServerConfig cfg, raft::RaftNode* raft, StateMachine* sm, QueryEngine* engine,
            EmbedClient* embed);
    ~HttpApi();

    void start();
    void stop();

private:
    std::string handle_status();
    std::string handle_init(const std::string& body, const std::string& query, int* http_status);
    std::string handle_job_get(const std::string& id, int* http_status);
    std::string handle_add(const std::string& body, int* http_status);
    std::string handle_delete(const std::string& body, int* http_status);
    std::string handle_rebuild(const std::string& body, const std::string& query, int* http_status);
    std::string handle_search(const std::string& body, int* http_status);
    std::string handle_fetch_snapshot(const std::string& body, int* http_status);

    bool fetch_and_load(const std::string& datanode, const std::string& expect_hash,
                        std::string* err);
    bool notify_peer_fetch(const std::string& peer_http, const std::string& datanode,
                           const std::string& content_hash, std::string* err);

    // 上传→建库→拉取→InstallBase；cutoff>0 时写入 InstallBase。
    // prepared_hash 非空：本机已装上该快照，跳过上传/建库。
    // hold_install：分发后、propose InstallBase 前等待下台（换主冒烟）。
    bool run_snapshot_pipeline(const std::string& corpus_json, const std::string& job_id,
                               int64_t install_version, int64_t cutoff_index, std::string* hash_out,
                               std::string* err, const std::string& prepared_hash = {},
                               bool hold_install = false);

    bool run_init_job(const std::string& corpus_json, const std::string& job_id, std::string* err);
    bool run_rebuild_job(int64_t expect_version, const std::string& job_id, std::string* err,
                         bool hold_install = false);
    bool resume_rebuild_job(const std::string& job_id, std::string* err, bool hold_install);

    ServerConfig cfg_;
    raft::RaftNode* raft_;
    StateMachine* sm_;
    QueryEngine* engine_;
    EmbedClient* embed_;
    FailoverDataNodeClient dn_ha_;
    JobTable jobs_;
    std::atomic<bool> stopping_{false};
    std::vector<std::thread> workers_;
    std::unique_ptr<class HttpServerHolder> holder_;
};

}  // namespace vdb
