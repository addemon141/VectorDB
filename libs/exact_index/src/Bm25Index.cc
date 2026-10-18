#include "Bm25Index.hh"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <cctype>

namespace exact_index {
namespace {

bool parse_stats_json(const std::string& text, Bm25Stats& out) {
    auto grab_number = [&](const char* key, double& dest) -> bool {
        const std::string k = std::string("\"") + key + "\"";
        auto pos = text.find(k);
        if (pos == std::string::npos) {
            return false;
        }
        pos = text.find(':', pos);
        if (pos == std::string::npos) {
            return false;
        }
        dest = std::stod(text.substr(pos + 1));
        return true;
    };
    double n = 0, avgdl = 0, k1 = 1.2, b = 0.75;
    if (!grab_number("n", n) || !grab_number("avgdl", avgdl)) {
        return false;
    }
    grab_number("k1", k1);
    grab_number("b", b);
    out.n = static_cast<int>(n);
    out.avgdl = avgdl;
    out.k1 = k1;
    out.b = b;
    return out.n > 0 && out.avgdl > 0.0;
}

}  // namespace

double Bm25Index::idf(int n, int df) {
    // Lucene-style: ln(1 + (N - df + 0.5)/(df + 0.5))
    return std::log(1.0 + (static_cast<double>(n) - df + 0.5) / (df + 0.5));
}

double Bm25Index::score_term(int tf, int dl, double idf_val) const {
    if (tf <= 0 || dl <= 0 || stats_.avgdl <= 0.0) {
        return 0.0;
    }
    const double denom =
        tf + stats_.k1 * (1.0 - stats_.b + stats_.b * (static_cast<double>(dl) / stats_.avgdl));
    return idf_val * (tf * (stats_.k1 + 1.0)) / denom;
}

int Bm25Index::doc_length(int doc_id) const {
    auto it = doclen_.find(doc_id);
    return it == doclen_.end() ? 0 : it->second;
}

int Bm25Index::df(const std::string& term) const {
    auto it = postings_.find(term);
    return it == postings_.end() ? 0 : static_cast<int>(it->second.size());
}

bool Bm25Index::load(const std::string& data_dir) {
    return load_paths(data_dir + "/index.data", data_dir + "/doclen.data",
                      data_dir + "/stats.json");
}

bool Bm25Index::load_paths(const std::string& index_path,
                           const std::string& doclen_path,
                           const std::string& stats_path) {
    postings_.clear();
    doclen_.clear();

    std::ifstream stats_ifs(stats_path);
    if (!stats_ifs) {
        return false;
    }
    std::stringstream ss;
    ss << stats_ifs.rdbuf();
    if (!parse_stats_json(ss.str(), stats_)) {
        return false;
    }

    std::ifstream doclen_ifs(doclen_path);
    if (!doclen_ifs) {
        return false;
    }
    int id = 0;
    int len = 0;
    while (doclen_ifs >> id >> len) {
        doclen_[id] = len;
    }

    std::ifstream index_ifs(index_path);
    if (!index_ifs) {
        return false;
    }
    std::string line;
    while (std::getline(index_ifs, line)) {
        if (line.empty()) {
            continue;
        }
        std::istringstream iss(line);
        std::string term;
        if (!(iss >> term)) {
            continue;
        }
        auto& posting = postings_[term];
        int doc_id = 0;
        int tf = 0;
        while (iss >> doc_id >> tf) {
            posting.emplace_back(doc_id, tf);
        }
    }
    return !postings_.empty() || stats_.n == 0;
}

void fill_query_tf(cppjieba::Jieba& jieba,
                   const Stopwords& sw,
                   const std::string& text,
                   std::map<std::string, int>& out_tf) {
    out_tf.clear();
    std::vector<std::string> words;
    jieba.Cut(text, words);
    for (auto word : words) {
        if (valid_cn_word(word, sw)) {
            out_tf[word] += 1;
            continue;
        }
        bool is_en = true;
        for (char& ch : word) {
            if (!std::isalpha(static_cast<unsigned char>(ch))) {
                is_en = false;
                break;
            }
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        if (!is_en || !valid_en_word(word, sw)) {
            continue;
        }
        out_tf[word] += 1;
    }
}

std::vector<Bm25Hit> Bm25Index::search_tf(const std::map<std::string, int>& query_tf,
                                          int top_k) const {
    std::unordered_map<int, double> scores;
    for (const auto& [term, qtf] : query_tf) {
        if (qtf <= 0) {
            continue;
        }
        auto it = postings_.find(term);
        if (it == postings_.end()) {
            continue;
        }
        const int df = static_cast<int>(it->second.size());
        const double idf_val = idf(stats_.n, df);
        for (const auto& [doc_id, tf] : it->second) {
            const int dl = doc_length(doc_id);
            // Query term frequency is usually 1; multiply lightly if repeated.
            scores[doc_id] += static_cast<double>(qtf) * score_term(tf, dl, idf_val);
        }
    }

    std::vector<Bm25Hit> hits;
    hits.reserve(scores.size());
    for (const auto& [doc_id, score] : scores) {
        hits.push_back(Bm25Hit{doc_id, score});
    }
    std::sort(hits.begin(), hits.end(), [](const Bm25Hit& a, const Bm25Hit& b) {
        if (a.score != b.score) {
            return a.score > b.score;
        }
        return a.doc_id < b.doc_id;
    });
    if (top_k > 0 && static_cast<int>(hits.size()) > top_k) {
        hits.resize(static_cast<size_t>(top_k));
    }
    return hits;
}

std::vector<Bm25Hit> Bm25Index::search(const std::string& query,
                                       int top_k,
                                       cppjieba::Jieba& jieba,
                                       const Stopwords& sw) const {
    std::map<std::string, int> qtf;
    fill_query_tf(jieba, sw, query, qtf);
    return search_tf(qtf, top_k);
}

}  // namespace exact_index
