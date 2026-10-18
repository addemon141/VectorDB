#include "ExactIndexBuilder.hh"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

// Benchmark / control-group harness. Built only when EXACT_INDEX_ENABLE_BENCH=ON.
// Single-threaded baseline is behind the same macro in ExactIndexBuilder.

namespace {

void print_counts(const char* tag, int round, const exact_index::BuildResult& r) {
    std::printf("[%s round=%d] xml_files=%zu parsed=%zu kept=%zu dropped=%zu seconds=%.3f\n",
                tag, round, r.stats.xml_files, r.stats.parsed_docs, r.stats.kept_docs,
                r.stats.dropped_docs, r.stats.seconds);
}

double median(std::vector<double> v) {
    if (v.empty()) {
        return 0.0;
    }
    std::sort(v.begin(), v.end());
    const size_t mid = v.size() / 2;
    return v.size() % 2 ? v[mid] : (v[mid - 1] + v[mid]) / 2.0;
}

// Median of one phase field (span or busy) over all runs.
double median_phase(const std::vector<exact_index::BuildStats>& runs,
                    exact_index::PhaseTime exact_index::PhaseTimes::*phase,
                    double exact_index::PhaseTime::*metric) {
    std::vector<double> v;
    v.reserve(runs.size());
    for (const auto& s : runs) {
        v.push_back(s.phases.*phase.*metric);
    }
    return median(std::move(v));
}

double median_total(const std::vector<exact_index::BuildStats>& runs) {
    std::vector<double> v;
    v.reserve(runs.size());
    for (const auto& s : runs) {
        v.push_back(s.seconds);
    }
    return median(std::move(v));
}

void print_row(const char* name, bool has_st, double st_span, bool has_mt, double mt_span,
               double mt_busy, int threads) {
    std::printf("%-12s", name);
    if (has_st) {
        std::printf(" %10.3f", st_span);
    } else {
        std::printf(" %10s", "-");
    }
    if (has_mt) {
        std::printf(" %10.3f %10.3f", mt_span, mt_busy);
    } else {
        std::printf(" %10s %10s", "-", "-");
    }
    if (has_st && has_mt && mt_span > 0.0) {
        std::printf(" %9.2fx", st_span / mt_span);
    } else {
        std::printf(" %10s", "-");
    }
    if (has_mt && mt_span > 0.0 && mt_busy > 0.0) {
        std::printf(" %9.1f%%", 100.0 * mt_busy / (mt_span * threads));
    } else {
        std::printf(" %10s", "-");
    }
    std::printf("\n");
}

void usage(const char* argv0) {
    std::cerr << "Usage:\n  " << argv0
              << " --xml-dir DIR --out-dir DIR [--threads N] [--mode mt|st|both] [--rounds R]\n"
              << "Defaults: --threads=hardware_concurrency, --mode=both, --rounds=3\n"
              << "Rounds alternate st/mt order to cancel page-cache bias; the table shows\n"
              << "per-phase medians. mt phases overlap, so their spans do not sum to total.\n"
              << "Note: requires cmake -DEXACT_INDEX_ENABLE_BENCH=ON\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::string xml_dir;
    std::string out_dir;
    std::string mode = "both";
    int rounds = 3;
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
        } else if (arg == "--mode") {
            mode = need("--mode");
        } else if (arg == "--rounds") {
            rounds = std::stoi(need("--rounds"));
        } else if (arg == "-h" || arg == "--help") {
            usage(argv[0]);
            return 0;
        } else {
            std::cerr << "unknown arg: " << arg << "\n";
            usage(argv[0]);
            return 2;
        }
    }

    if (xml_dir.empty() || out_dir.empty() || rounds <= 0 ||
        (mode != "st" && mode != "mt" && mode != "both")) {
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

    std::cout << "xml_dir=" << xml_dir << " out_dir=" << out_dir << " threads=" << threads
              << " mode=" << mode << " rounds=" << rounds << std::endl;

    const bool run_st = mode == "st" || mode == "both";
    const bool run_mt = mode == "mt" || mode == "both";
    std::vector<exact_index::BuildStats> st_runs;
    std::vector<exact_index::BuildStats> mt_runs;

    auto do_st = [&](int round) {
        auto r = exact_index::build_exact_index_st(xml_dir, out_dir + "/st", paths);
        print_counts("single-thread", round, r);
        st_runs.push_back(r.stats);
    };
    auto do_mt = [&](int round) {
        auto r = exact_index::build_exact_index_mt(xml_dir, out_dir + "/mt", paths, threads);
        print_counts("multi-thread", round, r);
        mt_runs.push_back(r.stats);
    };

    try {
        for (int round = 0; round < rounds; ++round) {
            const bool st_first = round % 2 == 0;
            if (st_first && run_st) {
                do_st(round);
            }
            if (run_mt) {
                do_mt(round);
            }
            if (!st_first && run_st) {
                do_st(round);
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "exact_index_bench failed: " << e.what() << "\n";
        return 1;
    }

    std::printf("\nper-phase medians over %d round(s), seconds (threads=%d)\n", rounds,
                threads);
    std::printf("%-12s %10s %10s %10s %10s %10s\n", "phase", "st_span", "mt_span", "mt_busy",
                "speedup", "par_eff");
    for (const auto& f : exact_index::phase_fields()) {
        const double st_span =
            run_st ? median_phase(st_runs, f.field, &exact_index::PhaseTime::span) : 0.0;
        const double mt_span =
            run_mt ? median_phase(mt_runs, f.field, &exact_index::PhaseTime::span) : 0.0;
        const double mt_busy =
            run_mt ? median_phase(mt_runs, f.field, &exact_index::PhaseTime::busy) : 0.0;
        print_row(f.name, run_st, st_span, run_mt, mt_span, mt_busy, threads);
    }
    const double st_total = run_st ? median_total(st_runs) : 0.0;
    const double mt_total = run_mt ? median_total(mt_runs) : 0.0;
    print_row("total", run_st, st_total, run_mt, mt_total, 0.0, threads);

    if (run_st && run_mt && mt_total > 0.0) {
        std::printf("[speedup] st/mt = %.2fx\n", st_total / mt_total);
    }
    return 0;
}
