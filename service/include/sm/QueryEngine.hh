#pragma once

#include "index/EmbedClient.hh"
#include "sm/StateMachine.hh"
#include "sm/Tokenizer.hh"

#include <string>
#include <vector>

namespace vdb {

struct QueryConfig {
    int top_k = 10;
    int min_depth = 50;      // 每路候选深度 R = max(top_k*3, min_depth)
    int rrf_k = 60;          // RRF 平滑常数
    double lex_weight = 1.0; // 词法路（BM25）权重
    double vec_weight = 1.0; // 语义路（向量）权重
    int embed_timeout_s = 10;
};

struct SearchHit {
    StoredDoc doc;
    std::string source;  // "bm25" | "vector" | "both"
    double score = 0.0;  // RRF 分数，只用于排序
};

// QueryEngine = 用户通道的查询链路。任一节点本地执行，不经过 Raft。
//   1. 锁外：分词（Tokenizer）、取 query 向量（EmbedClient，失败则只走 BM25）
//   2. 持 StateMachine 读锁：基座 BM25 / 暂存区 BM25（用基座统计量）合成词法路，
//      基座 HNSW / 暂存区暴力 L2 合成语义路，均过滤 base_tombstones；两路 RRF 融合取 top_k；
//      暂存文档正文在锁内拷贝
//   3. 锁外：按 id 从基座 DocStore 读正文
// 线程安全：可并发调用。
class QueryEngine {
public:
    QueryEngine(const StateMachine* sm, const Tokenizer* tok, const EmbedClient* embed,
                QueryConfig cfg)
        : sm_(sm), tok_(tok), embed_(embed), cfg_(cfg) {}

    // 输入：query 非空文本；输出：最多 top_k 条，按 RRF 分数降序；影响：只读
    std::vector<SearchHit> search(const std::string& query) const;

private:
    const StateMachine* sm_;
    const Tokenizer* tok_;
    const EmbedClient* embed_;
    QueryConfig cfg_;
};

}  // namespace vdb
