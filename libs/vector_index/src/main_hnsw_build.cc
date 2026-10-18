#include "HnswBuilder.hh"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void usage(const char* argv0) {
    std::cerr
        << "Usage:\n  " << argv0
        << " --emb-dir DIR --out-dir DIR [--M N] [--ef-construction N] [--ef-search N]"
           " [--threads N]\n"
        << "--threads: concurrent insert threads (default: hardware_concurrency)\n"
        << "Reads embeddings.f32 + embeddings.ids from --emb-dir;\n"
        << "writes vectors.hnsw + vectors.meta.json to --out-dir.\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::string emb_dir;
    std::string out_dir;
    vector_index::HnswBuildConfig cfg;

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
        } else if (arg == "--out-dir") {
            out_dir = need("--out-dir");
        } else if (arg == "--M") {
            cfg.M = std::stoi(need("--M"));
        } else if (arg == "--ef-construction") {
            cfg.ef_construction = std::stoi(need("--ef-construction"));
        } else if (arg == "--ef-search") {
            cfg.ef_search = std::stoi(need("--ef-search"));
        } else if (arg == "--threads") {
            cfg.threads = std::stoi(need("--threads"));
        } else if (arg == "-h" || arg == "--help") {
            usage(argv[0]);
            return 0;
        } else {
            std::cerr << "unknown arg: " << arg << "\n";
            usage(argv[0]);
            return 2;
        }
    }

    if (emb_dir.empty() || out_dir.empty()) {
        usage(argv[0]);
        return 2;
    }

    try {
        auto emb = vector_index::load_embeddings(emb_dir);
        // Allow writing into the same directory as embeddings.
        vector_index::build_and_save_hnsw(emb, out_dir, cfg);
    } catch (const std::exception& ex) {
        std::cerr << "hnsw_build failed: " << ex.what() << "\n";
        return 1;
    }
    return 0;
}
