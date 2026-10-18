#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace vdb::tlv {

constexpr uint32_t kMagic = 0x56444231u;  // VDB1
constexpr size_t kHeaderSize = 4 + 2 + 2 + 8 + 8 + 4 + 4;
constexpr size_t kDefaultChunk = 1 << 20;  // 1 MiB

enum class MsgType : uint16_t {
    // DataNode
    StatusReq = 1,
    StatusResp = 2,
    PutCorpusBegin = 10,
    PutCorpusChunk = 11,
    PutCorpusEnd = 12,
    PutCorpusAck = 13,
    BuildReq = 20,
    BuildResp = 21,
    SnapshotMetaReq = 30,
    SnapshotMetaResp = 31,
    SnapshotChunkReq = 32,
    SnapshotChunkResp = 33,
    // Peer snapshot install (Leader → Follower)
    PeerInstallBegin = 40,
    PeerInstallFile = 41,
    PeerInstallEnd = 42,
    PeerInstallAck = 43,
    Error = 255,
};

enum : uint16_t { kFlagResume = 1 };

struct Frame {
    MsgType type = MsgType::Error;
    uint16_t flags = 0;
    uint64_t total_len = 0;
    uint64_t offset = 0;
    std::string payload;
};

uint32_t crc32(const void* data, size_t n);
bool encode_frame(const Frame& f, std::string* out, std::string* err);
// 从缓冲解析一帧；consumed 为消耗字节。不够一帧返回 false 且 err 空。
bool decode_frame(const std::string& buf, size_t* consumed, Frame* out, std::string* err);

}  // namespace vdb::tlv
