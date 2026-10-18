#pragma once

#include <cstdint>
#include <string>

namespace exact_index {

struct Document {
    int id = 0;
    std::string content;
    std::string title;
    std::string link;
    uint64_t simhash = 0;
};

struct BuildPaths {
    std::string jieba_dict = "/usr/local/dict/jieba.dict.utf8";
    std::string jieba_hmm = "/usr/local/dict/hmm_model.utf8";
    std::string jieba_user = "/usr/local/dict/user.dict.utf8";
    std::string jieba_idf = "/usr/local/dict/idf.utf8";
    std::string jieba_stop = "/usr/local/dict/stop_words.utf8";
    std::string cn_stopwords;
    std::string en_stopwords;
};

// 一个建库阶段的耗时（秒）
struct PhaseTime {
    double span = 0.0;  // 该阶段最早任务开始到最晚任务结束
    double busy = 0.0;  // 该阶段所有任务耗时之和；busy/(span*线程数) 即并行效率
};

// 各阶段耗时；多线程流水线中阶段之间可能重叠，因此各 span 之和不等于总时间
struct PhaseTimes {
    PhaseTime init;         // 分词/simhash 模型加载
    PhaseTime list;         // 扫描 XML 文件夹
    PhaseTime parse;        // 解析 XML + 计算 simhash
    PhaseTime dedup;        // simhash 去重并赋正式 id
    PhaseTime tf;           // 分词统计词频（多线程版同时产出局部倒排）
    PhaseTime write_docs;   // 写 docs.data / offset.data
    PhaseTime write_jsonl;  // 写 contents.jsonl
    PhaseTime write_bm25;   // 合并倒排并写 index.data / doclen.data / stats.json
};

struct BuildStats {
    size_t xml_files = 0;
    size_t parsed_docs = 0;
    size_t kept_docs = 0;
    size_t dropped_docs = 0;
    double seconds = 0.0;  // 总墙钟时间
    PhaseTimes phases;
};

}  // namespace exact_index
