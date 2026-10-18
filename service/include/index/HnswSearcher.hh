#pragma once

#include <memory>
#include <string>
#include <vector>

namespace vdb {

struct HnswHit {
    int doc_id = 0;
    float dist = 0.0f;  // L2 平方距离（hnswlib L2Space 的输出），越小越近
};

// HnswSearcher = 加载基座 vectors.hnsw，做近似最近邻查询。
// 线程安全：load 之后并发 search 安全（hnswlib 查询只读）。
class HnswSearcher {
public:
    HnswSearcher();
    ~HnswSearcher();

    HnswSearcher(const HnswSearcher&) = delete;
    HnswSearcher& operator=(const HnswSearcher&) = delete;

    // 加载向量图。
    // 输入：data_dir（读 vectors.meta.json 或 embeddings.meta.json 取维度，再读 vectors.hnsw）
    // 输出：成功返回 true；缺失或损坏返回 false，此时 dim()==0，基座向量路关闭
    bool load(const std::string& data_dir);

    // 查询 k 个最近邻。输出：按距离升序；未加载、维度不符或 k<=0 返回空；影响：只读
    std::vector<HnswHit> search(const std::vector<float>& query, int k) const;

    int dim() const { return dim_; }  // 0 表示未加载

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    int dim_ = 0;
};

}  // namespace vdb
