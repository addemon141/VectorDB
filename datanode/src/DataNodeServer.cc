#include "DataNodeServer.hh"

#include "tlv/file_io.hh"
#include "tlv/hash_util.hh"
#include "tlv/socket_io.hh"

#include <nlohmann/json.hpp>

#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace vdb::datanode {

DataNodeServer::DataNodeServer(std::string listen_addr, std::string data_dir, BuildConfig cfg)
    : listen_addr_(std::move(listen_addr)),
      data_dir_(std::move(data_dir)),
      cfg_(std::move(cfg)) {}

DataNodeServer::~DataNodeServer() { stop(); }

void DataNodeServer::persist_job_locked() {
    json j = {{"job_id", job_id_},
              {"received_offset", corpus_received_},
              {"total_len", corpus_total_},
              {"state", state_},
              {"content_hash", content_hash_},
              {"error", last_error_}};
    std::ofstream ofs(job_meta_path_);
    ofs << j.dump();
}

void DataNodeServer::load_job() {
    if (!fs::exists(job_meta_path_)) {
        return;
    }
    std::ifstream ifs(job_meta_path_);
    auto j = json::parse(ifs, nullptr, false);
    if (j.is_discarded()) {
        return;
    }
    std::lock_guard<std::mutex> lk(mu_);
    job_id_ = j.value("job_id", "");
    corpus_received_ = j.value("received_offset", 0ull);
    corpus_total_ = j.value("total_len", 0ull);
    state_ = j.value("state", "idle");
    content_hash_ = j.value("content_hash", "");
    last_error_ = j.value("error", "");
    std::cerr << "[datanode] loaded job id=" << job_id_ << " state=" << state_
              << " offset=" << corpus_received_ << "\n";
}

bool DataNodeServer::start(std::string* err) {
    fs::create_directories(data_dir_);
    corpus_path_ = data_dir_ + "/corpus.json";
    snapshot_dir_ = data_dir_ + "/snapshot";
    job_meta_path_ = data_dir_ + "/job.json";
    load_job();
    listen_fd_ = tlv::Conn::listen_port(listen_addr_, err);
    if (listen_fd_ < 0) {
        return false;
    }
    stopped_ = false;
    acceptor_ = std::thread(&DataNodeServer::accept_loop, this);
    std::cerr << "[datanode] listening on " << listen_addr_ << " data_dir=" << data_dir_ << "\n";
    return true;
}

void DataNodeServer::stop() {
    stopped_ = true;
    if (listen_fd_ >= 0) {
        ::shutdown(listen_fd_, SHUT_RDWR);
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
    if (acceptor_.joinable()) {
        acceptor_.join();
    }
}

void DataNodeServer::accept_loop() {
    while (!stopped_) {
        std::string err;
        auto c = tlv::Conn::accept_one(listen_fd_, &err);
        if (!c.valid()) {
            if (stopped_) {
                break;
            }
            continue;
        }
        const int fd = c.fd();
        // 移交 fd：Conn 析构会 close，这里用 release 手法——再包一层
        int dupfd = ::dup(fd);
        c.close();
        std::thread(&DataNodeServer::handle_conn, this, dupfd).detach();
    }
}

void DataNodeServer::handle_conn(int fd) {
    tlv::Conn conn(fd);
    std::string err;
    std::ofstream corpus_ofs;
    uint64_t corpus_expected = 0;
    uint64_t corpus_got = 0;

    auto send_err = [&](const std::string& msg) {
        tlv::Frame f;
        f.type = tlv::MsgType::Error;
        f.payload = json{{"error", msg}}.dump();
        conn.send_frame(f, &err);
    };

    while (true) {
        tlv::Frame req;
        if (!conn.recv_frame(&req, &err)) {
            break;
        }
        switch (req.type) {
            case tlv::MsgType::StatusReq: {
                json st;
                {
                    std::lock_guard<std::mutex> lk(mu_);
                    st = {{"state", state_},
                          {"content_hash", content_hash_},
                          {"error", last_error_},
                          {"snapshot_dir", snapshot_dir_},
                          {"job",
                           {{"job_id", job_id_},
                            {"received_offset", corpus_received_},
                            {"total_len", corpus_total_},
                            {"phase", state_}}}};
                }
                tlv::Frame resp;
                resp.type = tlv::MsgType::StatusResp;
                resp.payload = st.dump();
                if (!conn.send_frame(resp, &err)) {
                    return;
                }
                break;
            }
            case tlv::MsgType::PutCorpusBegin: {
                json meta = json::parse(req.payload, nullptr, false);
                if (meta.is_discarded() || !meta.contains("total_len")) {
                    send_err("bad PutCorpusBegin");
                    return;
                }
                corpus_expected = meta["total_len"].get<uint64_t>();
                const uint64_t resume = meta.value("resume_offset", 0ull);
                const std::string job_id = meta.value("job_id", "");
                {
                    std::lock_guard<std::mutex> lk(mu_);
                    state_ = "receiving";
                    job_id_ = job_id;
                    corpus_total_ = corpus_expected;
                    if (resume == 0) {
                        content_hash_.clear();
                        last_error_.clear();
                        fs::remove_all(snapshot_dir_);
                        corpus_got = 0;
                        corpus_ofs = std::ofstream(corpus_path_, std::ios::binary | std::ios::trunc);
                    } else {
                        // RESUME：沿用已收字节（优先本地已持久化的 offset）
                        corpus_got = std::max(resume, corpus_received_);
                        if (job_id == job_id_ && corpus_received_ > 0) {
                            corpus_got = corpus_received_;
                        }
                        corpus_ofs = std::ofstream(corpus_path_, std::ios::binary | std::ios::app);
                    }
                    corpus_received_ = corpus_got;
                    persist_job_locked();
                }
                if (!corpus_ofs) {
                    send_err("open corpus failed");
                    return;
                }
                tlv::Frame ack;
                ack.type = tlv::MsgType::PutCorpusAck;
                ack.payload = json{{"ok", true}, {"offset", corpus_got}}.dump();
                conn.send_frame(ack, &err);
                break;
            }
            case tlv::MsgType::PutCorpusChunk: {
                if (!corpus_ofs) {
                    send_err("chunk without begin");
                    return;
                }
                if (req.offset != corpus_got) {
                    send_err("unexpected offset");
                    return;
                }
                corpus_ofs.write(req.payload.data(),
                                 static_cast<std::streamsize>(req.payload.size()));
                corpus_got += req.payload.size();
                {
                    std::lock_guard<std::mutex> lk(mu_);
                    corpus_received_ = corpus_got;
                    persist_job_locked();
                }
                tlv::Frame ack;
                ack.type = tlv::MsgType::PutCorpusAck;
                ack.payload = json{{"ok", true}, {"offset", corpus_got}}.dump();
                conn.send_frame(ack, &err);
                break;
            }
            case tlv::MsgType::PutCorpusEnd: {
                corpus_ofs.close();
                if (corpus_got != corpus_expected) {
                    send_err("incomplete corpus");
                    return;
                }
                {
                    std::lock_guard<std::mutex> lk(mu_);
                    state_ = "received";
                    corpus_received_ = corpus_got;
                    persist_job_locked();
                }
                tlv::Frame ack;
                ack.type = tlv::MsgType::PutCorpusAck;
                ack.payload = json{{"ok", true}, {"bytes", corpus_got}}.dump();
                conn.send_frame(ack, &err);
                break;
            }
            case tlv::MsgType::BuildReq: {
                {
                    std::lock_guard<std::mutex> lk(mu_);
                    state_ = "building";
                    persist_job_locked();
                }
                std::ifstream ifs(corpus_path_);
                std::string corpus((std::istreambuf_iterator<char>(ifs)),
                                   std::istreambuf_iterator<char>());
                std::string hash, build_err;
                const bool ok =
                    build_snapshot_from_json(corpus, snapshot_dir_, cfg_, &hash, &build_err);
                tlv::Frame resp;
                resp.type = tlv::MsgType::BuildResp;
                if (!ok) {
                    std::lock_guard<std::mutex> lk(mu_);
                    state_ = "error";
                    last_error_ = build_err;
                    persist_job_locked();
                    resp.payload = json{{"ok", false}, {"error", build_err}}.dump();
                } else {
                    std::lock_guard<std::mutex> lk(mu_);
                    state_ = "ready";
                    content_hash_ = hash;
                    persist_job_locked();
                    resp.payload =
                        json{{"ok", true}, {"content_hash", hash}, {"base_version", 1}}.dump();
                }
                conn.send_frame(resp, &err);
                break;
            }
            case tlv::MsgType::SnapshotMetaReq: {
                std::string hash, snap;
                {
                    std::lock_guard<std::mutex> lk(mu_);
                    hash = content_hash_;
                    snap = snapshot_dir_;
                }
                if (hash.empty() || !fs::exists(snap)) {
                    send_err("no snapshot");
                    return;
                }
                json files = json::array();
                uint64_t total = 0;
                for (auto it = fs::recursive_directory_iterator(snap);
                     it != fs::recursive_directory_iterator(); ++it) {
                    if (!it->is_regular_file()) {
                        continue;
                    }
                    const auto rel = fs::relative(it->path(), snap).generic_string();
                    const auto sz = static_cast<uint64_t>(it->file_size());
                    files.push_back({{"path", rel}, {"size", sz}});
                    total += sz;
                }
                tlv::Frame resp;
                resp.type = tlv::MsgType::SnapshotMetaResp;
                resp.payload =
                    json{{"content_hash", hash}, {"total_bytes", total}, {"files", files}}.dump();
                conn.send_frame(resp, &err);
                break;
            }
            case tlv::MsgType::SnapshotChunkReq: {
                json meta = json::parse(req.payload, nullptr, false);
                if (meta.is_discarded() || !meta.contains("path")) {
                    send_err("bad SnapshotChunkReq");
                    return;
                }
                const std::string rel = meta["path"].get<std::string>();
                const uint64_t offset = meta.value("offset", 0ull);
                const uint32_t want = meta.value("length", static_cast<uint32_t>(tlv::kDefaultChunk));
                std::string snap;
                {
                    std::lock_guard<std::mutex> lk(mu_);
                    snap = snapshot_dir_;
                }
                const fs::path path = fs::path(snap) / rel;
                if (!fs::exists(path)) {
                    send_err("missing file " + rel);
                    return;
                }
                std::string buf, ioerr;
                if (tlv::file_pread(path.string(), offset, want, &buf, &ioerr) < 0) {
                    send_err(ioerr);
                    return;
                }
                tlv::Frame resp;
                resp.type = tlv::MsgType::SnapshotChunkResp;
                resp.total_len = static_cast<uint64_t>(fs::file_size(path));
                resp.offset = offset;
                resp.payload = std::move(buf);
                conn.send_frame(resp, &err);
                break;
            }
            default:
                send_err("unsupported type");
                return;
        }
    }
}

}  // namespace vdb::datanode
