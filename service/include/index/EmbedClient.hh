#pragma once

#include <string>
#include <vector>

namespace vdb {

// EmbedClient = 调 Python 常驻服务把文本变成向量（curl POST /embed）。无内部状态，可并发调用。
class EmbedClient {
public:
    explicit EmbedClient(std::string url);  // 如 http://127.0.0.1:50053/embed

    // 文本 → 向量。
    // 输出：sidecar 返回的向量（已 L2 归一化）；未启动、超时或解析失败返回空
    // 影响：fork 一个 curl 子进程并阻塞等待（超时 timeout_s 秒）；
    //       文本作为命令行参数传递，单参数超过约 128KB 会失败，调用方需限制长度
    std::vector<float> embed(const std::string& text, int timeout_s = 30) const;

private:
    std::string url_;
};

}  // namespace vdb
