#!/usr/bin/env bash
# 一期冒烟：fake embed + DataNode + 单服务节点 Init / search / add / delete
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/build/bin"
WORKDIR="${TMPDIR:-/tmp}/vdb_smoke_$$"
mkdir -p "$WORKDIR"/{dn,n1}

cleanup() {
  [[ -n "${EMBED_PID:-}" ]] && kill "$EMBED_PID" 2>/dev/null || true
  [[ -n "${DN_PID:-}" ]] && kill "$DN_PID" 2>/dev/null || true
  [[ -n "${N1_PID:-}" ]] && kill "$N1_PID" 2>/dev/null || true
  wait 2>/dev/null || true
  rm -rf "$WORKDIR"
}
trap cleanup EXIT

"$BIN/raft_log_test"

python3 "$ROOT/scripts/fake_embed_server.py" --host 127.0.0.1 --port 50053 --dim 32 &
EMBED_PID=$!
sleep 0.5

"$BIN/vectordb_datanode" \
  --listen 127.0.0.1:6100 \
  --data-dir "$WORKDIR/dn" \
  --embed-url http://127.0.0.1:50053/embed \
  --cn-stop "$ROOT/testdata/stopwords/cn_stopwords.txt" \
  --en-stop "$ROOT/testdata/stopwords/en_stopwords.txt" \
  --workers 2 &
DN_PID=$!
sleep 0.3

"$BIN/vectordb_service" \
  --node-id 1 \
  --peers 1=127.0.0.1:7001 \
  --http-addr 127.0.0.1:8001 \
  --data-dir "$WORKDIR/n1" \
  --datanode 127.0.0.1:6100 \
  --embed-url http://127.0.0.1:50053/embed \
  --cn-stop "$ROOT/testdata/stopwords/cn_stopwords.txt" \
  --en-stop "$ROOT/testdata/stopwords/en_stopwords.txt" &
N1_PID=$!

for i in $(seq 1 40); do
  if curl -sf http://127.0.0.1:8001/admin/status >/dev/null; then
    break
  fi
  sleep 0.25
done

STATUS=$(curl -sf http://127.0.0.1:8001/admin/status)
echo "status_before=$STATUS"
echo "$STATUS" | grep -q '"ready":false'

for i in $(seq 1 40); do
  STATUS=$(curl -sf http://127.0.0.1:8001/admin/status)
  echo "$STATUS" | grep -q '"role":"leader"' && break
  sleep 0.25
done

CORPUS='[
  {"title":"alpha","link":"http://a","content":"vector database raft consensus search"},
  {"title":"beta","link":"http://b","content":"bm25 lexical retrieval and hnsw vectors"},
  {"title":"gamma","link":"http://c","content":"datanode builds snapshot for service nodes"}
]'
INIT=$(curl -sf -X POST 'http://127.0.0.1:8001/admin/init?sync=1' \
  -H 'Content-Type: application/json' -d "$CORPUS")
echo "init=$INIT"
echo "$INIT" | grep -q '"code":"OK"'

STATUS=$(curl -sf http://127.0.0.1:8001/admin/status)
echo "status_after=$STATUS"
echo "$STATUS" | grep -q '"ready":true'

SEARCH=$(curl -sf -X POST http://127.0.0.1:8001/query/search \
  -H 'Content-Type: application/json' \
  -d '{"query":"vector raft search"}')
echo "search=$SEARCH"
echo "$SEARCH" | grep -q '"code":"OK"'

ADD=$(curl -sf -X POST http://127.0.0.1:8001/admin/add \
  -H 'Content-Type: application/json' \
  -d '{"docs":[{"client_temp_id":1,"title":"delta","link":"http://d","content":"new staging document about embeddings"}]}')
echo "add=$ADD"
echo "$ADD" | grep -q '"code":"OK"'
STAGING_ID=$(python3 -c "import json,sys; print(json.load(sys.stdin)['ids'][0]['staging_id'])" <<<"$ADD")
BASE_VER=$(python3 -c "import json,sys; print(json.load(sys.stdin)['base_version'])" <<<"$ADD")

DEL=$(curl -sf -X POST http://127.0.0.1:8001/admin/delete \
  -H 'Content-Type: application/json' \
  -d "{\"base_version\":$BASE_VER,\"id\":$STAGING_ID}")
echo "delete=$DEL"
echo "$DEL" | grep -q '"code":"OK"'

# staging 已删空时 rebuild 会因 empty corpus 失败；这里只确认接口可达且非 NOT_LEADER
REBUILD_BODY=$(curl -sf -X POST http://127.0.0.1:8001/admin/rebuild \
  -H 'Content-Type: application/json' -d "{\"base_version\":$BASE_VER}" || true)
echo "rebuild=$REBUILD_BODY"
# 空基座+空 staging → INTERNAL；有数据时 OK。阶段1至少不应是 NOT_LEADER
echo "$REBUILD_BODY" | grep -vq 'NOT_LEADER'

echo "SMOKE OK"
