#include "sm/QueryEngine.hh"

#include <algorithm>
#include <climits>
#include <unordered_map>
#include <utility>

namespace vdb {
namespace {

// L2 平方距离，与 hnswlib L2Space 的输出同一尺度。影响：无
float l2_sq(const std::vector<float>& a, const std::vector<float>& b) {
    float s = 0.0f;
    for (size_t i = 0; i < a.size(); ++i) {
        const float d = a[i] - b[i];
        s += d * d;
    }
    return s;
}

struct Fused {
    int id = 0;
    double score = 0.0;
    int best_rank = INT_MAX;
    bool lex = false;
    bool vec = false;
};

}  // namespace

std::vector<SearchHit> QueryEngine::search(const std::string& query) const {
    // 1. 锁外预处理
    const auto qtf = tok_->term_freq(query);
    const std::vector<float> qvec = embed_->embed(query, cfg_.embed_timeout_s);

    const size_t depth = static_cast<size_t>(std::max(cfg_.top_k * 3, cfg_.min_depth));
    std::vector<SearchHit> out;
    std::shared_ptr<const BaseIndex> base;
    {
        // 2. 一致快照上召回与融合
        const auto view = sm_->read();
        if (!view.ready || !view.base || !view.staging) {
            return {};
        }
        base = view.base;
        const StagingArea& st = *view.staging;
        const auto& tomb = st.base_tombstones;

        // 词法路：基座 + 暂存区，同一套 N / avgdl / df，分数可直接比较
        std::vector<std::pair<int, double>> lex;
        if (!qtf.empty()) {
            for (const auto& h : base->bm25.search_tf(qtf, 0)) {
                if (!tomb.count(h.doc_id)) {
                    lex.emplace_back(h.doc_id, h.score);
                    if (lex.size() >= depth) {
                        break;
                    }
                }
            }
            const int n = base->bm25.stats().n;
            for (const auto& [id, d] : st.docs) {
                double score = 0.0;
                for (const auto& [term, qc] : qtf) {
                    auto it = d.tf.find(term);
                    if (it == d.tf.end()) {
                        continue;
                    }
                    const int df = std::max(base->bm25.df(term), 1);  // 只在暂存区出现的词按 1 计
                    score += qc * base->bm25.score_term(it->second, d.doc_len,
                                                        exact_index::Bm25Index::idf(n, df));
                }
                if (score > 0.0) {
                    lex.emplace_back(id, score);
                }
            }
            std::sort(lex.begin(), lex.end(), [](const auto& a, const auto& b) {
                return a.second != b.second ? a.second > b.second : a.first < b.first;
            });
            if (lex.size() > depth) {
                lex.resize(depth);
            }
        }

        // 语义路：基座 HNSW + 暂存区暴力，都是 L2 平方距离
        std::vector<std::pair<int, float>> vec;
        if (!qvec.empty()) {
            if (base->hnsw.dim() == static_cast<int>(qvec.size())) {
                for (const auto& h :
                     base->hnsw.search(qvec, static_cast<int>(depth + tomb.size()))) {
                    if (!tomb.count(h.doc_id)) {
                        vec.emplace_back(h.doc_id, h.dist);
                    }
                }
            }
            for (const auto& [id, d] : st.docs) {
                if (d.embedding.size() == qvec.size()) {
                    vec.emplace_back(id, l2_sq(qvec, d.embedding));
                }
            }
            std::sort(vec.begin(), vec.end(), [](const auto& a, const auto& b) {
                return a.second != b.second ? a.second < b.second : a.first < b.first;
            });
            if (vec.size() > depth) {
                vec.resize(depth);
            }
        }

        // RRF：score = Σ w / (k + rank)，名次从 1 开始
        std::unordered_map<int, Fused> acc;
        auto add_list = [&](const auto& ranked, bool is_lex, double w) {
            for (size_t i = 0; i < ranked.size(); ++i) {
                const int id = ranked[i].first;
                const int rank = static_cast<int>(i) + 1;
                auto& f = acc[id];
                f.id = id;
                f.score += w / (cfg_.rrf_k + rank);
                f.best_rank = std::min(f.best_rank, rank);
                (is_lex ? f.lex : f.vec) = true;
            }
        };
        add_list(lex, true, cfg_.lex_weight);
        add_list(vec, false, cfg_.vec_weight);

        std::vector<Fused> fused;
        fused.reserve(acc.size());
        for (auto& [_, f] : acc) {
            fused.push_back(f);
        }
        std::sort(fused.begin(), fused.end(), [](const Fused& a, const Fused& b) {
            if (a.score != b.score) {
                return a.score > b.score;
            }
            if (a.best_rank != b.best_rank) {
                return a.best_rank < b.best_rank;
            }
            return a.id < b.id;
        });
        if (fused.size() > static_cast<size_t>(cfg_.top_k)) {
            fused.resize(static_cast<size_t>(cfg_.top_k));
        }

        // 暂存文档读锁一放就可能被删，正文在锁内拷贝；基座文档先只记 id
        for (const auto& f : fused) {
            SearchHit h;
            h.doc.id = f.id;
            h.source = f.lex && f.vec ? "both" : (f.lex ? "bm25" : "vector");
            h.score = f.score;
            if (f.id < 0) {
                const auto& d = st.docs.at(f.id);
                h.doc.title = d.title;
                h.doc.link = d.link;
                h.doc.content = d.content;
            }
            out.push_back(std::move(h));
        }
    }

    // 3. 锁外读基座正文（base 由 shared_ptr 持有，Rebuild 替换后旧文件仍可读）
    std::vector<SearchHit> result;
    result.reserve(out.size());
    for (auto& h : out) {
        if (h.doc.id > 0 && !base->docs.get(h.doc.id, &h.doc)) {
            continue;
        }
        result.push_back(std::move(h));
    }
    return result;
}

}  // namespace vdb
