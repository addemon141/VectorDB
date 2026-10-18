#include "tlv/tlv.hh"

#include <cstring>

namespace vdb::tlv {
namespace {

void put_u16(std::string* o, uint16_t v) {
    o->push_back(static_cast<char>(v & 0xff));
    o->push_back(static_cast<char>((v >> 8) & 0xff));
}
void put_u32(std::string* o, uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        o->push_back(static_cast<char>((v >> (8 * i)) & 0xff));
    }
}
void put_u64(std::string* o, uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        o->push_back(static_cast<char>((v >> (8 * i)) & 0xff));
    }
}
uint16_t get_u16(const char* p) {
    return static_cast<uint16_t>(static_cast<uint8_t>(p[0])) |
           (static_cast<uint16_t>(static_cast<uint8_t>(p[1])) << 8);
}
uint32_t get_u32(const char* p) {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
        v |= static_cast<uint32_t>(static_cast<uint8_t>(p[i])) << (8 * i);
    }
    return v;
}
uint64_t get_u64(const char* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<uint64_t>(static_cast<uint8_t>(p[i])) << (8 * i);
    }
    return v;
}

// CRC-32/ISO-HDLC
uint32_t crc_table[256];
bool crc_ready = false;

void init_crc() {
    if (crc_ready) {
        return;
    }
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int j = 0; j < 8; ++j) {
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        crc_table[i] = c;
    }
    crc_ready = true;
}

}  // namespace

uint32_t crc32(const void* data, size_t n) {
    init_crc();
    uint32_t c = 0xFFFFFFFFu;
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < n; ++i) {
        c = crc_table[(c ^ p[i]) & 0xff] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

bool encode_frame(const Frame& f, std::string* out, std::string* err) {
    if (f.payload.size() > 0xffffffffu) {
        *err = "payload too large";
        return false;
    }
    out->clear();
    out->reserve(kHeaderSize + f.payload.size());
    put_u32(out, kMagic);
    put_u16(out, static_cast<uint16_t>(f.type));
    put_u16(out, f.flags);
    put_u64(out, f.total_len);
    put_u64(out, f.offset);
    put_u32(out, static_cast<uint32_t>(f.payload.size()));
    put_u32(out, crc32(f.payload.data(), f.payload.size()));
    out->append(f.payload);
    return true;
}

bool decode_frame(const std::string& buf, size_t* consumed, Frame* out, std::string* err) {
    *consumed = 0;
    err->clear();
    if (buf.size() < kHeaderSize) {
        return false;
    }
    const char* p = buf.data();
    if (get_u32(p) != kMagic) {
        *err = "bad magic";
        return false;
    }
    const uint16_t type = get_u16(p + 4);
    const uint16_t flags = get_u16(p + 6);
    const uint64_t total_len = get_u64(p + 8);
    const uint64_t offset = get_u64(p + 16);
    const uint32_t chunk_len = get_u32(p + 24);
    const uint32_t expect_crc = get_u32(p + 28);
    if (buf.size() < kHeaderSize + chunk_len) {
        return false;
    }
    const char* payload = p + kHeaderSize;
    if (crc32(payload, chunk_len) != expect_crc) {
        *err = "crc mismatch";
        return false;
    }
    out->type = static_cast<MsgType>(type);
    out->flags = flags;
    out->total_len = total_len;
    out->offset = offset;
    out->payload.assign(payload, chunk_len);
    *consumed = kHeaderSize + chunk_len;
    return true;
}

}  // namespace vdb::tlv
