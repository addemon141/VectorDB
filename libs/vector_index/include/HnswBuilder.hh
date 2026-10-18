#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace vector_index {

// 一批嵌入：ids[i] 对应 data 里第 i 行（长度 dim）
struct EmbeddingSet {
    int dim = 0;
    std::vector<int32_t> ids;
    std::vector<float> data;  // 行主序：共 ids.size()*dim 个 float
};

struct HnswBuildConfig {
    int M = 16;                 // 图每个节点的边数规模
    int ef_construction = 200;  // 建图时搜索宽度（越大越准越慢）
    int ef_search = 64;         // 查询默认搜索宽度
    std::string metric = "l2";
    int threads = 0;            // 并发插入线程数；<=0 取 hardware_concurrency
};

// 读 embed_sbert.py 产出的 embeddings.f32 / .ids
EmbeddingSet load_embeddings(const std::string& dir);

// 建 HNSW 图并写出 vectors.hnsw + vectors.meta.json（供查询服务加载）
void build_and_save_hnsw(const EmbeddingSet& emb,
                         const std::string& out_dir,
                         const HnswBuildConfig& cfg);

}  // namespace vector_index
