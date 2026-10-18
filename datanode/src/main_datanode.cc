#include "DataNodeServer.hh"

#include <csignal>
#include <iostream>
#include <string>

namespace {

void usage(const char* argv0) {
    std::cerr << "Usage: " << argv0
              << " --listen host:port --data-dir DIR [--embed-url URL]\n"
                 "       [--cn-stop FILE] [--en-stop FILE] [--workers N]\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::string listen = "0.0.0.0:6100";
    std::string data_dir;
    vdb::datanode::BuildConfig cfg;
    cfg.text_paths.cn_stopwords = "testdata/stopwords/cn_stopwords.txt";
    cfg.text_paths.en_stopwords = "testdata/stopwords/en_stopwords.txt";

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto need = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "missing value for " << arg << "\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--listen") {
            listen = need();
        } else if (arg == "--data-dir") {
            data_dir = need();
        } else if (arg == "--embed-url") {
            cfg.embed_url = need();
        } else if (arg == "--cn-stop") {
            cfg.text_paths.cn_stopwords = need();
        } else if (arg == "--en-stop") {
            cfg.text_paths.en_stopwords = need();
        } else if (arg == "--workers") {
            cfg.workers = std::stoi(need());
        } else if (arg == "-h" || arg == "--help") {
            usage(argv[0]);
            return 0;
        } else {
            std::cerr << "unknown arg " << arg << "\n";
            usage(argv[0]);
            return 2;
        }
    }
    if (data_dir.empty()) {
        usage(argv[0]);
        return 2;
    }

    sigset_t sigs;
    sigemptyset(&sigs);
    sigaddset(&sigs, SIGINT);
    sigaddset(&sigs, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &sigs, nullptr);

    vdb::datanode::DataNodeServer srv(listen, data_dir, cfg);
    std::string err;
    if (!srv.start(&err)) {
        std::cerr << err << "\n";
        return 1;
    }
    int sig = 0;
    sigwait(&sigs, &sig);
    std::cerr << "signal " << sig << ", shutting down\n";
    srv.stop();
    return 0;
}
