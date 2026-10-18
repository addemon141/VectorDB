#pragma once

#include "Document.hh"

#include <string>
#include <vector>

namespace vdb::datanode {

struct BuildConfig {
    exact_index::BuildPaths text_paths;
    std::string embed_url = "http://127.0.0.1:50053/embed";
    int embed_timeout_s = 30;
    int workers = 4;
    int hnsw_M = 16;
    int hnsw_ef_construction = 200;
    int hnsw_ef_search = 64;
};

// 语料 JSON 数组 → BM25 + embeddings + HNSW，写出 out_dir；成功返回 content_hash
bool build_snapshot_from_json(const std::string& corpus_json, const std::string& out_dir,
                              const BuildConfig& cfg, std::string* content_hash,
                              std::string* err);

}  // namespace vdb::datanode
