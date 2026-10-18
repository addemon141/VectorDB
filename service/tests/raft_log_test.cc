// RaftLog 持久化单测：追加 / 截断 / 重新加载 / 损坏尾部截断 / HardState 原子写
#include "raft/RaftLog.hh"

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;
using vdb::raft::RaftLog;
using vectordb::raft::LogEntry;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::cerr << __FILE__ << ":" << __LINE__ << " CHECK failed: " #cond "\n"; \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

LogEntry make(int64_t term, int64_t index, const std::string& cmd) {
    LogEntry e;
    e.set_term(term);
    e.set_index(index);
    e.set_command(cmd);
    return e;
}

void test_append_reload(const std::string& dir) {
    std::string err;
    {
        RaftLog log(dir);
        CHECK(log.load(&err));
        CHECK(log.last_index() == 0);
        CHECK(log.term_at(0) == 0);
        CHECK(log.append({make(1, 1, "a"), make(1, 2, "b"), make(2, 3, "c")}));
        CHECK(!log.append({make(2, 5, "gap")}));  // 不连续
        CHECK(log.save_hard_state(2, 3));
    }
    RaftLog log(dir);
    CHECK(log.load(&err));
    CHECK(log.last_index() == 3);
    CHECK(log.last_term() == 2);
    CHECK(log.at(2).command() == "b");
    CHECK(log.current_term() == 2);
    CHECK(log.voted_for() == 3);
    CHECK(log.first_index_of_term(1) == 1);
    CHECK(log.last_index_of_term(1) == 2);
    CHECK(log.first_index_of_term(5) == 0);
    CHECK(log.term_at(4) == -1);
}

void test_truncate(const std::string& dir) {
    std::string err;
    {
        RaftLog log(dir);
        CHECK(log.load(&err));
        CHECK(log.truncate_from(2));
        CHECK(log.last_index() == 1);
        CHECK(log.append({make(3, 2, "x")}));
    }
    RaftLog log(dir);
    CHECK(log.load(&err));
    CHECK(log.last_index() == 2);
    CHECK(log.at(2).term() == 3);
    CHECK(log.at(2).command() == "x");
}

void test_slice(const std::string& dir) {
    std::string err;
    RaftLog log(dir);
    CHECK(log.load(&err));
    const std::string big(1000, 'z');
    CHECK(log.append({make(3, 3, big), make(3, 4, big), make(3, 5, big)}));
    CHECK(log.slice(1, 64, 1 << 20).size() == 5);
    CHECK(log.slice(2, 2, 1 << 20).size() == 2);
    const auto s = log.slice(3, 64, 1500);  // 每条约 1KB：只放得下一条
    CHECK(s.size() == 1 && s[0].index() == 3);
    CHECK(log.slice(6, 64, 1 << 20).empty());
}

void test_corrupt_tail(const std::string& dir) {
    std::string err;
    const std::string path = dir + "/log.bin";
    const auto good = fs::file_size(path);
    {
        std::ofstream ofs(path, std::ios::binary | std::ios::app);
        const uint32_t len = 999;  // 声称 999 字节，实际只写 3 字节
        ofs.write(reinterpret_cast<const char*>(&len), sizeof(len));
        ofs.write("abc", 3);
    }
    {
        RaftLog log(dir);
        CHECK(log.load(&err));
        CHECK(log.last_index() == 5);
        CHECK(fs::file_size(path) == good);  // 损坏尾部已截掉
        CHECK(log.append({make(4, 6, "after")}));
    }
    RaftLog log(dir);
    CHECK(log.load(&err));
    CHECK(log.last_index() == 6);
    CHECK(log.at(6).command() == "after");
}

void test_hard_state_atomic(const std::string& dir) {
    std::string err;
    RaftLog log(dir);
    CHECK(log.load(&err));
    CHECK(log.save_hard_state(7, 0));
    CHECK(!fs::exists(dir + "/hard_state.pb.tmp"));
    RaftLog again(dir);
    CHECK(again.load(&err));
    CHECK(again.current_term() == 7);
    CHECK(again.voted_for() == 0);
}

void test_compact(const std::string& dir) {
    std::string err;
    fs::remove_all(dir);
    {
        RaftLog log(dir);
        CHECK(log.load(&err));
        CHECK(log.append({make(1, 1, "a"), make(1, 2, "b"), make(2, 3, "c"), make(2, 4, "d")}));
        vectordb::raft::AppliedAdd kept;
        kept.set_request_id("req-1");
        kept.set_log_index(1);
        kept.set_content_fp(99);
        kept.add_doc_ids(-1);
        CHECK(log.compact_through(2, "hashx", {kept}));
        CHECK(log.first_index() == 3);
        CHECK(log.last_index() == 4);
        CHECK(log.term_at(2) == 1);
        CHECK(log.at(3).command() == "c");
    }
    RaftLog log(dir);
    CHECK(log.load(&err));
    CHECK(log.has_snapshot());
    CHECK(log.snapshot_meta().last_included_index() == 2);
    CHECK(log.snapshot_meta().applied_adds_size() == 1);
    CHECK(log.snapshot_meta().applied_adds(0).request_id() == "req-1");
    CHECK(log.snapshot_meta().applied_adds(0).doc_ids(0) == -1);
    CHECK(log.first_index() == 3);
    CHECK(log.last_index() == 4);
    CHECK(log.append({make(3, 5, "e")}));
    CHECK(log.last_index() == 5);
}

}  // namespace

int main() {
    const std::string dir =
        (fs::temp_directory_path() / ("raft_log_test_" + std::to_string(::getpid()))).string();
    fs::remove_all(dir);
    test_append_reload(dir);
    test_truncate(dir);
    test_slice(dir);
    test_corrupt_tail(dir);
    test_hard_state_atomic(dir);
    test_compact(dir);
    fs::remove_all(dir);
    if (g_failures) {
        std::cerr << "raft_log_test FAILED (" << g_failures << ")\n";
        return 1;
    }
    std::cout << "raft_log_test OK\n";
    return 0;
}
