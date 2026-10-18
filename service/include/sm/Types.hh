#pragma once

#include "Bm25Index.hh"
#include "index/DocStore.hh"
#include "index/HnswSearcher.hh"

#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace vdb {

// 基座：一次离线建库（或一次 Rebuild）的只读产物。整体以 shared_ptr<const BaseIndex> 持有，
// Rebuild 时整份替换，正在进行的查询仍持有旧的一份。
struct BaseIndex {
    std::string dir;
    DocStore docs;
    exact_index::Bm25Index bm25;
    HnswSearcher hnsw;  // dim()==0 表示基座没有向量索引
};

// 暂存区里一篇已提交、尚未并入基座的文档
struct StagingDoc {
    // 来自日志命令，所有节点一致
    int32_t id = 0;  // apply 时分配的负数 staging_id
    std::string title;
    std::string link;
    std::string content;
    std::vector<float> embedding;  // 可能为空（仅 BM25 可召回）
    int64_t log_index = 0;         // 创建该文档的 Raft 日志下标

    // apply 时本地派生，只服务查询
    std::unordered_map<std::string, int> tf;
    int doc_len = 0;
};

struct StagingArea {
    int32_t next_seq = 1;                        // 下一个 staging_id = -next_seq
    std::map<int32_t, StagingDoc> docs;  // 有序：Rebuild 时按确定顺序并入基座
    // 被删 id → 执行删除的日志下标。查询只用 key。
    std::unordered_map<int32_t, int64_t> base_tombstones;
};

}  // namespace vdb
