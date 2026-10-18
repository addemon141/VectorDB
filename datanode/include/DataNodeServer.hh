#pragma once

#include "IndexBuilder.hh"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

namespace vdb::datanode {

class DataNodeServer {
public:
    DataNodeServer(std::string listen_addr, std::string data_dir, BuildConfig cfg);
    ~DataNodeServer();

    bool start(std::string* err);
    void stop();

private:
    void accept_loop();
    void handle_conn(int fd);
    void persist_job_locked();
    void load_job();

    std::string listen_addr_;
    std::string data_dir_;
    BuildConfig cfg_;
    int listen_fd_ = -1;
    std::atomic<bool> stopped_{false};
    std::thread acceptor_;

    std::mutex mu_;
    std::string corpus_path_;
    std::string snapshot_dir_;
    std::string job_meta_path_;
    std::string content_hash_;
    std::string state_ = "idle";  // idle|receiving|received|building|ready|error
    std::string last_error_;
    std::string job_id_;
    uint64_t corpus_received_ = 0;
    uint64_t corpus_total_ = 0;
};

}  // namespace vdb::datanode
