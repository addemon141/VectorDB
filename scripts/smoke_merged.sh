#!/usr/bin/env bash
# 合并工程冒烟：离线 BM25 建库 + bm25_query，再跑单节点服务冒烟（smoke_query_service.sh）
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
BIN="$ROOT/build/bin"
OUT="${TMPDIR:-/tmp}/vdb_merged_smoke_$$"
rm -rf "$OUT"
mkdir -p "$OUT"

if [[ ! -x "$BIN/exact_index_build" || ! -x "$BIN/bm25_query" ]]; then
  echo "请先: cmake -S . -B build && cmake --build build -j" >&2
  exit 1
fi

export EXACT_INDEX_CN_STOP="$ROOT/testdata/stopwords/cn_stopwords.txt"
export EXACT_INDEX_EN_STOP="$ROOT/testdata/stopwords/en_stopwords.txt"

"$BIN/exact_index_build" \
  --xml-dir "$ROOT/testdata/corpus_tiny" \
  --out-dir "$OUT" \
  --threads 2

"$BIN/bm25_query" --data-dir "$OUT" --query "苹果 手机" --k 3 | tee "$OUT/bm25.txt"
grep -q 'id=' "$OUT/bm25.txt"

"$ROOT/scripts/smoke_query_service.sh"

echo "smoke_merged OK out=$OUT"
