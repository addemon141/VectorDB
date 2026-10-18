#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace vdb {

// 一篇文档的字段（与建库 XML 块对应）
struct StoredDoc {
    int id = 0;
    std::string title;
    std::string link;
    std::string content;
};

// DocStore = 基座正文仓库：读 docs.data + offset.data，按正式 id 随机取一篇。
// 线程安全：load 之后只读；并发 get 各自打开文件。
class DocStore {
public:
    // 加载 offset.data 到内存（id → 偏移/长度），docs.data 只记路径。
    // 输入：data_dir 基座目录；输出：offset.data 存在且非空返回 true
    // 影响：清空并重建 offsets_、max_id_
    bool load(const std::string& data_dir);

    // 按正式 id 取一篇。输出：找到并解析成功返回 true；影响：只读，每次打开一次 docs.data
    bool get(int id, StoredDoc* out) const;

    // id 是否在基座中。影响：只读
    bool contains(int id) const { return offsets_.count(id) > 0; }

    // 枚举基座全部文档（顺序不定）。供 Rebuild 使用；影响：只读，逐篇 get
    std::vector<StoredDoc> list_all() const;

    int max_id() const { return max_id_; }  // 基座最大正式号；空库为 0
    size_t size() const { return offsets_.size(); }

private:
    struct OffsetEntry {
        std::uint64_t offset = 0;
        std::uint64_t size = 0;
    };

    std::string docs_path_;
    std::unordered_map<int, OffsetEntry> offsets_;
    int max_id_ = 0;

    // 从 <doc>...</doc> 片段抽出 id/link/title/content。缺 <id> 返回 false；影响：无
    static bool parse_doc_xml(const std::string& chunk, StoredDoc* out);
};

}  // namespace vdb
