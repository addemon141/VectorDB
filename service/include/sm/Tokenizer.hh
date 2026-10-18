#pragma once

#include "Bm25Index.hh"
#include "Document.hh"
#include "TextUtil.hh"

#include <cppjieba/Jieba.hpp>

#include <map>
#include <memory>
#include <string>

namespace vdb {

// Tokenizer = 与建库一致的分词 + 停用词过滤，输出词频。
// 一个进程一份，状态机（apply 时算暂存文档 tf）与查询共用；jieba 的切词是只读操作，可并发调用。
class Tokenizer {
public:
    // 加载 jieba 词典与停用词。输出：停用词加载失败返回 false；影响：耗时数秒
    bool load(const exact_index::BuildPaths& paths, std::string* err) {
        if (!stopwords_.load(paths.cn_stopwords, paths.en_stopwords)) {
            *err = "load stopwords failed: " + paths.cn_stopwords + " / " + paths.en_stopwords;
            return false;
        }
        jieba_ = std::make_unique<cppjieba::Jieba>(paths.jieba_dict, paths.jieba_hmm,
                                                   paths.jieba_user, paths.jieba_idf,
                                                   paths.jieba_stop);
        return true;
    }

    // 文本 → 词频（term → 次数）。影响：无
    std::map<std::string, int> term_freq(const std::string& text) const {
        std::map<std::string, int> out;
        exact_index::fill_query_tf(*jieba_, stopwords_, text, out);
        return out;
    }

private:
    std::unique_ptr<cppjieba::Jieba> jieba_;
    exact_index::Stopwords stopwords_;
};

}  // namespace vdb
