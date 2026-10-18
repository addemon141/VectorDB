#include "index/HnswSearcher.hh"

#include "hnswlib.h"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace vdb {
namespace {

// 从 meta json 中取 "dim"（优先 vectors.meta.json，其次 embeddings.meta.json）。
// 输入：data_dir；输出：维度，缺失返回 0；影响：无
int parse_dim(const std::string& data_dir) {
    std::ifstream ifs(data_dir + "/vectors.meta.json");
    if (!ifs) {
        ifs.open(data_dir + "/embeddings.meta.json");
    }
    if (!ifs) {
        return 0;
    }
    std::stringstream ss;
    ss << ifs.rdbuf();
    const std::string text = ss.str();
    auto pos = text.find("\"dim\"");
    if (pos == std::string::npos) {
        return 0;
    }
    pos = text.find(':', pos);
    if (pos == std::string::npos) {
        return 0;
    }
    return std::stoi(text.substr(pos + 1));
}

}  // namespace

struct HnswSearcher::Impl {
    std::unique_ptr<hnswlib::L2Space> space;
    std::unique_ptr<hnswlib::HierarchicalNSW<float>> index;
};

HnswSearcher::HnswSearcher() : impl_(std::make_unique<Impl>()) {}

HnswSearcher::~HnswSearcher() = default;

bool HnswSearcher::load(const std::string& data_dir) {
    dim_ = parse_dim(data_dir);
    if (dim_ <= 0) {
        return false;
    }
    try {
        impl_->space = std::make_unique<hnswlib::L2Space>(dim_);
        impl_->index = std::make_unique<hnswlib::HierarchicalNSW<float>>(
            impl_->space.get(), data_dir + "/vectors.hnsw");
        impl_->index->setEf(64);
    } catch (...) {
        impl_->index.reset();
        impl_->space.reset();
        dim_ = 0;
        return false;
    }
    return true;
}

std::vector<HnswHit> HnswSearcher::search(const std::vector<float>& query, int k) const {
    std::vector<HnswHit> out;
    if (!impl_->index || query.size() != static_cast<size_t>(dim_) || k <= 0) {
        return out;
    }
    auto pq = impl_->index->searchKnn(query.data(), static_cast<size_t>(k));
    out.reserve(pq.size());
    while (!pq.empty()) {
        auto p = pq.top();
        pq.pop();
        out.push_back(HnswHit{static_cast<int>(p.second), p.first});
    }
    std::reverse(out.begin(), out.end());
    return out;
}

}  // namespace vdb
