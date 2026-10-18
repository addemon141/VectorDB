#pragma once

#include "tlv/tlv.hh"

#include <string>

namespace vdb::tlv {

class Conn {
public:
    explicit Conn(int fd = -1) : fd_(fd) {}
    ~Conn();
    Conn(const Conn&) = delete;
    Conn& operator=(const Conn&) = delete;
    Conn(Conn&& o) noexcept : fd_(o.fd_), rbuf_(std::move(o.rbuf_)) { o.fd_ = -1; }
    Conn& operator=(Conn&& o) noexcept;

    int fd() const { return fd_; }
    bool valid() const { return fd_ >= 0; }
    void close();

    bool send_frame(const Frame& f, std::string* err);
    bool recv_frame(Frame* f, std::string* err);

    // 客户端：连接 host:port
    static Conn connect(const std::string& host_port, std::string* err);
    // 服务端：listen / accept
    static int listen_port(const std::string& host_port, std::string* err);
    static Conn accept_one(int listen_fd, std::string* err);

private:
    int fd_ = -1;
    std::string rbuf_;
    bool write_all(const char* p, size_t n, std::string* err);
};

}  // namespace vdb::tlv
