#include "HnswBuilder.hh"

#include "hnswlib.h"

#include <algorithm>
#include <iostream>
#include <vector>

// Smoke: load vectors.hnsw and search with the first --queries stored embeddings as queries.
int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0]
                  << " --emb-dir DIR --index-dir DIR [--k N] [--queries N]\n";
        return 2;
    }
    std::string emb_dir;
    std::string index_dir;
    int k = 5;
    int queries = 1;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto need = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "missing value for " << name << "\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--emb-dir") {
            emb_dir = need("--emb-dir");
        } else if (arg == "--index-dir") {
            index_dir = need("--index-dir");
        } else if (arg == "--k") {
            k = std::stoi(need("--k"));
        } else if (arg == "--queries") {
            queries = std::stoi(need("--queries"));
        }
    }
    if (emb_dir.empty() || index_dir.empty() || queries <= 0) {
        return 2;
    }

    try {
        auto emb = vector_index::load_embeddings(emb_dir);
        hnswlib::L2Space space(emb.dim);
        hnswlib::HierarchicalNSW<float> index(&space, index_dir + "/vectors.hnsw");
        index.setEf(64);
        const size_t q_count = std::min(emb.ids.size(), static_cast<size_t>(queries));
        for (size_t q = 0; q < q_count; ++q) {
            const float* row = emb.data.data() + q * static_cast<size_t>(emb.dim);
            auto result = index.searchKnn(row, static_cast<size_t>(k));
            std::vector<std::pair<float, hnswlib::labeltype>> hits;
            while (!result.empty()) {
                hits.push_back(result.top());
                result.pop();
            }
            std::reverse(hits.begin(), hits.end());
            std::cout << "top-" << k << " for docid=" << emb.ids[q] << ":\n";
            for (const auto& [dist, label] : hits) {
                std::cout << "  label=" << label << " dist=" << dist << "\n";
            }
        }
    } catch (const std::exception& ex) {
        std::cerr << "hnsw_query failed: " << ex.what() << "\n";
        return 1;
    }
    return 0;
}
