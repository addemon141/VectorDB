#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace vdb {

enum class JobPhase {
    Queued,
    Uploading,
    Building,
    Distributing,
    Installing,
    Done,
    Failed,
};

inline const char* job_phase_name(JobPhase p) {
    switch (p) {
        case JobPhase::Queued:
            return "queued";
        case JobPhase::Uploading:
            return "uploading";
        case JobPhase::Building:
            return "building";
        case JobPhase::Distributing:
            return "distributing";
        case JobPhase::Installing:
            return "installing";
        case JobPhase::Done:
            return "done";
        case JobPhase::Failed:
            return "failed";
    }
    return "?";
}

inline JobPhase job_phase_from_name(const std::string& s) {
    if (s == "uploading") {
        return JobPhase::Uploading;
    }
    if (s == "building") {
        return JobPhase::Building;
    }
    if (s == "distributing") {
        return JobPhase::Distributing;
    }
    if (s == "installing") {
        return JobPhase::Installing;
    }
    if (s == "done") {
        return JobPhase::Done;
    }
    if (s == "failed") {
        return JobPhase::Failed;
    }
    return JobPhase::Queued;
}

struct JobStatus {
    std::string id;
    std::string type;  // init | rebuild
    JobPhase phase = JobPhase::Queued;
    std::string error;
    std::string content_hash;
    std::string corpus_json;  // 续传用（Init）
    int64_t base_version = 0;
    int64_t cutoff_index = 0;
    std::vector<std::pair<int32_t, int32_t>> mapping;
};

// Leader 侧异步 Init/Rebuild；持久化到 data_dir/jobs/
class JobTable {
public:
    void set_persist_dir(std::string dir);
    void load();

    std::string create(const std::string& type);
    bool ensure(const std::string& id, const std::string& type);  // 续传同一 job_id
    bool get(const std::string& id, JobStatus* out) const;
    void set_corpus(const std::string& id, const std::string& corpus);
    void update(const std::string& id, JobPhase phase, const std::string& err = {});
    void finish_ok(const std::string& id, const std::string& hash, int64_t ver,
                   int64_t cutoff = 0, std::vector<std::pair<int32_t, int32_t>> mapping = {});
    void finish_err(const std::string& id, const std::string& err);

private:
    void persist_locked(const JobStatus& st) const;

    mutable std::mutex mu_;
    std::unordered_map<std::string, JobStatus> jobs_;
    std::atomic<uint64_t> seq_{1};
    std::string persist_dir_;
};

}  // namespace vdb
