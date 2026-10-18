#pragma once

#include "raft.grpc.pb.h"
#include "raft/RaftNode.hh"

namespace vdb::raft {

// RaftService 的 gRPC 适配层：把其他节点发来的 RPC 原样转给 RaftNode。
// 注册在管理 server 上，与 AdminService 共用端口和线程池。
class RaftServiceImpl final : public vectordb::raft::RaftService::Service {
public:
    explicit RaftServiceImpl(RaftNode* node) : node_(node) {}

    grpc::Status RequestVote(grpc::ServerContext*, const vectordb::raft::RequestVoteArgs* req,
                             vectordb::raft::RequestVoteReply* reply) override {
        *reply = node_->handle_request_vote(*req);
        return grpc::Status::OK;
    }

    grpc::Status AppendEntries(grpc::ServerContext*, const vectordb::raft::AppendEntriesArgs* req,
                               vectordb::raft::AppendEntriesReply* reply) override {
        *reply = node_->handle_append_entries(*req);
        return grpc::Status::OK;
    }

private:
    RaftNode* node_;
};

}  // namespace vdb::raft
