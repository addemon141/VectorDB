#pragma once

#include "sm/Types.hh"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace vdb {

struct RebuildOutput {
    std::string base_dir;                              // 新基座目录（如 data_dir/base_v{N}）
    std::vector<std::pair<int32_t, int32_t>> mapping;  // old_id -> new_id，只含保留的文档
    int32_t kept_docs = 0;
};

// Rebuilder = 把「基座 − base_tombstones + 暂存区」重建成新基座。本期只有接口，没有实现。
//
// 约定：
//   - 在 applier 线程里同步调用，调用期间不持有 RaftNode::mu_ 与 StateMachine 的锁，
//     base / staging 在此期间不会被修改（applier 是唯一写者）；
//   - 同样的输入必须产出同样的 mapping（各节点独立重建，id 必须一致）：
//     基座保留文档按旧 id 升序在前，暂存文档按 staging_id 从 -1 到 -N 在后，依次编号 1..K；
//   - 只写新目录，不修改 base.dir。
class Rebuilder {
public:
    virtual ~Rebuilder() = default;
    virtual bool rebuild(const BaseIndex& base, const StagingArea& staging, int64_t new_version,
                         RebuildOutput* out, std::string* err) = 0;
};

}  // namespace vdb
