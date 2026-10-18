#include "raft/RaftLog.hh"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace vdb::raft {
namespace fs = std::filesystem;

namespace {

bool write_all(int fd, const char* buf, size_t n) {
    while (n > 0) {
        const ssize_t w = ::write(fd, buf, n);
        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        buf += w;
        n -= static_cast<size_t>(w);
    }
    return true;
}

bool fsync_dir(const std::string& dir) {
    const int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
    if (dfd < 0) {
        return false;
    }
    const bool ok = ::fsync(dfd) == 0;
    ::close(dfd);
    return ok;
}

bool atomic_write(const std::string& path, const std::string& dir, const std::string& bytes) {
    const std::string tmp = path + ".tmp";
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return false;
    }
    const bool ok = write_all(fd, bytes.data(), bytes.size()) && ::fsync(fd) == 0;
    ::close(fd);
    return ok && ::rename(tmp.c_str(), path.c_str()) == 0 && fsync_dir(dir);
}

}  // namespace

RaftLog::RaftLog(std::string dir)
    : dir_(std::move(dir)),
      log_path_(dir_ + "/log.bin"),
      hard_path_(dir_ + "/hard_state.pb"),
      snap_path_(dir_ + "/snapshot_meta.pb") {}

RaftLog::~RaftLog() {
    if (fd_ >= 0) {
        ::close(fd_);
    }
}

int64_t RaftLog::last_index() const {
    if (entries_.empty()) {
        return snap_.last_included_index();
    }
    return entries_.back().index();
}

int64_t RaftLog::last_term() const {
    if (entries_.empty()) {
        return snap_.last_included_term();
    }
    return entries_.back().term();
}

bool RaftLog::load(std::string* err) {
    std::error_code ec;
    fs::create_directories(dir_, ec);
    if (ec) {
        *err = "create " + dir_ + ": " + ec.message();
        return false;
    }

    hard_.Clear();
    {
        std::ifstream ifs(hard_path_, std::ios::binary);
        if (ifs && !hard_.ParseFromIstream(&ifs)) {
            *err = "corrupt " + hard_path_;
            return false;
        }
    }

    snap_.Clear();
    {
        std::ifstream ifs(snap_path_, std::ios::binary);
        if (ifs) {
            if (!snap_.ParseFromIstream(&ifs)) {
                *err = "corrupt " + snap_path_;
                return false;
            }
        }
    }
    first_index_ = snap_.last_included_index() + 1;

    entries_.clear();
    offsets_.clear();
    end_offset_ = 0;
    std::string data;
    {
        std::ifstream ifs(log_path_, std::ios::binary);
        if (ifs) {
            data.assign(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
        }
    }
    size_t pos = 0;
    int64_t expect = first_index_;
    while (pos + sizeof(uint32_t) <= data.size()) {
        uint32_t len = 0;
        std::memcpy(&len, data.data() + pos, sizeof(len));
        const size_t body = pos + sizeof(len);
        if (body + len > data.size()) {
            break;
        }
        vectordb::raft::LogEntry e;
        if (!e.ParseFromArray(data.data() + body, static_cast<int>(len)) || e.index() != expect) {
            break;
        }
        offsets_.push_back(pos);
        entries_.push_back(std::move(e));
        ++expect;
        pos = body + len;
    }
    end_offset_ = pos;

    if (fd_ >= 0) {
        ::close(fd_);
    }
    fd_ = ::open(log_path_.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd_ < 0) {
        *err = "open " + log_path_ + ": " + std::strerror(errno);
        return false;
    }
    if (end_offset_ != data.size()) {
        if (::ftruncate(fd_, static_cast<off_t>(end_offset_)) != 0 || ::fdatasync(fd_) != 0) {
            *err = "truncate corrupt tail";
            return false;
        }
    }
    return true;
}

bool RaftLog::save_hard_state(int64_t term, int32_t voted_for) {
    vectordb::raft::HardState hs;
    hs.set_current_term(term);
    hs.set_voted_for(voted_for);
    std::string bytes;
    if (!hs.SerializeToString(&bytes) || !atomic_write(hard_path_, dir_, bytes)) {
        return false;
    }
    hard_ = std::move(hs);
    return true;
}

bool RaftLog::save_snapshot_meta() {
    std::string bytes;
    return snap_.SerializeToString(&bytes) && atomic_write(snap_path_, dir_, bytes);
}

int64_t RaftLog::term_at(int64_t index) const {
    if (index == 0) {
        return 0;
    }
    if (index == snap_.last_included_index()) {
        return snap_.last_included_term();
    }
    if (index < first_index_ || index > last_index()) {
        return -1;
    }
    return entries_[static_cast<size_t>(index - first_index_)].term();
}

const vectordb::raft::LogEntry& RaftLog::at(int64_t index) const {
    return entries_[static_cast<size_t>(index - first_index_)];
}

bool RaftLog::append(const std::vector<vectordb::raft::LogEntry>& entries) {
    if (entries.empty()) {
        return true;
    }
    std::string buf;
    std::vector<uint64_t> new_offsets;
    int64_t expect = last_index() + 1;
    for (const auto& e : entries) {
        if (e.index() != expect++) {
            return false;
        }
        std::string bytes;
        if (!e.SerializeToString(&bytes)) {
            return false;
        }
        const uint32_t len = static_cast<uint32_t>(bytes.size());
        new_offsets.push_back(end_offset_ + buf.size());
        buf.append(reinterpret_cast<const char*>(&len), sizeof(len));
        buf += bytes;
    }
    if (::lseek(fd_, static_cast<off_t>(end_offset_), SEEK_SET) < 0 ||
        !write_all(fd_, buf.data(), buf.size()) || ::fdatasync(fd_) != 0) {
        return false;
    }
    end_offset_ += buf.size();
    offsets_.insert(offsets_.end(), new_offsets.begin(), new_offsets.end());
    entries_.insert(entries_.end(), entries.begin(), entries.end());
    if (entries_.size() == new_offsets.size() && first_index_ == last_index() - static_cast<int64_t>(entries_.size()) + 1) {
        // first_index_ already correct
    }
    if (entries_.size() >= 1 && first_index_ != entries_.front().index()) {
        first_index_ = entries_.front().index();
    }
    return true;
}

bool RaftLog::truncate_from(int64_t index) {
    if (index < first_index_ || index > last_index() + 1) {
        return false;
    }
    if (index == last_index() + 1) {
        return true;
    }
    const size_t pos = static_cast<size_t>(index - first_index_);
    const uint64_t off = offsets_[pos];
    if (::ftruncate(fd_, static_cast<off_t>(off)) != 0 || ::fdatasync(fd_) != 0) {
        return false;
    }
    end_offset_ = off;
    entries_.resize(pos);
    offsets_.resize(pos);
    return true;
}

bool RaftLog::rewrite_log_file(std::string* err) {
    const std::string tmp = log_path_ + ".tmp";
    const int nfd = ::open(tmp.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (nfd < 0) {
        *err = "open tmp log";
        return false;
    }
    uint64_t off = 0;
    std::vector<uint64_t> new_offsets;
    for (const auto& e : entries_) {
        std::string bytes;
        if (!e.SerializeToString(&bytes)) {
            ::close(nfd);
            return false;
        }
        const uint32_t len = static_cast<uint32_t>(bytes.size());
        new_offsets.push_back(off);
        if (!write_all(nfd, reinterpret_cast<const char*>(&len), sizeof(len)) ||
            !write_all(nfd, bytes.data(), bytes.size())) {
            ::close(nfd);
            return false;
        }
        off += sizeof(len) + bytes.size();
    }
    if (::fsync(nfd) != 0) {
        ::close(nfd);
        return false;
    }
    ::close(fd_);
    fd_ = -1;
    if (::rename(tmp.c_str(), log_path_.c_str()) != 0 || !fsync_dir(dir_)) {
        *err = "rename log";
        return false;
    }
    fd_ = ::open(log_path_.c_str(), O_RDWR, 0644);
    if (fd_ < 0) {
        *err = "reopen log";
        return false;
    }
    offsets_ = std::move(new_offsets);
    end_offset_ = off;
    return true;
}

bool RaftLog::compact_through(int64_t through, const std::string& content_hash,
                              const std::vector<vectordb::raft::AppliedAdd>& applied) {
    if (through < snap_.last_included_index()) {
        return true;
    }
    if (through > last_index()) {
        return false;
    }
    const int64_t term = term_at(through);
    if (term < 0) {
        return false;
    }
    // drop entries with index <= through
    if (!entries_.empty() && through >= first_index_) {
        const size_t drop = static_cast<size_t>(through - first_index_ + 1);
        if (drop > entries_.size()) {
            return false;
        }
        entries_.erase(entries_.begin(), entries_.begin() + static_cast<std::ptrdiff_t>(drop));
    }
    snap_.set_last_included_index(through);
    snap_.set_last_included_term(term);
    snap_.set_content_hash(content_hash);
    snap_.clear_applied_adds();
    for (const auto& a : applied) {
        *snap_.add_applied_adds() = a;
    }
    first_index_ = through + 1;
    std::string err;
    if (!save_snapshot_meta() || !rewrite_log_file(&err)) {
        return false;
    }
    return true;
}

std::vector<vectordb::raft::LogEntry> RaftLog::slice(int64_t from, size_t max_n,
                                                     size_t max_bytes) const {
    std::vector<vectordb::raft::LogEntry> out;
    size_t bytes = 0;
    if (from < first_index_) {
        from = first_index_;
    }
    for (int64_t i = from; i <= last_index() && out.size() < max_n; ++i) {
        if (i <= snap_.last_included_index()) {
            continue;
        }
        const auto& e = at(i);
        const size_t sz = e.ByteSizeLong();
        if (!out.empty() && bytes + sz > max_bytes) {
            break;
        }
        bytes += sz;
        out.push_back(e);
    }
    return out;
}

int64_t RaftLog::first_index_of_term(int64_t term) const {
    for (int64_t i = first_index_; i <= last_index(); ++i) {
        if (i <= snap_.last_included_index()) {
            continue;
        }
        const int64_t t = term_at(i);
        if (t == term) {
            return i;
        }
        if (t > term) {
            break;
        }
    }
    return 0;
}

int64_t RaftLog::last_index_of_term(int64_t term) const {
    for (int64_t i = last_index(); i >= first_index_; --i) {
        if (i <= snap_.last_included_index()) {
            break;
        }
        const int64_t t = term_at(i);
        if (t == term) {
            return i;
        }
        if (t < term) {
            break;
        }
    }
    return 0;
}

}  // namespace vdb::raft
