#pragma once

#include <string>
#include <vector>

namespace vdb::tlv {

// 对目录下（递归）常规文件按相对路径排序后做内容 CRC 串联摘要，返回 16 进制字符串。
// 用于 InstallBase.content_hash；不含临时文件（名以 '.' 开头）。
std::string snapshot_hash(const std::string& dir, std::string* err);

// 单缓冲 FNV-1a 64 → hex（小对象用）
std::string fnv1a64_hex(const void* data, size_t n);

}  // namespace vdb::tlv
