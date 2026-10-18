#include "IndexBuilder.hh"

#include "ExactIndexBuilder.hh"
#include "HnswBuilder.hh"
#include "tlv/hash_util.hh"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

namespace vdb::datanode {
namespace {

std::string shell_escape(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out.push_back(c);
        }
    }
    out.push_back('\'');
    return out;
}

std::string json_escape(const std::string& text) {
    std::string e;
    e.reserve(text.size() + 16);
    for (unsigned char c : text) {
        if (c == '\\' || c == '"') {
            e.push_back('\\');
            e.push_back(static_cast<char>(c));
        } else if (c == '\n') {
            e += "\\n";
        } else if (c == '\r') {
            e += "\\r";
        } else if (c == '\t') {
            e += "\\t";
        } else if (c < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\u%04x", c);
            e += buf;
        } else {
            e.push_back(static_cast<char>(c));
        }
    }
    return e;
}

bool parse_embed_json(const std::string& body, std::vector<float>* out) {
    auto pos = body.find("\"values\"");
    if (pos == std::string::npos) {
        return false;
    }
    pos = body.find('[', pos);
    if (pos == std::string::npos) {
        return false;
    }
    auto end = body.find(']', pos);
    if (end == std::string::npos) {
        return false;
    }
    out->clear();
    std::stringstream ss(body.substr(pos + 1, end - pos - 1));
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        if (tok.empty()) {
            continue;
        }
        try {
            out->push_back(std::stof(tok));
        } catch (...) {
            return false;
        }
    }
    return !out->empty();
}

bool embed_one(const std::string& url, const std::string& text, int timeout_s,
               std::vector<float>* out) {
    const std::string payload = "{\"text\":\"" + json_escape(text) + "\"}";
    const std::string cmd = "curl -sS -m " + std::to_string(timeout_s) + " -H 'Content-Type: application/json' -d " +
                            shell_escape(payload) + " " + shell_escape(url);
    FILE* fp = popen(cmd.c_str(), "r");
    if (!fp) {
        return false;
    }
    std::string body;
    char buf[4096];
    while (fgets(buf, sizeof(buf), fp)) {
        body += buf;
    }
    const int rc = pclose(fp);
    if (rc != 0) {
        return false;
    }
    return parse_embed_json(body, out);
}

bool write_embeddings(const std::string& dir, const std::vector<int32_t>& ids,
                      const std::vector<float>& data, int dim) {
    {
        std::ofstream ids_ofs(dir + "/embeddings.ids", std::ios::binary);
        std::ofstream f32_ofs(dir + "/embeddings.f32", std::ios::binary);
        if (!ids_ofs || !f32_ofs) {
            return false;
        }
        ids_ofs.write(reinterpret_cast<const char*>(ids.data()),
                      static_cast<std::streamsize>(ids.size() * sizeof(int32_t)));
        f32_ofs.write(reinterpret_cast<const char*>(data.data()),
                      static_cast<std::streamsize>(data.size() * sizeof(float)));
    }
    std::ofstream meta(dir + "/embeddings.meta.json");
    if (!meta) {
        return false;
    }
    meta << "{\"dim\":" << dim << ",\"count\":" << ids.size() << "}\n";
    return true;
}

}  // namespace

bool build_snapshot_from_json(const std::string& corpus_json, const std::string& out_dir,
                              const BuildConfig& cfg, std::string* content_hash,
                              std::string* err) {
    nlohmann::json root;
    try {
        root = nlohmann::json::parse(corpus_json);
    } catch (const std::exception& ex) {
        *err = std::string("corpus json: ") + ex.what();
        return false;
    }
    if (!root.is_array() || root.empty()) {
        *err = "corpus must be a non-empty JSON array";
        return false;
    }

    std::vector<exact_index::Document> docs;
    docs.reserve(root.size());
    int auto_id = 1;
    for (const auto& item : root) {
        exact_index::Document d;
        d.id = item.value("id", 0);
        if (d.id <= 0) {
            d.id = auto_id;
        }
        auto_id = std::max(auto_id, d.id + 1);
        d.title = item.value("title", "");
        d.link = item.value("link", "");
        d.content = item.value("content", "");
        if (d.content.empty()) {
            *err = "doc id " + std::to_string(d.id) + " has empty content";
            return false;
        }
        docs.push_back(std::move(d));
    }

    std::error_code ec;
    std::filesystem::create_directories(out_dir, ec);
    if (!exact_index::write_corpus_bm25(docs, out_dir, cfg.text_paths, cfg.workers)) {
        *err = "write_corpus_bm25 failed";
        return false;
    }

    std::vector<int32_t> ids;
    std::vector<float> data;
    int dim = 0;
    ids.reserve(docs.size());
    for (const auto& d : docs) {
        std::vector<float> vec;
        if (!embed_one(cfg.embed_url, d.content, cfg.embed_timeout_s, &vec)) {
            *err = "embedding failed for doc " + std::to_string(d.id);
            return false;
        }
        if (dim == 0) {
            dim = static_cast<int>(vec.size());
            data.reserve(docs.size() * static_cast<size_t>(dim));
        } else if (static_cast<int>(vec.size()) != dim) {
            *err = "embedding dim mismatch";
            return false;
        }
        ids.push_back(d.id);
        data.insert(data.end(), vec.begin(), vec.end());
    }
    if (!write_embeddings(out_dir, ids, data, dim)) {
        *err = "write embeddings failed";
        return false;
    }

    try {
        vector_index::EmbeddingSet emb;
        emb.dim = dim;
        emb.ids = ids;
        emb.data = data;
        vector_index::HnswBuildConfig hcfg;
        hcfg.M = cfg.hnsw_M;
        hcfg.ef_construction = cfg.hnsw_ef_construction;
        hcfg.ef_search = cfg.hnsw_ef_search;
        hcfg.threads = cfg.workers;
        vector_index::build_and_save_hnsw(emb, out_dir, hcfg);
    } catch (const std::exception& ex) {
        *err = std::string("hnsw build: ") + ex.what();
        return false;
    }

    *content_hash = vdb::tlv::snapshot_hash(out_dir, err);
    if (content_hash->empty()) {
        return false;
    }
    std::ofstream hash_ofs(out_dir + "/SNAPSHOT.hash");
    hash_ofs << *content_hash << "\n";
    std::cerr << "[datanode] snapshot ready hash=" << *content_hash << " docs=" << docs.size()
              << "\n";
    return true;
}

}  // namespace vdb::datanode
