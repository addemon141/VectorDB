#pragma once

#include "TextUtil.hh"

#include <cppjieba/Jieba.hpp>

#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace exact_index {

struct Bm25Stats {
    int n = 0;           // 语料文档数 N
    double avgdl = 0.0;  // 平均文档长度
    double k1 = 1.2;     // TF 饱和参数
    double b = 0.75;     // 长度归一参数
};

struct Bm25Hit {
    int doc_id = 0;
    double score = 0.0;
};

// Bm25Index：内存倒排（词→(文档,词频)），查询时用 BM25 公式打分
class Bm25Index {
public:
    bool load(const std::string& data_dir);  // 读默认 index/doclen/stats
    bool load_paths(const std::string& index_path,
                    const std::string& doclen_path,
                    const std::string& stats_path);

    // 查询串分词后取 top_k；（bm25_query / QueryEngine 主路径）
    std::vector<Bm25Hit> search(const std::string& query,
                                int top_k,
                                cppjieba::Jieba& jieba,
                                const Stopwords& sw) const;

    // 已分好词的 query_tf 直接打分（staging 近似打分也会用类似逻辑）
    std::vector<Bm25Hit> search_tf(const std::map<std::string, int>& query_tf,
                                   int top_k) const;

    const Bm25Stats& stats() const { return stats_; }
    int doc_length(int doc_id) const;

    // 含 term 的基座文档数；不在词表中返回 0
    int df(const std::string& term) const;

    // 以下两个公开给 server 对暂存文档打分，保证与基座同一套公式
    static double idf(int n, int df);  // 文档频率 → idf
    double score_term(int tf, int dl, double idf_val) const;  // 单术语贡献分（用本索引的 avgdl/k1/b）

private:
    Bm25Stats stats_;
    std::unordered_map<int, int> doclen_;  // 文档长度
    // 倒排：term → [(docid, 原始词频 tf), ...]
    std::unordered_map<std::string, std::vector<std::pair<int, int>>> postings_;
};

// Build query term frequencies (raw counts) with build-compatible tokenization.
void fill_query_tf(cppjieba::Jieba& jieba,
                   const Stopwords& sw,
                   const std::string& text,
                   std::map<std::string, int>& out_tf);

}  // namespace exact_index
