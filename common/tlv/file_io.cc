#include "tlv/file_io.hh"

#include <cstdio>
#include <cstring>
#include <filesystem>

#if defined(VDB_IO_URING) && VDB_IO_URING
#include <liburing.h>
#include <unistd.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace vdb::tlv {
namespace fs = std::filesystem;

#if defined(VDB_IO_URING) && VDB_IO_URING

ssize_t file_pread(const std::string& path, uint64_t offset, size_t want, std::string* out,
                   std::string* err) {
    FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) {
        *err = "fopen " + path;
        return -1;
    }
    const int fd = fileno(fp);
    out->assign(want, '\0');
    io_uring ring{};
    if (io_uring_queue_init(2, &ring, 0) < 0) {
        std::fclose(fp);
        *err = "io_uring_queue_init";
        return -1;
    }
    io_uring_sqe* sqe = io_uring_get_sqe(&ring);
    io_uring_prep_read(sqe, fd, out->data(), want, static_cast<__u64>(offset));
    io_uring_submit(&ring);
    io_uring_cqe* cqe = nullptr;
    if (io_uring_wait_cqe(&ring, &cqe) < 0 || cqe->res < 0) {
        *err = "io_uring read failed";
        io_uring_queue_exit(&ring);
        std::fclose(fp);
        return -1;
    }
    const ssize_t n = cqe->res;
    io_uring_cqe_seen(&ring, cqe);
    io_uring_queue_exit(&ring);
    std::fclose(fp);
    out->resize(static_cast<size_t>(n));
    return n;
}

bool file_pwrite(const std::string& path, uint64_t offset, const std::string& data, bool trunc_first,
                 std::string* err) {
    fs::create_directories(fs::path(path).parent_path());
    const char* mode = (trunc_first && offset == 0) ? "wb" : "r+b";
    FILE* fp = std::fopen(path.c_str(), mode);
    if (!fp && !(trunc_first && offset == 0)) {
        fp = std::fopen(path.c_str(), "wb");
    }
    if (!fp) {
        *err = "fopen write " + path;
        return false;
    }
    if (!(trunc_first && offset == 0)) {
        // ensure file large enough: open r+b may fail if missing — already handled
    }
    const int fd = fileno(fp);
    io_uring ring{};
    if (io_uring_queue_init(2, &ring, 0) < 0) {
        std::fclose(fp);
        *err = "io_uring_queue_init";
        return false;
    }
    io_uring_sqe* sqe = io_uring_get_sqe(&ring);
    io_uring_prep_write(sqe, fd, data.data(), data.size(), static_cast<__u64>(offset));
    io_uring_submit(&ring);
    io_uring_cqe* cqe = nullptr;
    bool ok = true;
    if (io_uring_wait_cqe(&ring, &cqe) < 0 || cqe->res < 0 ||
        static_cast<size_t>(cqe->res) != data.size()) {
        *err = "io_uring write failed";
        ok = false;
    }
    if (cqe) {
        io_uring_cqe_seen(&ring, cqe);
    }
    io_uring_queue_exit(&ring);
    std::fclose(fp);
    return ok;
}

#else

ssize_t file_pread(const std::string& path, uint64_t offset, size_t want, std::string* out,
                   std::string* err) {
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        *err = "open " + path;
        return -1;
    }
    out->assign(want, '\0');
    const ssize_t n = ::pread(fd, out->data(), want, static_cast<off_t>(offset));
    ::close(fd);
    if (n < 0) {
        *err = "pread failed";
        return -1;
    }
    out->resize(static_cast<size_t>(n));
    return n;
}

bool file_pwrite(const std::string& path, uint64_t offset, const std::string& data, bool trunc_first,
                 std::string* err) {
    fs::create_directories(fs::path(path).parent_path());
    int flags = O_WRONLY | O_CREAT;
    if (trunc_first && offset == 0) {
        flags |= O_TRUNC;
    }
    const int fd = ::open(path.c_str(), flags, 0644);
    if (fd < 0) {
        *err = "open write " + path;
        return false;
    }
    const ssize_t n = ::pwrite(fd, data.data(), data.size(), static_cast<off_t>(offset));
    ::close(fd);
    if (n < 0 || static_cast<size_t>(n) != data.size()) {
        *err = "pwrite failed";
        return false;
    }
    return true;
}

#endif

}  // namespace vdb::tlv
