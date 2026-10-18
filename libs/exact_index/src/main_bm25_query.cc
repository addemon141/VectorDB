#include "Bm25Index.hh"
#include "Document.hh"

#include <cppjieba/Jieba.hpp>

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void usage(const char* argv0) {
    std::cerr << "Usage:\n  " << argv0
              << " --data-dir DIR --query TEXT [--k N]\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::string data_dir;
    std::string query;
    int k = 5;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto need = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "missing value for " << name << "\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--data-dir") {
            data_dir = need("--data-dir");
        } else if (arg == "--query") {
            query = need("--query");
        } else if (arg == "--k") {
            k = std::stoi(need("--k"));
        } else if (arg == "-h" || arg == "--help") {
            usage(argv[0]);
            return 0;
        } else {
            std::cerr << "unknown arg: " << arg << "\n";
            usage(argv[0]);
            return 2;
        }
    }

    if (data_dir.empty() || query.empty()) {
        usage(argv[0]);
        return 2;
    }

    exact_index::Bm25Index index;
    if (!index.load(data_dir)) {
        std::cerr << "failed to load BM25 index from " << data_dir << "\n";
        return 1;
    }

    exact_index::BuildPaths paths;
    const char* sw_cn_env = std::getenv("EXACT_INDEX_CN_STOP");
    const char* sw_en_env = std::getenv("EXACT_INDEX_EN_STOP");
    paths.cn_stopwords =
        sw_cn_env ? sw_cn_env
                  : std::string("testdata/stopwords/cn_stopwords.txt");
    paths.en_stopwords =
        sw_en_env ? sw_en_env
                  : std::string("testdata/stopwords/en_stopwords.txt");

    exact_index::Stopwords sw;
    if (!sw.load(paths.cn_stopwords, paths.en_stopwords)) {
        std::cerr << "failed to load stopwords\n";
        return 1;
    }

    cppjieba::Jieba jieba(paths.jieba_dict, paths.jieba_hmm, paths.jieba_user,
                          paths.jieba_idf, paths.jieba_stop);

    auto hits = index.search(query, k, jieba, sw);
    std::cout << "bm25 top-" << k << " n=" << index.stats().n
              << " avgdl=" << index.stats().avgdl << "\n";
    for (const auto& h : hits) {
        std::cout << "  id=" << h.doc_id << " score=" << h.score << "\n";
    }
    return 0;
}
