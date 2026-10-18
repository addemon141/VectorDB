#include "index/DocStore.hh"

#include <fstream>

namespace vdb {
namespace {

// 取 <tag>...</tag> 之间的文本（首次出现，不处理嵌套/转义）。
// 输入：xml 片段、标签名；输出：标签内容，找不到返回空串；影响：无
std::string extract_tag(const std::string& xml, const char* tag) {
    const std::string open = std::string("<") + tag + ">";
    const std::string close = std::string("</") + tag + ">";
    auto a = xml.find(open);
    if (a == std::string::npos) {
        return {};
    }
    a += open.size();
    auto b = xml.find(close, a);
    if (b == std::string::npos) {
        return {};
    }
    return xml.substr(a, b - a);
}

}  // namespace

bool DocStore::parse_doc_xml(const std::string& chunk, StoredDoc* out) {
    if (!out) {
        return false;
    }
    const std::string id_s = extract_tag(chunk, "id");
    if (id_s.empty()) {
        return false;
    }
    out->id = std::stoi(id_s);
    out->link = extract_tag(chunk, "link");
    out->title = extract_tag(chunk, "title");
    out->content = extract_tag(chunk, "content");
    return true;
}

bool DocStore::load(const std::string& data_dir) {
    offsets_.clear();
    max_id_ = 0;
    docs_path_ = data_dir + "/docs.data";
    std::ifstream ifs(data_dir + "/offset.data");
    if (!ifs) {
        return false;
    }
    int id = 0;
    std::uint64_t off = 0;
    std::uint64_t size = 0;
    while (ifs >> id >> off >> size) {
        offsets_[id] = OffsetEntry{off, size};
        if (id > max_id_) {
            max_id_ = id;
        }
    }
    return !offsets_.empty();
}

bool DocStore::get(int id, StoredDoc* out) const {
    auto it = offsets_.find(id);
    if (it == offsets_.end() || !out) {
        return false;
    }
    std::ifstream ifs(docs_path_, std::ios::binary);
    if (!ifs) {
        return false;
    }
    ifs.seekg(static_cast<std::streamoff>(it->second.offset));
    std::string chunk(static_cast<size_t>(it->second.size), '\0');
    ifs.read(chunk.data(), static_cast<std::streamsize>(it->second.size));
    if (!ifs) {
        return false;
    }
    return parse_doc_xml(chunk, out);
}

std::vector<StoredDoc> DocStore::list_all() const {
    std::vector<StoredDoc> out;
    out.reserve(offsets_.size());
    for (const auto& [id, _] : offsets_) {
        StoredDoc d;
        if (get(id, &d)) {
            out.push_back(std::move(d));
        }
    }
    return out;
}

}  // namespace vdb
