#include "index/EmbedClient.hh"

#include <cstdio>
#include <sstream>

namespace vdb {
namespace {

// 用单引号包裹并转义内部单引号，得到可安全拼进 shell 命令的参数。影响：无
std::string shell_escape(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out.push_back(c);
        }
    }
    out.push_back('\'');
    return out;
}

// 把文本转义成 JSON 字符串内容（不含两侧引号）。影响：无
std::string json_escape(const std::string& text) {
    std::string e;
    e.reserve(text.size() + 16);
    for (unsigned char c : text) {
        if (c == '\\' || c == '"') {
            e.push_back('\\');
            e.push_back(static_cast<char>(c));
        } else if (c == '\n') {
            e += "\\n";
        } else if (c == '\r') {
            e += "\\r";
        } else if (c == '\t') {
            e += "\\t";
        } else if (c < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\u%04x", c);
            e += buf;
        } else {
            e.push_back(static_cast<char>(c));
        }
    }
    return e;
}

// 解析 {"dim":N,"values":[...]} 中的 values。输出：至少一个数返回 true；影响：覆盖 *out
bool parse_embed_json(const std::string& body, std::vector<float>* out) {
    auto pos = body.find("\"values\"");
    if (pos == std::string::npos) {
        return false;
    }
    pos = body.find('[', pos);
    if (pos == std::string::npos) {
        return false;
    }
    auto end = body.find(']', pos);
    if (end == std::string::npos) {
        return false;
    }
    std::stringstream ss(body.substr(pos + 1, end - pos - 1));
    out->clear();
    std::string token;
    while (std::getline(ss, token, ',')) {
        if (token.find_first_not_of(" \t\n\r") == std::string::npos) {
            continue;
        }
        out->push_back(std::stof(token));
    }
    return !out->empty();
}

}  // namespace

EmbedClient::EmbedClient(std::string url) : url_(std::move(url)) {}

std::vector<float> EmbedClient::embed(const std::string& text, int timeout_s) const {
    const std::string payload = "{\"text\":\"" + json_escape(text) + "\"}";
    const std::string cmd = "curl -sS -f -m " + std::to_string(timeout_s) +
                            " -H 'Content-Type: application/json' -d " + shell_escape(payload) +
                            " " + shell_escape(url_) + " 2>/dev/null";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        return {};
    }
    std::string body;
    char buf[4096];
    while (fgets(buf, sizeof(buf), pipe)) {
        body += buf;
    }
    if (pclose(pipe) != 0) {
        return {};
    }
    std::vector<float> values;
    try {
        if (!parse_embed_json(body, &values)) {
            return {};
        }
    } catch (...) {
        return {};
    }
    return values;
}

}  // namespace vdb
