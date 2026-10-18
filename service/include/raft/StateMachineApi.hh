#pragma once

#include "raft.pb.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace vdb::raft {

struct ApplyResult {
    vectordb::raft::ErrorCode code = vectordb::raft::OK;
    std::string message;
    int64_t base_version = 0;
    std::vector<int32_t> staging_ids;
    std::vector<std::pair<int32_t, int32_t>> id_mapping;
    int32_t kept_docs = 0;
    int64_t compact_through = 0;  // >0：apply 成功后 Raft 裁剪日志至此 index
    int64_t cutoff_index = 0;     // RebuildCmd apply 后记录的 cutoff
    bool idempotent = false;      // 同一 request_id 再次 apply，未新建文档
    // 本次裁剪要写入 SnapshotMeta 的 Add 去重记录（仅 log_index <= compact_through）
    std::vector<vectordb::raft::AppliedAdd> compacted_adds;
};

class StateMachineApi {
public:
    virtual ~StateMachineApi() = default;
    // log_index：正在 apply 的日志下标（Rebuild 用来写 cutoff）
    virtual ApplyResult apply(const vectordb::raft::Command& cmd, int64_t log_index) = 0;
    // 进程启动、重放裁剪点之后的日志之前调用。把已裁掉的 Add 去重记录装回内存。
    virtual void restore_compacted_adds(const vectordb::raft::SnapshotMeta&) {}
};

}  // namespace vdb::raft
