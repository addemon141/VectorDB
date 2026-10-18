#include "job/ClusterJob.hh"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace vdb {

void JobTable::set_persist_dir(std::string dir) { persist_dir_ = std::move(dir); }

void JobTable::persist_locked(const JobStatus& st) const {
    if (persist_dir_.empty()) {
        return;
    }
    fs::create_directories(persist_dir_);
    json map = json::array();
    for (const auto& [a, b] : st.mapping) {
        map.push_back({{"old_id", a}, {"new_id", b}});
    }
    const json j = {{"id", st.id},
                    {"type", st.type},
                    {"phase", job_phase_name(st.phase)},
                    {"error", st.error},
                    {"content_hash", st.content_hash},
                    {"corpus_json", st.corpus_json},
                    {"base_version", st.base_version},
                    {"cutoff_index", st.cutoff_index},
                    {"mapping", map}};
    const std::string path = persist_dir_ + "/" + st.id + ".json";
    std::ofstream ofs(path);
    ofs << j.dump();
}

void JobTable::load() {
    if (persist_dir_.empty() || !fs::exists(persist_dir_)) {
        return;
    }
    std::lock_guard<std::mutex> lk(mu_);
    uint64_t max_seq = 1;
    for (const auto& ent : fs::directory_iterator(persist_dir_)) {
        if (!ent.is_regular_file() || ent.path().extension() != ".json") {
            continue;
        }
        std::ifstream ifs(ent.path());
        auto j = json::parse(ifs, nullptr, false);
        if (j.is_discarded() || !j.contains("id")) {
            continue;
        }
        JobStatus st;
        st.id = j["id"].get<std::string>();
        st.type = j.value("type", "init");
        st.phase = job_phase_from_name(j.value("phase", "queued"));
        st.error = j.value("error", "");
        st.content_hash = j.value("content_hash", "");
        st.corpus_json = j.value("corpus_json", "");
        st.base_version = j.value("base_version", 0);
        st.cutoff_index = j.value("cutoff_index", 0);
        for (const auto& m : j.value("mapping", json::array())) {
            st.mapping.emplace_back(m.value("old_id", 0), m.value("new_id", 0));
        }
        jobs_[st.id] = st;
        // init-12 → track seq
        const auto dash = st.id.rfind('-');
        if (dash != std::string::npos) {
            try {
                max_seq = std::max(max_seq, static_cast<uint64_t>(std::stoull(st.id.substr(dash + 1)) + 1));
            } catch (...) {
            }
        }
    }
    seq_ = max_seq;
}

std::string JobTable::create(const std::string& type) {
    std::lock_guard<std::mutex> lk(mu_);
    JobStatus st;
    st.id = type + "-" + std::to_string(seq_++);
    st.type = type;
    st.phase = JobPhase::Queued;
    jobs_[st.id] = st;
    persist_locked(st);
    return st.id;
}

bool JobTable::ensure(const std::string& id, const std::string& type) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = jobs_.find(id);
    if (it != jobs_.end()) {
        return true;
    }
    JobStatus st;
    st.id = id;
    st.type = type;
    st.phase = JobPhase::Queued;
    jobs_[id] = st;
    persist_locked(st);
    return true;
}

bool JobTable::get(const std::string& id, JobStatus* out) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = jobs_.find(id);
    if (it == jobs_.end()) {
        return false;
    }
    *out = it->second;
    return true;
}

void JobTable::set_corpus(const std::string& id, const std::string& corpus) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = jobs_.find(id);
    if (it == jobs_.end()) {
        return;
    }
    it->second.corpus_json = corpus;
    persist_locked(it->second);
}

void JobTable::update(const std::string& id, JobPhase phase, const std::string& err) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = jobs_.find(id);
    if (it == jobs_.end()) {
        return;
    }
    it->second.phase = phase;
    it->second.error = err;
    persist_locked(it->second);
}

void JobTable::finish_ok(const std::string& id, const std::string& hash, int64_t ver,
                         int64_t cutoff, std::vector<std::pair<int32_t, int32_t>> mapping) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = jobs_.find(id);
    if (it == jobs_.end()) {
        return;
    }
    it->second.phase = JobPhase::Done;
    it->second.content_hash = hash;
    it->second.base_version = ver;
    it->second.cutoff_index = cutoff;
    it->second.mapping = std::move(mapping);
    it->second.error.clear();
    it->second.corpus_json.clear();  // 完成可清 corpus
    persist_locked(it->second);
}

void JobTable::finish_err(const std::string& id, const std::string& err) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = jobs_.find(id);
    if (it == jobs_.end()) {
        return;
    }
    it->second.phase = JobPhase::Failed;
    it->second.error = err;
    persist_locked(it->second);
}

}  // namespace vdb
