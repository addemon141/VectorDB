#pragma once

#include "Document.hh"
#include "TextUtil.hh"

#include <string>
#include <vector>

namespace exact_index {

struct BuildResult {
    BuildStats stats;
    std::vector<Document> documents;  // 去重后保留的文档（正式 id=1..N）
};

// 阶段名与 PhaseTimes 字段的对应表（按流水线顺序），供建库/基准程序打印报表
struct PhaseField {
    const char* name;
    PhaseTime PhaseTimes::*field;
};
const std::vector<PhaseField>& phase_fields();

// 一期主路径：RSS/XML → 并行解析/simhash → 串行去重 → 写 BM25 + docs + contents.jsonl
// 任一任务抛异常时，等流水线排空后在调用线程重新抛出第一个异常
BuildResult build_exact_index_mt(const std::string& xml_dir,
                                 const std::string& out_dir,
                                 const BuildPaths& paths,
                                 int worker_count);

// Compact 用：已有内存文档列表 → 只重算/写出 BM25 产物（不再解析 XML、不再去重）
// 任务异常不外抛：打印错误并返回 false
bool write_corpus_bm25(const std::vector<Document>& docs,
                       const std::string& out_dir,
                       const BuildPaths& paths,
                       int worker_count);

#if defined(EXACT_INDEX_ENABLE_BENCH)
// cmake启用条件编译-DEXACT_INDEX_ENABLE_BENCH=ON时启用，与项目多线程路径作对照以测试优化性能。
BuildResult build_exact_index_st(const std::string& xml_dir,
                                 const std::string& out_dir,
                                 const BuildPaths& paths);
#endif

}  // namespace exact_index
