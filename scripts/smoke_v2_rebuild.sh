#!/usr/bin/env bash
# Init → Add → Rebuild → search；校验 staging 清空、版本 bump
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/build/bin"
WORKDIR="${TMPDIR:-/tmp}/vdb_v2_rebuild_$$"
mkdir -p "$WORKDIR"/{dn,n1}
PIDS=()
cleanup() { for p in "${PIDS[@]:-}"; do kill "$p" 2>/dev/null || true; done; wait 2>/dev/null || true; rm -rf "$WORKDIR"; }
trap cleanup EXIT

python3 "$ROOT/scripts/fake_embed_server.py" --host 127.0.0.1 --port 50053 --dim 32 &
PIDS+=($!)
sleep 0.4
"$BIN/vectordb_datanode" --listen 127.0.0.1:6100 --data-dir "$WORKDIR/dn" \
  --embed-url http://127.0.0.1:50053/embed \
  --cn-stop "$ROOT/testdata/stopwords/cn_stopwords.txt" \
  --en-stop "$ROOT/testdata/stopwords/en_stopwords.txt" --workers 2 &
PIDS+=($!)
sleep 0.3
"$BIN/vectordb_service" --node-id 1 --peers 1=127.0.0.1:7001 --http-addr 127.0.0.1:8001 \
  --data-dir "$WORKDIR/n1" --datanode 127.0.0.1:6100 \
  --embed-url http://127.0.0.1:50053/embed \
  --cn-stop "$ROOT/testdata/stopwords/cn_stopwords.txt" \
  --en-stop "$ROOT/testdata/stopwords/en_stopwords.txt" &
PIDS+=($!)

for _ in $(seq 1 40); do curl -sf http://127.0.0.1:8001/admin/status >/dev/null && break; sleep 0.25; done
for _ in $(seq 1 40); do
  curl -sf http://127.0.0.1:8001/admin/status | grep -q '"role":"leader"' && break
  sleep 0.25
done

curl -sf -X POST 'http://127.0.0.1:8001/admin/init?sync=1' -H 'Content-Type: application/json' \
  -d '[{"title":"t","link":"l","content":"alpha document about raft"}]' | grep -q '"code":"OK"'

ADD=$(curl -sf -X POST http://127.0.0.1:8001/admin/add -H 'Content-Type: application/json' \
  -d '{"docs":[{"client_temp_id":1,"title":"s","link":"l","content":"staging beta document embeddings"}]}')
echo "add=$ADD"
echo "$ADD" | grep -q '"staging_id":-1'

RB=$(curl -sf -X POST http://127.0.0.1:8001/admin/rebuild -H 'Content-Type: application/json' \
  -d '{"base_version":1}')
echo "rebuild=$RB"
echo "$RB" | grep -q '"code":"OK"'
echo "$RB" | grep -q '"new_version":2'
echo "$RB" | grep -q '"cutoff_index"'

ST=$(curl -sf http://127.0.0.1:8001/admin/status)
echo "status=$ST"
echo "$ST" | grep -q '"base_version":2'
echo "$ST" | grep -q '"rebuilding":false'

SEARCH=$(curl -sf -X POST http://127.0.0.1:8001/query/search -H 'Content-Type: application/json' \
  -d '{"query":"staging beta"}')
echo "search=$SEARCH"
echo "$SEARCH" | grep -q '"code":"OK"'
# rebuild 后应为正数正式 id
echo "$SEARCH" | grep -q '"id":[1-9]'

echo "SMOKE V2 REBUILD OK"
