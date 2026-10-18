#include "ServerConfig.hh"
#include "api/HttpApi.hh"
#include "index/EmbedClient.hh"
#include "raft/RaftNode.hh"
#include "raft/RaftServiceImpl.hh"
#include "sm/QueryEngine.hh"
#include "sm/StateMachine.hh"
#include "tlv/hash_util.hh"

#include <grpcpp/grpcpp.h>

#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;

namespace {

void usage(const char* argv0) {
    std::cerr
        << "Usage: " << argv0
        << " --node-id N --peers 1=host:raftport,... --http-addr host:port\n"
           "       --data-dir DIR --datanode host:port [--datanode-backup host:port]\n"
           "       [--http-peers 1=host:httpport,...] [--raft-dir DIR] [--embed-url URL]\n"
           "       [--cn-stop FILE] [--en-stop FILE]\n"
           "  --peers 为各节点 Raft gRPC 地址（含自己）。\n"
           "  多节点必须提供 --http-peers（Init 时 Leader 通知各节点从 DataNode 拉快照）。\n"
           "  空库启动：不要求 data-dir/base 已存在；Init 后写入 base。\n";
}

bool parse_id_addr(const std::string& s, std::vector<std::pair<int32_t, std::string>>* out) {
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ',')) {
        const auto eq = item.find('=');
        if (eq == std::string::npos) {
            return false;
        }
        int32_t id = 0;
        try {
            id = std::stoi(item.substr(0, eq));
        } catch (...) {
            return false;
        }
        const std::string addr = item.substr(eq + 1);
        if (id <= 0 || addr.empty()) {
            return false;
        }
        out->emplace_back(id, addr);
    }
    return !out->empty();
}

}  // namespace

int main(int argc, char** argv) {
    vdb::ServerConfig cfg;
    cfg.text_paths.cn_stopwords = "testdata/stopwords/cn_stopwords.txt";
    cfg.text_paths.en_stopwords = "testdata/stopwords/en_stopwords.txt";
    std::string peers;
    std::string http_peers;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto need = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "missing value for " << arg << "\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--node-id") {
            cfg.raft.node_id = std::stoi(need());
        } else if (arg == "--peers") {
            peers = need();
        } else if (arg == "--http-peers") {
            http_peers = need();
        } else if (arg == "--http-addr") {
            cfg.http_addr = need();
        } else if (arg == "--data-dir") {
            cfg.data_dir = need();
        } else if (arg == "--raft-dir") {
            cfg.raft.dir = need();
        } else if (arg == "--datanode") {
            cfg.datanode_addr = need();
        } else if (arg == "--datanode-backup") {
            cfg.datanode_backup = need();
        } else if (arg == "--embed-url") {
            cfg.embed_url = need();
        } else if (arg == "--cn-stop") {
            cfg.text_paths.cn_stopwords = need();
        } else if (arg == "--en-stop") {
            cfg.text_paths.en_stopwords = need();
        } else if (arg == "-h" || arg == "--help") {
            usage(argv[0]);
            return 0;
        } else {
            std::cerr << "unknown argument " << arg << "\n";
            usage(argv[0]);
            return 2;
        }
    }

    std::vector<std::pair<int32_t, std::string>> peer_list;
    if (cfg.raft.node_id <= 0 || cfg.data_dir.empty() || cfg.http_addr.empty() ||
        !parse_id_addr(peers, &peer_list)) {
        usage(argv[0]);
        return 2;
    }
    for (const auto& [id, addr] : peer_list) {
        vdb::raft::PeerInfo p;
        p.id = id;
        p.address = addr;
        cfg.raft.peers.push_back(p);
        if (id == cfg.raft.node_id) {
            cfg.raft_addr = addr;
        }
    }
    if (cfg.raft_addr.empty()) {
        std::cerr << "node-id not in --peers\n";
        return 2;
    }

    if (http_peers.empty()) {
        if (cfg.raft.peers.size() > 1) {
            std::cerr << "multi-node requires --http-peers 1=host:port,...\n";
            return 2;
        }
        cfg.peer_http.push_back({cfg.raft.node_id, cfg.http_addr});
    } else {
        std::vector<std::pair<int32_t, std::string>> http_list;
        if (!parse_id_addr(http_peers, &http_list)) {
            std::cerr << "bad --http-peers\n";
            return 2;
        }
        for (const auto& [id, addr] : http_list) {
            cfg.peer_http.push_back({id, addr});
        }
    }

    if (cfg.raft.dir.empty()) {
        cfg.raft.dir = cfg.data_dir + "/raft_n" + std::to_string(cfg.raft.node_id);
    }
    fs::create_directories(cfg.data_dir);
    fs::create_directories(cfg.raft.dir);

    sigset_t sigs;
    sigemptyset(&sigs);
    sigaddset(&sigs, SIGINT);
    sigaddset(&sigs, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &sigs, nullptr);

    std::string err;
    vdb::Tokenizer tokenizer;
    if (!tokenizer.load(cfg.text_paths, &err)) {
        std::cerr << err << "\n";
        return 1;
    }
    vdb::StateMachine sm(&tokenizer);
    sm.reset_empty();

    const std::string base_dir = cfg.data_dir + "/base";
    if (fs::exists(base_dir + "/docs.data")) {
        std::string hash;
        std::ifstream hf(base_dir + "/SNAPSHOT.hash");
        if (hf) {
            std::getline(hf, hash);
        }
        if (hash.empty()) {
            hash = vdb::tlv::snapshot_hash(base_dir, &err);
        }
        if (!hash.empty() && sm.load_base(base_dir, hash, &err)) {
            std::cerr << "[service] preloaded base hash=" << hash << "\n";
        } else {
            std::cerr << "[service] base present but load failed: " << err << "\n";
            sm.reset_empty();
        }
    }

    vdb::EmbedClient embed(cfg.embed_url);
    vdb::QueryEngine engine(&sm, &tokenizer, &embed, cfg.query);
    vdb::raft::RaftNode raft(cfg.raft, &sm);
    if (!raft.start(&err)) {
        std::cerr << "raft start failed: " << err << "\n";
        return 1;
    }

    vdb::raft::RaftServiceImpl raft_svc(&raft);
    grpc::ServerBuilder raft_b;
    raft_b.AddChannelArgument(GRPC_ARG_ALLOW_REUSEPORT, 0);
    raft_b.AddListeningPort(cfg.raft_addr, grpc::InsecureServerCredentials());
    raft_b.RegisterService(&raft_svc);
    raft_b.SetMaxReceiveMessageSize(64 << 20);
    auto raft_server = raft_b.BuildAndStart();
    if (!raft_server) {
        std::cerr << "raft listen failed on " << cfg.raft_addr << "\n";
        raft.stop();
        return 1;
    }

    vdb::HttpApi http(cfg, &raft, &sm, &engine, &embed);
    std::thread http_thread([&] { http.start(); });

    std::cout << "vectordb_service node=" << cfg.raft.node_id << " http=" << cfg.http_addr
              << " raft=" << cfg.raft_addr << " datanode=" << cfg.datanode_addr << std::endl;

    int sig = 0;
    sigwait(&sigs, &sig);
    std::cout << "signal " << sig << ", shutting down" << std::endl;

    http.stop();
    if (http_thread.joinable()) {
        http_thread.join();
    }
    const auto deadline = std::chrono::system_clock::now() + std::chrono::seconds(2);
    raft.stop();
    raft_server->Shutdown(deadline);
    return 0;
}
