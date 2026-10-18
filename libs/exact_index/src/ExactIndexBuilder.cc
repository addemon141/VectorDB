#include "ExactIndexBuilder.hh"

#include "Deduper.hh"
#include "JobGroup.hh"
#include "LockFreeQueue.hh"
#include "ThreadPool.hh"

#include <cppjieba/Jieba.hpp>
#include <simhash/Simhasher.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace exact_index {

namespace fs = std::filesystem;

namespace {

using Clock = std::chrono::steady_clock;

// term -> [(doc_id, raw tf), ...]
using Postings = std::unordered_map<std::string, std::vector<std::pair<int, int>>>;

// Aggregates the task timings of one build phase; record() may be called from any thread.
class PhaseTimer {
public:
    explicit PhaseTimer(Clock::time_point origin) : origin_(origin) {}

    void record(Clock::time_point start, Clock::time_point end) {
        const int64_t s = ns_since_origin(start);
        const int64_t e = ns_since_origin(end);
        int64_t cur = min_start_.load(std::memory_order_relaxed);
        while (s < cur &&
               !min_start_.compare_exchange_weak(cur, s, std::memory_order_relaxed)) {
        }
        cur = max_end_.load(std::memory_order_relaxed);
        while (e > cur && !max_end_.compare_exchange_weak(cur, e, std::memory_order_relaxed)) {
        }
        busy_ns_.fetch_add(e - s, std::memory_order_relaxed);
    }

    PhaseTime result() const {
        PhaseTime out;
        const int64_t s = min_start_.load();
        const int64_t e = max_end_.load();
        if (e >= s) {
            out.span = static_cast<double>(e - s) / 1e9;
        }
        out.busy = static_cast<double>(busy_ns_.load()) / 1e9;
        return out;
    }

private:
    int64_t ns_since_origin(Clock::time_point t) const {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(t - origin_).count();
    }

    Clock::time_point origin_;
    std::atomic<int64_t> min_start_{std::numeric_limits<int64_t>::max()};
    std::atomic<int64_t> max_end_{std::numeric_limits<int64_t>::min()};
    std::atomic<int64_t> busy_ns_{0};
};

// Records the lifetime of one task into a PhaseTimer; a null timer disables recording.
class TaskTimer {
public:
    explicit TaskTimer(PhaseTimer* timer) : timer_(timer), start_(Clock::now()) {}
    ~TaskTimer() {
        if (timer_) {
            timer_->record(start_, Clock::now());
        }
    }
    TaskTimer(const TaskTimer&) = delete;
    TaskTimer& operator=(const TaskTimer&) = delete;

private:
    PhaseTimer* timer_;
    Clock::time_point start_;
};

struct BuildTimers {
    explicit BuildTimers(Clock::time_point o)
        : init(o), list(o), parse(o), dedup(o), tf(o), write_docs(o), write_jsonl(o),
          write_bm25(o) {}

    PhaseTimes result() const {
        PhaseTimes p;
        p.init = init.result();
        p.list = list.result();
        p.parse = parse.result();
        p.dedup = dedup.result();
        p.tf = tf.result();
        p.write_docs = write_docs.result();
        p.write_jsonl = write_jsonl.result();
        p.write_bm25 = write_bm25.result();
        return p;
    }

    PhaseTimer init, list, parse, dedup, tf, write_docs, write_jsonl, write_bm25;
};

double seconds_since(Clock::time_point t0) {
    return std::chrono::duration<double>(Clock::now() - t0).count();
}

uint64_t make_simhash(const simhash::Simhasher& hasher, const std::string& text) {
    uint64_t h = 0;
    int top_n = std::max(5, std::min(200, static_cast<int>(text.size()) / 120));
    hasher.make(text, top_n, h);
    return h;
}

// Raw term frequency + token count (document length for BM25).
int fill_tf(const cppjieba::Jieba& jieba,
            const Stopwords& sw,
            const Document& doc,
            std::map<std::string, int>& out_tf) {
    out_tf.clear();
    std::vector<std::string> words;
    jieba.Cut(doc.content, words);
    int word_num = 0;
    for (auto word : words) {
        if (valid_cn_word(word, sw)) {
            out_tf[word] += 1;
            ++word_num;
            continue;
        }
        bool is_en = true;
        for (char& ch : word) {
            if (!std::isalpha(static_cast<unsigned char>(ch))) {
                is_en = false;
                break;
            }
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        if (!is_en || !valid_en_word(word, sw)) {
            continue;
        }
        out_tf[word] += 1;
        ++word_num;
    }
    return word_num;
}

bool ensure_dir(const std::string& path) {
    std::error_code ec;
    fs::create_directories(path, ec);
    return !ec;
}

void write_docs_and_offset(const std::vector<Document>& docs, const std::string& out_dir) {
    std::ofstream docs_ofs(out_dir + "/docs.data");
    std::ofstream offset_ofs(out_dir + "/offset.data");
    size_t offset = 0;
    for (const auto& doc : docs) {
        std::string info = "<doc>\n  <id>" + std::to_string(doc.id) + "</id>\n  " +
                           "<link>" + doc.link + "</link>\n  " + "<title>" + doc.title +
                           "</title>\n  " + "<content>" + doc.content + "</content>\n" +
                           "</doc>\n";
        size_t size = info.size();
        docs_ofs << info;
        offset_ofs << doc.id << " " << offset << " " << size << "\n";
        offset += size;
    }
}

std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '\\':
                out += "\\\\";
                break;
            case '"':
                out += "\\\"";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
                break;
        }
    }
    return out;
}

// Export kept documents for Phase-2 embedding (one JSON object per line).
void write_contents_jsonl(const std::vector<Document>& docs, const std::string& out_dir) {
    std::ofstream ofs(out_dir + "/contents.jsonl");
    for (const auto& doc : docs) {
        ofs << "{\"id\":" << doc.id << ",\"content\":\"" << json_escape(doc.content)
            << "\"}\n";
    }
}

// doclen.data + stats.json; shared by both BM25 writers so their output stays byte-identical.
void write_doclen_and_stats(const std::vector<Document>& docs,
                            const std::vector<int>& doc_lens,
                            const std::string& out_dir,
                            double k1 = 1.2,
                            double b = 0.75) {
    const int n = static_cast<int>(docs.size());
    double sum_dl = 0.0;
    for (int dl : doc_lens) {
        sum_dl += static_cast<double>(std::max(dl, 0));
    }
    const double avgdl = n > 0 ? sum_dl / static_cast<double>(n) : 0.0;

    std::ofstream doclen_ofs(out_dir + "/doclen.data");
    for (size_t i = 0; i < docs.size(); ++i) {
        doclen_ofs << docs[i].id << " " << doc_lens[i] << "\n";
    }

    std::ofstream stats_ofs(out_dir + "/stats.json");
    stats_ofs << "{\n"
              << "  \"n\": " << n << ",\n"
              << "  \"avgdl\": " << avgdl << ",\n"
              << "  \"k1\": " << k1 << ",\n"
              << "  \"b\": " << b << ",\n"
              << "  \"metric\": \"bm25\",\n"
              << "  \"postings\": \"raw_tf\"\n"
              << "}\n";
}

#if defined(EXACT_INDEX_ENABLE_BENCH)
// Single-thread control group: builds the whole inverted index in one ordered map.
void write_bm25_index(const std::vector<Document>& docs,
                      const std::vector<std::map<std::string, int>>& per_doc_tf,
                      const std::vector<int>& doc_lens,
                      const std::string& out_dir) {
    std::map<std::string, std::map<int, int>> index;
    for (size_t i = 0; i < docs.size(); ++i) {
        for (const auto& [w, tf] : per_doc_tf[i]) {
            if (tf > 0) {
                index[w][docs[i].id] = tf;
            }
        }
    }

    std::ofstream ofs(out_dir + "/index.data");
    for (const auto& [word, posting] : index) {
        ofs << word;
        for (const auto& [id, tf] : posting) {
            ofs << " " << id << " " << tf;
        }
        ofs << "\n";
    }
    write_doclen_and_stats(docs, doc_lens, out_dir);
}
#endif  // EXACT_INDEX_ENABLE_BENCH

// Keep doc-id order when joining two posting lists. Each list is id-sorted; if the
// ranges do not interleave, append the later one, otherwise merge.
void append_posting(std::vector<std::pair<int, int>>& dst,
                    std::vector<std::pair<int, int>>& src) {
    if (src.empty()) {
        return;
    }
    if (dst.empty()) {
        dst = std::move(src);
        return;
    }
    if (dst.back().first <= src.front().first) {
        dst.insert(dst.end(), src.begin(), src.end());
        return;
    }
    if (src.back().first <= dst.front().first) {
        src.insert(src.end(), dst.begin(), dst.end());
        dst = std::move(src);
        return;
    }
    std::vector<std::pair<int, int>> out;
    out.reserve(dst.size() + src.size());
    std::merge(dst.begin(), dst.end(), src.begin(), src.end(), std::back_inserter(out));
    dst = std::move(out);
}

void merge_into(Postings& dst, Postings& src) {
    for (auto& [term, plist] : src) {
        append_posting(dst[term], plist);
    }
}

// Pairwise reduce. Threads pull two maps from the queue, merge, and push the result
// back. `remaining` counts maps still alive (queued or held). The merge that turns
// 2 into 1, or the thread left holding the only map, publishes the result. An empty
// queue is not completion: another thread may still be merging.
Postings merge_shards(std::vector<Postings>& shards, int threads) {
    std::vector<Postings> live;
    live.reserve(shards.size());
    for (auto& shard : shards) {
        if (!shard.empty()) {
            live.push_back(std::move(shard));
        }
        Postings().swap(shard);
    }
    if (live.empty()) {
        return {};
    }
    if (live.size() == 1) {
        return std::move(live[0]);
    }

    const int n = static_cast<int>(live.size());
    MpmcQueue<Postings> queue(static_cast<size_t>(n));
    for (auto& part : live) {
        if (!queue.try_enqueue(std::move(part))) {
            throw std::runtime_error("merge queue rejected a shard");
        }
    }

    std::atomic<int> remaining{n};
    std::atomic<bool> done{false};
    Postings result;
    std::exception_ptr error;
    std::mutex error_mu;

    int workers = threads > 0 ? threads : 1;
    if (workers > n) {
        workers = n;
    }

    auto worker = [&] {
        try {
            while (!done.load(std::memory_order_acquire)) {
                Postings a;
                if (!queue.try_dequeue(a)) {
                    std::this_thread::yield();
                    continue;
                }
                Postings b;
                if (!queue.try_dequeue(b)) {
                    if (remaining.load(std::memory_order_acquire) == 1) {
                        result = std::move(a);
                        done.store(true, std::memory_order_release);
                        return;
                    }
                    while (!queue.try_enqueue(std::move(a))) {
                        std::this_thread::yield();
                    }
                    continue;
                }
                merge_into(a, b);
                if (remaining.fetch_sub(1, std::memory_order_acq_rel) == 2) {
                    result = std::move(a);
                    done.store(true, std::memory_order_release);
                    return;
                }
                while (!queue.try_enqueue(std::move(a))) {
                    std::this_thread::yield();
                }
            }
        } catch (...) {
            {
                std::lock_guard<std::mutex> lock(error_mu);
                if (!error) {
                    error = std::current_exception();
                }
            }
            done.store(true, std::memory_order_release);
        }
    };

    std::vector<std::thread> pool;
    pool.reserve(static_cast<size_t>(workers));
    for (int i = 0; i < workers; ++i) {
        pool.emplace_back(worker);
    }
    for (auto& t : pool) {
        t.join();
    }
    if (error) {
        std::rethrow_exception(error);
    }
    return result;
}

// Merges per-shard postings and writes the same files as write_bm25_index.
void write_merged_bm25(const std::vector<Document>& docs,
                       std::vector<Postings>& shard_postings,
                       const std::vector<int>& doc_lens,
                       const std::string& out_dir,
                       int threads) {
    Postings merged = merge_shards(shard_postings, threads);

    std::vector<Postings::value_type*> terms;
    terms.reserve(merged.size());
    for (auto& kv : merged) {
        terms.push_back(&kv);
    }
    std::sort(terms.begin(), terms.end(),
              [](const Postings::value_type* a, const Postings::value_type* b) {
                  return a->first < b->first;
              });

    std::ofstream ofs(out_dir + "/index.data");
    for (auto* kv : terms) {
        auto& plist = kv->second;
        if (!std::is_sorted(plist.begin(), plist.end())) {
            std::sort(plist.begin(), plist.end());
        }
        ofs << kv->first;
        for (const auto& [id, tf] : plist) {
            ofs << " " << id << " " << tf;
        }
        ofs << "\n";
    }
    write_doclen_and_stats(docs, doc_lens, out_dir);
}

// Loaded once and shared by all workers: the segmentation/extraction paths are const
// and read-only. Simhasher owns its own Jieba without the user dict, so the two
// instances cannot be merged without changing simhash values.
std::unique_ptr<simhash::Simhasher> make_hasher(const BuildPaths& paths) {
    return std::make_unique<simhash::Simhasher>(paths.jieba_dict, paths.jieba_hmm,
                                                paths.jieba_idf, paths.jieba_stop);
}

std::unique_ptr<cppjieba::Jieba> make_jieba(const BuildPaths& paths) {
    return std::make_unique<cppjieba::Jieba>(paths.jieba_dict, paths.jieba_hmm,
                                             paths.jieba_user, paths.jieba_idf,
                                             paths.jieba_stop);
}

// Keeps the first exception thrown by any task so the caller can rethrow it after the
// pipeline has drained. Later stages check failed() and skip their work.
class FirstError {
public:
    void capture() {
        std::lock_guard<std::mutex> lk(mu_);
        if (!error_) {
            error_ = std::current_exception();
        }
        failed_.store(true, std::memory_order_release);
    }

    bool failed() const { return failed_.load(std::memory_order_acquire); }

    void rethrow_if_any() {
        std::lock_guard<std::mutex> lk(mu_);
        if (error_) {
            std::rethrow_exception(error_);
        }
    }

private:
    std::mutex mu_;
    std::exception_ptr error_;
    std::atomic<bool> failed_{false};
};

// Runs fn, recording any exception in err instead of letting it escape the worker
// (which would call std::terminate).
template <typename Fn>
void guarded(FirstError& err, Fn&& fn) {
    try {
        fn();
    } catch (...) {
        err.capture();
    }
}

// Submits n tasks; on_complete runs once, on the thread that finishes the last task
// (immediately on the caller when n == 0). A throwing task still counts as finished, so
// on_complete always runs and must check err.failed().
void run_group(ThreadPool& pool,
               FirstError& err,
               int n,
               std::function<void(int)> task,
               std::function<void()> on_complete) {
    if (n <= 0) {
        if (on_complete) {
            guarded(err, on_complete);
        }
        return;
    }
    auto group = std::make_shared<JobGroup>(n, [&err, cb = std::move(on_complete)] {
        if (cb) {
            guarded(err, cb);
        }
    });
    auto shared_task = std::make_shared<std::function<void(int)>>(std::move(task));
    for (int i = 0; i < n; ++i) {
        pool.submit([&err, group, shared_task, i] {
            if (!err.failed()) {
                guarded(err, [&] { (*shared_task)(i); });
            }
            group->done();
        });
    }
}

// Tokenizes docs in fine-grained shards; each shard fills doc_lens for its range and emits
// a local inverted index into shard_postings[shard].
void run_tf_stage(ThreadPool& pool,
                  FirstError& err,
                  const std::vector<Document>& docs,
                  const cppjieba::Jieba& jieba,
                  const Stopwords& sw,
                  int worker_count,
                  std::vector<int>& doc_lens,
                  std::vector<Postings>& shard_postings,
                  PhaseTimer* timer,
                  std::function<void()> on_complete) {
    const size_t n = docs.size();
    const int shards =
        static_cast<int>(std::min<size_t>(n, static_cast<size_t>(4 * worker_count)));
    doc_lens.assign(n, 0);
    shard_postings.assign(static_cast<size_t>(shards), Postings{});
    run_group(
        pool, err, shards,
        [&docs, &jieba, &sw, &doc_lens, &shard_postings, timer, n, shards](int w) {
            TaskTimer tt(timer);
            const size_t begin = n * static_cast<size_t>(w) / static_cast<size_t>(shards);
            const size_t end = n * static_cast<size_t>(w + 1) / static_cast<size_t>(shards);
            auto& local = shard_postings[static_cast<size_t>(w)];
            std::map<std::string, int> tf;
            for (size_t i = begin; i < end; ++i) {
                doc_lens[i] = fill_tf(jieba, sw, docs[i], tf);
                for (const auto& [word, count] : tf) {
                    local[word].emplace_back(docs[i].id, count);
                }
            }
        },
        std::move(on_complete));
}

}  // namespace

const std::vector<PhaseField>& phase_fields() {
    static const std::vector<PhaseField> fields = {
        {"init", &PhaseTimes::init},
        {"list", &PhaseTimes::list},
        {"parse", &PhaseTimes::parse},
        {"dedup", &PhaseTimes::dedup},
        {"tf", &PhaseTimes::tf},
        {"write_docs", &PhaseTimes::write_docs},
        {"write_jsonl", &PhaseTimes::write_jsonl},
        {"write_bm25", &PhaseTimes::write_bm25},
    };
    return fields;
}

#if defined(EXACT_INDEX_ENABLE_BENCH)
BuildResult build_exact_index_st(const std::string& xml_dir,
                                 const std::string& out_dir,
                                 const BuildPaths& paths) {
    const auto t0 = Clock::now();
    BuildTimers timers(t0);

    BuildResult result;
    Stopwords sw;
    if (!sw.load(paths.cn_stopwords, paths.en_stopwords)) {
        std::cerr << "failed to load stopwords\n";
        return result;
    }
    if (!ensure_dir(out_dir)) {
        std::cerr << "failed to create out_dir\n";
        return result;
    }

    std::unique_ptr<simhash::Simhasher> hasher;
    std::unique_ptr<cppjieba::Jieba> jieba;
    {
        TaskTimer tt(&timers.init);
        hasher = make_hasher(paths);
        jieba = make_jieba(paths);
    }

    std::vector<std::string> files;
    {
        TaskTimer tt(&timers.list);
        files = list_xml_files(xml_dir);
    }
    result.stats.xml_files = files.size();

    std::vector<Document> parsed;
    {
        TaskTimer tt(&timers.parse);
        for (const auto& f : files) {
            auto docs = parse_rss_xml(f);
            for (auto& d : docs) {
                d.simhash = make_simhash(*hasher, d.content);
                parsed.push_back(std::move(d));
            }
        }
    }
    result.stats.parsed_docs = parsed.size();

    {
        TaskTimer tt(&timers.dedup);
        Deduper deduper;
        for (auto& d : parsed) {
            deduper.try_insert(std::move(d), result.documents);
        }
        result.stats.dropped_docs = deduper.dropped();
    }
    result.stats.kept_docs = result.documents.size();

    std::vector<std::map<std::string, int>> per_doc_tf(result.documents.size());
    std::vector<int> doc_lens(result.documents.size(), 0);
    {
        TaskTimer tt(&timers.tf);
        for (size_t i = 0; i < result.documents.size(); ++i) {
            doc_lens[i] = fill_tf(*jieba, sw, result.documents[i], per_doc_tf[i]);
        }
    }

    {
        TaskTimer tt(&timers.write_docs);
        write_docs_and_offset(result.documents, out_dir);
    }
    {
        TaskTimer tt(&timers.write_bm25);
        write_bm25_index(result.documents, per_doc_tf, doc_lens, out_dir);
    }
    {
        TaskTimer tt(&timers.write_jsonl);
        write_contents_jsonl(result.documents, out_dir);
    }

    result.stats.phases = timers.result();
    result.stats.seconds = seconds_since(t0);
    return result;
}
#endif  // EXACT_INDEX_ENABLE_BENCH

// Pipeline:
//   load models -> list -> [per file: parse + simhash] -> dedup (in-order, serial)
//     -> write docs/offset | write contents.jsonl | tf shards -> merge + write BM25
// The parse group's callback starts the tail; the caller waits for the pool to go idle and
// rethrows the first task exception, if any.
BuildResult build_exact_index_mt(const std::string& xml_dir,
                                 const std::string& out_dir,
                                 const BuildPaths& paths,
                                 int worker_count) {
    const auto t0 = Clock::now();
    BuildTimers timers(t0);

    BuildResult result;
    Stopwords sw;
    if (!sw.load(paths.cn_stopwords, paths.en_stopwords)) {
        std::cerr << "failed to load stopwords\n";
        return result;
    }
    if (!ensure_dir(out_dir)) {
        std::cerr << "failed to create out_dir\n";
        return result;
    }
    if (worker_count <= 0) {
        worker_count = 1;
    }

    std::unique_ptr<simhash::Simhasher> hasher;
    std::unique_ptr<cppjieba::Jieba> jieba;
    {
        TaskTimer tt(&timers.init);
        hasher = make_hasher(paths);
        jieba = make_jieba(paths);
    }

    std::vector<std::string> files;
    std::vector<std::vector<Document>> slots;
    std::vector<int> doc_lens;
    std::vector<Postings> shard_postings;
    FirstError err;

    // Declared after all state that tasks reference: its destructor joins the workers
    // before that state is destroyed.
    ThreadPool pool(worker_count);

    {
        TaskTimer tt(&timers.list);
        files = list_xml_files(xml_dir);
    }
    result.stats.xml_files = files.size();
    if (files.empty()) {
        result.stats.phases = timers.result();
        result.stats.seconds = seconds_since(t0);
        return result;
    }

    slots.resize(files.size());
    run_group(
        pool, err, static_cast<int>(files.size()),
        [&](int i) {
            TaskTimer tt(&timers.parse);
            auto docs = parse_rss_xml(files[static_cast<size_t>(i)]);
            for (auto& d : docs) {
                d.simhash = make_simhash(*hasher, d.content);
            }
            slots[static_cast<size_t>(i)] = std::move(docs);
        },
        [&] {
            if (err.failed()) {
                return;
            }
            {
                TaskTimer tt(&timers.dedup);
                size_t total = 0;
                for (const auto& s : slots) {
                    total += s.size();
                }
                result.stats.parsed_docs = total;

                // Iterate in file order (not completion order) so formal ids are reproducible.
                Deduper deduper;
                for (auto& s : slots) {
                    for (auto& d : s) {
                        deduper.try_insert(std::move(d), result.documents);
                    }
                    std::vector<Document>().swap(s);
                }
                result.stats.dropped_docs = deduper.dropped();
                result.stats.kept_docs = result.documents.size();
            }

            pool.submit([&] {
                guarded(err, [&] {
                    TaskTimer tt(&timers.write_docs);
                    write_docs_and_offset(result.documents, out_dir);
                });
            });
            pool.submit([&] {
                guarded(err, [&] {
                    TaskTimer tt(&timers.write_jsonl);
                    write_contents_jsonl(result.documents, out_dir);
                });
            });
            run_tf_stage(pool, err, result.documents, *jieba, sw, worker_count, doc_lens,
                         shard_postings, &timers.tf, [&] {
                             if (err.failed()) {
                                 return;
                             }
                             TaskTimer tt(&timers.write_bm25);
                             write_merged_bm25(result.documents, shard_postings, doc_lens,
                                               out_dir, worker_count);
                         });
        });

    // Each stage is submitted from inside the last task of the previous one, so the pool
    // only becomes idle once the whole pipeline has finished or failed.
    pool.wait_idle();
    err.rethrow_if_any();
    result.stats.phases = timers.result();
    result.stats.seconds = seconds_since(t0);
    return result;
}

bool write_corpus_bm25(const std::vector<Document>& docs,
                       const std::string& out_dir,
                       const BuildPaths& paths,
                       int worker_count) {
    Stopwords sw;
    if (!sw.load(paths.cn_stopwords, paths.en_stopwords)) {
        std::cerr << "failed to load stopwords\n";
        return false;
    }
    if (!ensure_dir(out_dir)) {
        std::cerr << "failed to create out_dir\n";
        return false;
    }
    if (worker_count <= 0) {
        worker_count = 1;
    }

    const auto jieba = make_jieba(paths);
    std::vector<int> doc_lens;
    std::vector<Postings> shard_postings;
    FirstError err;

    // Declared after all state that tasks reference: its destructor joins the workers
    // before that state is destroyed.
    ThreadPool pool(worker_count);

    pool.submit([&] { guarded(err, [&] { write_docs_and_offset(docs, out_dir); }); });
    pool.submit([&] { guarded(err, [&] { write_contents_jsonl(docs, out_dir); }); });
    run_tf_stage(pool, err, docs, *jieba, sw, worker_count, doc_lens, shard_postings, nullptr,
                 [&] {
                     if (!err.failed()) {
                         write_merged_bm25(docs, shard_postings, doc_lens, out_dir,
                                           worker_count);
                     }
                 });

    pool.wait_idle();
    try {
        err.rethrow_if_any();
    } catch (const std::exception& e) {
        std::cerr << "write_corpus_bm25 failed: " << e.what() << "\n";
        return false;
    } catch (...) {
        std::cerr << "write_corpus_bm25 failed: unknown exception\n";
        return false;
    }
    return true;
}

}  // namespace exact_index
