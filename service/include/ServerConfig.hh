#pragma once

#include "Document.hh"
#include "raft/RaftNode.hh"
#include "sm/QueryEngine.hh"

#include <string>
#include <vector>

namespace vdb {

struct AdminLimits {
    int propose_timeout_ms = 30000;
    size_t max_batch_docs = 256;
    size_t max_content_bytes = 100 * 1024;
    size_t max_batch_bytes = 4 * 1024 * 1024;
    int embed_timeout_s = 30;
    int fetch_snapshot_timeout_s = 120;
};

struct PeerHttpInfo {
    int32_t id = 0;
    std::string http_addr;
};

struct ServerConfig {
    std::string data_dir;
    std::string http_addr;
    std::string raft_addr;
    std::string datanode_addr = "127.0.0.1:6100";
    std::string datanode_backup;  // 可空
    std::string embed_url = "http://127.0.0.1:50053/embed";
    int http_threads = 8;
    exact_index::BuildPaths text_paths;
    raft::RaftConfig raft;
    QueryConfig query;
    AdminLimits admin;
    std::vector<PeerHttpInfo> peer_http;
};

}  // namespace vdb
