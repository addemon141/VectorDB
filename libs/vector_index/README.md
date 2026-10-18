# vector_index — Phase 2: SBERT embeddings + HNSW

## Pipeline

1. First-phase `exact_index_build` writes `contents.jsonl` (kept docs).
2. `scripts/embed_sbert.py` → `embeddings.f32` + `embeddings.ids` (+ meta).
3. `hnsw_build` → `vectors.hnsw` + `vectors.meta.json`.
4. Optional: `hnsw_query` smoke-loads the index.

## Dependencies

Python (`requirements.txt`):

```bash
python3 -m pip install --user torch --index-url https://download.pytorch.org/whl/cpu
python3 -m pip install --user -r requirements.txt
# Keep numpy 1.x if the OS scipy is old: python3 -m pip install --user 'numpy<2'
```

If Hugging Face is unreachable, use a mirror:

```bash
export HF_ENDPOINT=https://hf-mirror.com
```

C++: vendored header-only [hnswlib](third_party/hnswlib).

Default model: `sentence-transformers/paraphrase-multilingual-MiniLM-L12-v2` (384-d), L2-normalized.
`--backend auto|sbert|transformers` (auto falls back when sentence-transformers import fails).

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

## Run

```bash
# after exact_index produced OUT/contents.jsonl
export HF_ENDPOINT=https://hf-mirror.com   # if needed
python3 scripts/embed_sbert.py --input OUT/contents.jsonl --out-dir OUT
./build/hnsw_build --emb-dir OUT --out-dir OUT
./build/hnsw_query --emb-dir OUT --index-dir OUT --k 5
```

HNSW defaults: `M=16`, `ef_construction=200`, `ef_search=64` (override via CLI flags on `hnsw_build`).
