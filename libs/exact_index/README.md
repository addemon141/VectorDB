# exact_index — Web 精确索引建库（BM25）

离线建库模块。现行精确侧为 **BM25**（倒排存原始 TF，查询时打分）。

历史 TF-IDF 实现见 [`../exact_index_tfidf_legacy`](../exact_index_tfidf_legacy)。

## 产物

- `docs.data` — 合并库文件
- `offset.data` — `docid offset size`
- `index.data` — 倒排 `word docid tf ...`（原始词频）
- `doclen.data` — `docid length`
- `stats.json` — `{n, avgdl, k1, b}`
- `contents.jsonl` — `{"id","content"}`，供二期向量化

## 流程

1. 并列解析 XML（tinyxml2，RSS `item`）
2. 并行计算 SimHash
3. **串行**查重并为每个入库文档分配 `id = index+1`
4. 并行统计原始 TF 与文档长度
5. 落盘 BM25 倒排 / doclen / stats（**不**在建库期写入最终分数）

## 构建

```bash
cd VectorDB/VectorDB/reference/exact_index
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

可选 ST/bench：`-DEXACT_INDEX_ENABLE_BENCH=ON`。

## 运行

```bash
./build/exact_index_build \
  --xml-dir testdata/corpus_web \
  --out-dir /tmp/exact_index_out \
  --threads $(nproc)

./build/bm25_query --data-dir /tmp/exact_index_out --query "向量检索" --k 5
```

## 冒烟测试

```bash
./scripts/smoke_bm25.sh
```

停用词默认 `testdata/stopwords/`；可用 `EXACT_INDEX_CN_STOP` / `EXACT_INDEX_EN_STOP` 覆盖。
