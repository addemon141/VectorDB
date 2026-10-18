#include "net/DataNodeClient.hh"

#include "tlv/file_io.hh"
#include "tlv/socket_io.hh"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace vdb {

bool DataNodeClient::put_corpus(const std::string& json_bytes, std::string* err,
                                const std::string& job_id, uint64_t resume_offset) {
    auto conn = tlv::Conn::connect(addr_, err);
    if (!conn.valid()) {
        return false;
    }
    tlv::Frame begin;
    begin.type = tlv::MsgType::PutCorpusBegin;
    begin.total_len = json_bytes.size();
    begin.flags = resume_offset > 0 ? tlv::kFlagResume : 0;
    begin.offset = resume_offset;
    begin.payload =
        json{{"total_len", json_bytes.size()}, {"job_id", job_id}, {"resume_offset", resume_offset}}
            .dump();
    if (!conn.send_frame(begin, err)) {
        return false;
    }
    tlv::Frame ack;
    if (!conn.recv_frame(&ack, err) || ack.type != tlv::MsgType::PutCorpusAck) {
        *err = "PutCorpusBegin ack failed";
        return false;
    }
    auto aj = json::parse(ack.payload, nullptr, false);
    uint64_t off = resume_offset;
    if (!aj.is_discarded() && aj.contains("offset")) {
        off = aj["offset"].get<uint64_t>();
    }
    while (off < json_bytes.size()) {
        const size_t n = std::min(tlv::kDefaultChunk, json_bytes.size() - static_cast<size_t>(off));
        tlv::Frame chunk;
        chunk.type = tlv::MsgType::PutCorpusChunk;
        chunk.total_len = json_bytes.size();
        chunk.offset = off;
        chunk.payload.assign(json_bytes.data() + off, n);
        if (!conn.send_frame(chunk, err) || !conn.recv_frame(&ack, err) ||
            ack.type != tlv::MsgType::PutCorpusAck) {
            *err = "PutCorpusChunk failed at " + std::to_string(off);
            return false;
        }
        off += n;
    }
    tlv::Frame end;
    end.type = tlv::MsgType::PutCorpusEnd;
    end.total_len = json_bytes.size();
    end.payload = json{{"job_id", job_id}}.dump();
    if (!conn.send_frame(end, err) || !conn.recv_frame(&ack, err) ||
        ack.type != tlv::MsgType::PutCorpusAck) {
        *err = "PutCorpusEnd failed";
        return false;
    }
    return true;
}

bool DataNodeClient::build(std::string* content_hash, int64_t* base_version, std::string* err,
                           const std::string& job_id) {
    auto conn = tlv::Conn::connect(addr_, err);
    if (!conn.valid()) {
        return false;
    }
    tlv::Frame req;
    req.type = tlv::MsgType::BuildReq;
    req.payload = json{{"job_id", job_id}}.dump();
    if (!conn.send_frame(req, err)) {
        return false;
    }
    tlv::Frame resp;
    if (!conn.recv_frame(&resp, err) || resp.type != tlv::MsgType::BuildResp) {
        *err = "BuildResp missing";
        return false;
    }
    auto j = json::parse(resp.payload, nullptr, false);
    if (j.is_discarded() || !j.value("ok", false)) {
        *err = j.value("error", "build failed");
        return false;
    }
    *content_hash = j.at("content_hash").get<std::string>();
    *base_version = j.value("base_version", 1);
    return true;
}

bool DataNodeClient::get_snapshot_meta(SnapshotMeta* meta, std::string* err) {
    auto conn = tlv::Conn::connect(addr_, err);
    if (!conn.valid()) {
        return false;
    }
    tlv::Frame req;
    req.type = tlv::MsgType::SnapshotMetaReq;
    if (!conn.send_frame(req, err)) {
        return false;
    }
    tlv::Frame resp;
    if (!conn.recv_frame(&resp, err) || resp.type != tlv::MsgType::SnapshotMetaResp) {
        *err = "SnapshotMetaResp missing";
        return false;
    }
    auto j = json::parse(resp.payload, nullptr, false);
    if (j.is_discarded()) {
        *err = "bad meta json";
        return false;
    }
    meta->content_hash = j.at("content_hash").get<std::string>();
    meta->total_bytes = j.value("total_bytes", 0ull);
    meta->files.clear();
    for (const auto& f : j.at("files")) {
        SnapshotFileMeta m;
        m.path = f.at("path").get<std::string>();
        m.size = f.at("size").get<uint64_t>();
        meta->files.push_back(std::move(m));
    }
    return true;
}

bool DataNodeClient::download_snapshot(const std::string& out_dir, SnapshotMeta* meta,
                                       std::string* err) {
    if (!get_snapshot_meta(meta, err)) {
        return false;
    }
    fs::remove_all(out_dir);
    fs::create_directories(out_dir);
    for (const auto& file : meta->files) {
        const fs::path dest = fs::path(out_dir) / file.path;
        uint64_t off = 0;
        bool first = true;
        while (off < file.size) {
            auto conn = tlv::Conn::connect(addr_, err);
            if (!conn.valid()) {
                return false;
            }
            const uint32_t want =
                static_cast<uint32_t>(std::min<uint64_t>(tlv::kDefaultChunk, file.size - off));
            tlv::Frame req;
            req.type = tlv::MsgType::SnapshotChunkReq;
            req.payload =
                json{{"path", file.path}, {"offset", off}, {"length", want}}.dump();
            if (!conn.send_frame(req, err)) {
                return false;
            }
            tlv::Frame resp;
            if (!conn.recv_frame(&resp, err) || resp.type != tlv::MsgType::SnapshotChunkResp) {
                *err = "chunk resp failed for " + file.path;
                return false;
            }
            if (!tlv::file_pwrite(dest.string(), off, resp.payload, first, err)) {
                return false;
            }
            first = false;
            off += resp.payload.size();
            if (resp.payload.empty()) {
                break;
            }
        }
    }
    return true;
}

bool DataNodeClient::status(std::string* state, std::string* content_hash, std::string* err,
                            std::string* job_json) {
    auto conn = tlv::Conn::connect(addr_, err);
    if (!conn.valid()) {
        return false;
    }
    tlv::Frame req;
    req.type = tlv::MsgType::StatusReq;
    if (!conn.send_frame(req, err)) {
        return false;
    }
    tlv::Frame resp;
    if (!conn.recv_frame(&resp, err) || resp.type != tlv::MsgType::StatusResp) {
        *err = "StatusResp missing";
        return false;
    }
    auto j = json::parse(resp.payload, nullptr, false);
    if (j.is_discarded()) {
        *err = "bad status json";
        return false;
    }
    *state = j.value("state", "");
    *content_hash = j.value("content_hash", "");
    if (job_json) {
        *job_json = j.value("job", json::object()).dump();
    }
    return true;
}

}  // namespace vdb
