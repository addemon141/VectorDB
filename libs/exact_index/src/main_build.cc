#include "ExactIndexBuilder.hh"

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <thread>

namespace {

void print_stats(const exact_index::BuildResult& r) {
    std::cout << "[exact_index]\n"
              << "  xml_files   : " << r.stats.xml_files << "\n"
              << "  parsed_docs : " << r.stats.parsed_docs << "\n"
              << "  kept_docs   : " << r.stats.kept_docs << "\n"
              << "  dropped_docs: " << r.stats.dropped_docs << "\n"
              << "  seconds     : " << r.stats.seconds << "\n"
              << "  phases (span / busy, seconds):\n";
    for (const auto& f : exact_index::phase_fields()) {
        const auto& p = r.stats.phases.*(f.field);
        std::printf("    %-12s: %8.3f / %8.3f\n", f.name, p.span, p.busy);
    }
}

void usage(const char* argv0) {
    std::cerr << "Usage:\n  " << argv0
              << " --xml-dir DIR --out-dir DIR [--threads N]\n";
}

}  // namespace

// Production entry: multi-threaded build only.
int main(int argc, char** argv) {
    std::string xml_dir;
    std::string out_dir;
    int threads = static_cast<int>(std::thread::hardware_concurrency());
    if (threads <= 0) {
        threads = 4;
    }

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto need = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "missing value for " << name << "\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--xml-dir") {
            xml_dir = need("--xml-dir");
        } else if (arg == "--out-dir") {
            out_dir = need("--out-dir");
        } else if (arg == "--threads") {
            threads = std::stoi(need("--threads"));
        } else if (arg == "-h" || arg == "--help") {
            usage(argv[0]);
            return 0;
        } else {
            std::cerr << "unknown arg: " << arg << "\n";
            usage(argv[0]);
            return 2;
        }
    }

    if (xml_dir.empty() || out_dir.empty()) {
        usage(argv[0]);
        return 2;
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

    exact_index::BuildResult r;
    try {
        r = exact_index::build_exact_index_mt(xml_dir, out_dir, paths, threads);
    } catch (const std::exception& e) {
        std::cerr << "exact_index_build failed: " << e.what() << "\n";
        return 1;
    }
    print_stats(r);
    return r.stats.kept_docs > 0 ? 0 : 1;
}
