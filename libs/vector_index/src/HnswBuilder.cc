#include "HnswBuilder.hh"

#include "hnswlib.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace vector_index {
namespace {

int parse_dim_from_meta(const std::string& dir) {
    std::ifstream ifs(dir + "/embeddings.meta.json");
    if (!ifs) {
        return 0;
    }
    std::stringstream ss;
    ss << ifs.rdbuf();
    const std::string text = ss.str();
    const auto key = std::string("\"dim\"");
    auto pos = text.find(key);
    if (pos == std::string::npos) {
        return 0;
    }
    pos = text.find(':', pos);
    if (pos == std::string::npos) {
        return 0;
    }
    return std::stoi(text.substr(pos + 1));
}

}  // namespace

EmbeddingSet load_embeddings(const std::string& dir) {
    EmbeddingSet emb;
    std::ifstream ids_ifs(dir + "/embeddings.ids", std::ios::binary);
    std::ifstream f32_ifs(dir + "/embeddings.f32", std::ios::binary);
    if (!ids_ifs || !f32_ifs) {
        throw std::runtime_error("failed to open embeddings.ids / embeddings.f32 in " + dir);
    }

    ids_ifs.seekg(0, std::ios::end);
    const auto ids_bytes = static_cast<size_t>(ids_ifs.tellg());
    ids_ifs.seekg(0);
    if (ids_bytes % sizeof(int32_t) != 0) {
        throw std::runtime_error("embeddings.ids size is not a multiple of 4");
    }
    const size_t n = ids_bytes / sizeof(int32_t);
    emb.ids.resize(n);
    ids_ifs.read(reinterpret_cast<char*>(emb.ids.data()),
                 static_cast<std::streamsize>(ids_bytes));

    f32_ifs.seekg(0, std::ios::end);
    const auto f32_bytes = static_cast<size_t>(f32_ifs.tellg());
    f32_ifs.seekg(0);
    if (f32_bytes % sizeof(float) != 0) {
        throw std::runtime_error("embeddings.f32 size is not a multiple of 4");
    }
    const size_t floats = f32_bytes / sizeof(float);
    if (n == 0 || floats % n != 0) {
        throw std::runtime_error("embeddings.f32 length does not match embeddings.ids");
    }
    emb.dim = static_cast<int>(floats / n);
    const int meta_dim = parse_dim_from_meta(dir);
    if (meta_dim > 0 && meta_dim != emb.dim) {
        throw std::runtime_error("dim mismatch between embeddings.meta.json and .f32");
    }

    emb.data.resize(floats);
    f32_ifs.read(reinterpret_cast<char*>(emb.data.data()),
                 static_cast<std::streamsize>(f32_bytes));
    return emb;
}

void build_and_save_hnsw(const EmbeddingSet& emb,
                         const std::string& out_dir,
                         const HnswBuildConfig& cfg) {
    if (emb.ids.empty() || emb.dim <= 0) {
        throw std::runtime_error("empty embedding set");
    }

    hnswlib::L2Space space(emb.dim);
    hnswlib::HierarchicalNSW<float> index(&space, emb.ids.size(), cfg.M,
                                          cfg.ef_construction);
    index.setEf(cfg.ef_search);

    const size_t n = emb.ids.size();
    int threads = cfg.threads > 0 ? cfg.threads
                                  : static_cast<int>(std::thread::hardware_concurrency());
    if (threads <= 0) {
        threads = 1;
    }
    if (static_cast<size_t>(threads) > n) {
        threads = static_cast<int>(n);
    }

    auto add_row = [&](size_t i) {
        const float* row = emb.data.data() + i * static_cast<size_t>(emb.dim);
        index.addPoint(row, static_cast<hnswlib::labeltype>(emb.ids[i]));
    };

    const auto t0 = std::chrono::steady_clock::now();
    // The first point becomes the entry point; insert it before going concurrent.
    add_row(0);
    if (threads == 1) {
        for (size_t i = 1; i < n; ++i) {
            add_row(i);
        }
    } else {
        std::atomic<size_t> next{1};
        std::exception_ptr error;
        std::mutex error_mu;
        std::vector<std::thread> workers;
        workers.reserve(static_cast<size_t>(threads));
        for (int t = 0; t < threads; ++t) {
            workers.emplace_back([&] {
                try {
                    for (size_t i = next.fetch_add(1); i < n; i = next.fetch_add(1)) {
                        add_row(i);
                    }
                } catch (...) {
                    std::lock_guard<std::mutex> lock(error_mu);
                    if (!error) {
                        error = std::current_exception();
                    }
                    next.store(n);
                }
            });
        }
        for (auto& w : workers) {
            w.join();
        }
        if (error) {
            std::rethrow_exception(error);
        }
    }
    const double insert_sec =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    const std::string hnsw_path = out_dir + "/vectors.hnsw";
    index.saveIndex(hnsw_path);

    std::ofstream meta(out_dir + "/vectors.meta.json");
    meta << "{\n"
         << "  \"n\": " << emb.ids.size() << ",\n"
         << "  \"dim\": " << emb.dim << ",\n"
         << "  \"metric\": \"" << cfg.metric << "\",\n"
         << "  \"M\": " << cfg.M << ",\n"
         << "  \"ef_construction\": " << cfg.ef_construction << ",\n"
         << "  \"ef_search\": " << cfg.ef_search << ",\n"
         << "  \"build_threads\": " << threads << ",\n"
         << "  \"index_file\": \"vectors.hnsw\",\n"
         << "  \"label\": \"docid\"\n"
         << "}\n";

    std::cout << "saved " << hnsw_path << " (n=" << emb.ids.size() << ", dim=" << emb.dim
              << ", threads=" << threads << ", insert_seconds=" << insert_sec << ")\n";
}

}  // namespace vector_index
