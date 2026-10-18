#pragma once

#include <cstdint>
#include <string>

namespace vdb::tlv {

// 读文件 [offset, offset+want) 到 *out；返回实际字节数，失败 -1
ssize_t file_pread(const std::string& path, uint64_t offset, size_t want, std::string* out,
                   std::string* err);

// 在 offset 处写入 data（创建父目录）；失败返回 false
bool file_pwrite(const std::string& path, uint64_t offset, const std::string& data, bool trunc_first,
                 std::string* err);

}  // namespace vdb::tlv
