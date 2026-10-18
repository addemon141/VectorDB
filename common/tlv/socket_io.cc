#include "tlv/socket_io.hh"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>

namespace vdb::tlv {
namespace {

bool split_host_port(const std::string& hp, std::string* host, int* port) {
    const auto colon = hp.rfind(':');
    if (colon == std::string::npos) {
        return false;
    }
    *host = hp.substr(0, colon);
    try {
        *port = std::stoi(hp.substr(colon + 1));
    } catch (...) {
        return false;
    }
    return *port > 0 && *port < 65536;
}

}  // namespace

Conn::~Conn() { close(); }

Conn& Conn::operator=(Conn&& o) noexcept {
    if (this != &o) {
        close();
        fd_ = o.fd_;
        rbuf_ = std::move(o.rbuf_);
        o.fd_ = -1;
    }
    return *this;
}

void Conn::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    rbuf_.clear();
}

bool Conn::write_all(const char* p, size_t n, std::string* err) {
    size_t off = 0;
    while (off < n) {
        const ssize_t w = ::send(fd_, p + off, n - off, MSG_NOSIGNAL);
        if (w <= 0) {
            *err = "send failed";
            return false;
        }
        off += static_cast<size_t>(w);
    }
    return true;
}

bool Conn::send_frame(const Frame& f, std::string* err) {
    std::string bytes;
    if (!encode_frame(f, &bytes, err)) {
        return false;
    }
    return write_all(bytes.data(), bytes.size(), err);
}

bool Conn::recv_frame(Frame* f, std::string* err) {
    while (true) {
        size_t consumed = 0;
        if (decode_frame(rbuf_, &consumed, f, err)) {
            rbuf_.erase(0, consumed);
            return true;
        }
        if (!err->empty()) {
            return false;
        }
        char buf[64 * 1024];
        const ssize_t n = ::recv(fd_, buf, sizeof(buf), 0);
        if (n <= 0) {
            *err = "recv failed or eof";
            return false;
        }
        rbuf_.append(buf, static_cast<size_t>(n));
    }
}

Conn Conn::connect(const std::string& host_port, std::string* err) {
    std::string host;
    int port = 0;
    if (!split_host_port(host_port, &host, &port)) {
        *err = "bad address " + host_port;
        return Conn{};
    }
    addrinfo hints{};
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    addrinfo* res = nullptr;
    const int rc = getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res);
    if (rc != 0) {
        *err = "getaddrinfo: " + std::string(gai_strerror(rc));
        return Conn{};
    }
    int fd = -1;
    for (addrinfo* ai = res; ai; ai = ai->ai_next) {
        fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
            break;
        }
        ::close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) {
        *err = "connect failed: " + host_port;
        return Conn{};
    }
    return Conn{fd};
}

int Conn::listen_port(const std::string& host_port, std::string* err) {
    std::string host;
    int port = 0;
    if (!split_host_port(host_port, &host, &port)) {
        *err = "bad listen addr " + host_port;
        return -1;
    }
    addrinfo hints{};
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    hints.ai_flags = AI_PASSIVE;
    addrinfo* res = nullptr;
    const char* node = (host == "0.0.0.0" || host.empty()) ? nullptr : host.c_str();
    if (getaddrinfo(node, std::to_string(port).c_str(), &hints, &res) != 0) {
        *err = "getaddrinfo listen failed";
        return -1;
    }
    int fd = -1;
    for (addrinfo* ai = res; ai; ai = ai->ai_next) {
        fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            continue;
        }
        int yes = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        if (::bind(fd, ai->ai_addr, ai->ai_addrlen) == 0 && ::listen(fd, 16) == 0) {
            break;
        }
        ::close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) {
        *err = "listen failed: " + host_port;
    }
    return fd;
}

Conn Conn::accept_one(int listen_fd, std::string* err) {
    const int fd = ::accept(listen_fd, nullptr, nullptr);
    if (fd < 0) {
        *err = "accept failed";
        return Conn{};
    }
    return Conn{fd};
}

}  // namespace vdb::tlv
