#include "tlv/hash_util.hh"
#include <algorithm>

#include "tlv/tlv.hh"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

namespace fs = std::filesystem;

namespace vdb::tlv {

std::string fnv1a64_hex(const void* data, size_t n) {
    uint64_t h = 14695981039346656037ull;
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    std::ostringstream oss;
    oss << std::hex << h;
    return oss.str();
}

std::string snapshot_hash(const std::string& dir, std::string* err) {
    std::vector<fs::path> files;
    std::error_code ec;
    if (!fs::exists(dir, ec)) {
        *err = "dir missing: " + dir;
        return {};
    }
    for (auto it = fs::recursive_directory_iterator(dir, ec);
         it != fs::recursive_directory_iterator(); ++it) {
        if (ec) {
            *err = ec.message();
            return {};
        }
        if (!it->is_regular_file()) {
            continue;
        }
        const auto name = it->path().filename().string();
        if (name.empty() || name[0] == '.') {
            continue;
        }
        // 摘要文件本身不参与哈希，避免「先算 hash 再写入」导致不一致
        if (name == "SNAPSHOT.hash") {
            continue;
        }
        files.push_back(it->path());
    }
    std::sort(files.begin(), files.end());
    std::string acc;
    for (const auto& p : files) {
        const auto rel = fs::relative(p, dir, ec).generic_string();
        std::ifstream ifs(p, std::ios::binary);
        if (!ifs) {
            *err = "open failed: " + p.string();
            return {};
        }
        std::string body((std::istreambuf_iterator<char>(ifs)),
                         std::istreambuf_iterator<char>());
        const uint32_t c = crc32(body.data(), body.size());
        acc.append(rel);
        acc.push_back('\0');
        acc.append(reinterpret_cast<const char*>(&c), sizeof(c));
        const uint64_t sz = body.size();
        acc.append(reinterpret_cast<const char*>(&sz), sizeof(sz));
    }
    if (acc.empty()) {
        *err = "empty snapshot dir: " + dir;
        return {};
    }
    return fnv1a64_hex(acc.data(), acc.size());
}

}  // namespace vdb::tlv
