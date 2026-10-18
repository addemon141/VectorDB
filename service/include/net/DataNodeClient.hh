#pragma once

#include <string>
#include <vector>

namespace vdb {

struct SnapshotFileMeta {
    std::string path;
    uint64_t size = 0;
};

struct SnapshotMeta {
    std::string content_hash;
    uint64_t total_bytes = 0;
    std::vector<SnapshotFileMeta> files;
};

class DataNodeClient {
public:
    explicit DataNodeClient(std::string addr) : addr_(std::move(addr)) {}
    const std::string& addr() const { return addr_; }

    bool put_corpus(const std::string& json_bytes, std::string* err,
                    const std::string& job_id = {}, uint64_t resume_offset = 0);
    bool build(std::string* content_hash, int64_t* base_version, std::string* err,
               const std::string& job_id = {});
    bool get_snapshot_meta(SnapshotMeta* meta, std::string* err);
    bool download_snapshot(const std::string& out_dir, SnapshotMeta* meta, std::string* err);
    bool status(std::string* state, std::string* content_hash, std::string* err,
                std::string* job_json = nullptr);

private:
    std::string addr_;
};

// primary / backup：整 job 失败时 switch_to_backup()，由调用方从头重做
class FailoverDataNodeClient {
public:
    FailoverDataNodeClient(std::string primary, std::string backup)
        : primary_(std::move(primary)), backup_(std::move(backup)), active_(primary_) {}

    DataNodeClient client() const { return DataNodeClient(active_); }
    const std::string& active_addr() const { return active_; }
    bool has_backup() const { return !backup_.empty() && active_ != backup_; }
    bool switch_to_backup() {
        if (!has_backup()) {
            return false;
        }
        active_ = backup_;
        return true;
    }
    void reset_to_primary() { active_ = primary_; }

private:
    std::string primary_;
    std::string backup_;
    std::string active_;
};

}  // namespace vdb
