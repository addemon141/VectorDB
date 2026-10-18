#!/usr/bin/env bash
# Build tiny BM25 index and assert query ranking basics.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

OUT="${TMPDIR:-/tmp}/exact_index_bm25_smoke_$$"
rm -rf "$OUT"
mkdir -p "$OUT"

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build -j >/dev/null

./build/exact_index_build \
  --xml-dir testdata/corpus_tiny \
  --out-dir "$OUT" \
  --threads 2

test -f "$OUT/index.data"
test -f "$OUT/doclen.data"
test -f "$OUT/stats.json"
test -f "$OUT/docs.data"
test -f "$OUT/contents.jsonl"

# Postings must be integer TF pairs (no floats in third column of a sample line).
if grep -E '^[^\s]+( [0-9]+ [0-9]*\.[0-9]+)+' "$OUT/index.data" >/dev/null; then
  echo "FAIL: index.data still looks like float TF-IDF weights" >&2
  exit 1
fi

QUERY_OUT="$(./build/bm25_query --data-dir "$OUT" --query "苹果 手机" --k 3)"
echo "$QUERY_OUT"

# Expect at least one hit and that "苹果" related docs rank (id 1 or 4 in tiny corpus).
echo "$QUERY_OUT" | grep -q 'id='
TOP_ID="$(echo "$QUERY_OUT" | sed -n 's/.*id=\([0-9][0-9]*\).*/\1/p' | head -n1)"
case "$TOP_ID" in
  1|4) ;;
  *)
    echo "FAIL: unexpected top id=$TOP_ID for query 苹果 手机" >&2
    exit 1
    ;;
esac

VEC_OUT="$(./build/bm25_query --data-dir "$OUT" --query "向量 数据库 检索" --k 2)"
echo "$VEC_OUT"
VEC_TOP="$(echo "$VEC_OUT" | sed -n 's/.*id=\([0-9][0-9]*\).*/\1/p' | head -n1)"
if [[ "$VEC_TOP" != "3" ]]; then
  echo "FAIL: expected doc id=3 for 向量查询, got id=$VEC_TOP" >&2
  exit 1
fi

echo "smoke_bm25 OK (out=$OUT)"
rm -rf "$OUT"
