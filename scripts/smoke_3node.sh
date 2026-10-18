#!/usr/bin/env bash
# 三服务节点 + DataNode：各节点从 DataNode 拉快照后 InstallBase，全员 ready
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/build/bin"
WORKDIR="${TMPDIR:-/tmp}/vdb_smoke3_$$"
mkdir -p "$WORKDIR"/{dn,n1,n2,n3}

PIDS=()
cleanup() {
  for p in "${PIDS[@]:-}"; do kill "$p" 2>/dev/null || true; done
  wait 2>/dev/null || true
  rm -rf "$WORKDIR"
}
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

PEERS='1=127.0.0.1:7001,2=127.0.0.1:7002,3=127.0.0.1:7003'
HTTP_PEERS='1=127.0.0.1:8001,2=127.0.0.1:8002,3=127.0.0.1:8003'
for id in 1 2 3; do
  "$BIN/vectordb_service" --node-id "$id" --peers "$PEERS" --http-peers "$HTTP_PEERS" \
    --http-addr "127.0.0.1:800$id" \
    --data-dir "$WORKDIR/n$id" --datanode 127.0.0.1:6100 \
    --embed-url http://127.0.0.1:50053/embed \
    --cn-stop "$ROOT/testdata/stopwords/cn_stopwords.txt" \
    --en-stop "$ROOT/testdata/stopwords/en_stopwords.txt" &
  PIDS+=($!)
done

wait_http() {
  local port=$1
  for _ in $(seq 1 60); do
    curl -sf "http://127.0.0.1:$port/admin/status" >/dev/null && return 0
    sleep 0.25
  done
  return 1
}
wait_http 8001
wait_http 8002
wait_http 8003

LEADER_PORT=
for port in 8001 8002 8003; do
  st=$(curl -sf "http://127.0.0.1:$port/admin/status")
  if echo "$st" | grep -q '"role":"leader"'; then
    LEADER_PORT=$port
    break
  fi
done
[[ -n "$LEADER_PORT" ]]
echo "leader_http=127.0.0.1:$LEADER_PORT"

CORPUS='[{"title":"a","link":"l","content":"raft cluster snapshot install base"},
{"title":"b","link":"l","content":"query search on any service node"}]'
INIT=$(curl -sf -X POST "http://127.0.0.1:$LEADER_PORT/admin/init?sync=1" \
  -H 'Content-Type: application/json' -d "$CORPUS")
echo "init=$INIT"
echo "$INIT" | grep -q '"code":"OK"'

for port in 8001 8002 8003; do
  st=$(curl -sf "http://127.0.0.1:$port/admin/status")
  echo "n$port=$st"
  echo "$st" | grep -q '"ready":true'
  SEARCH=$(curl -sf -X POST "http://127.0.0.1:$port/query/search" \
    -H 'Content-Type: application/json' -d '{"query":"raft snapshot"}')
  echo "$SEARCH" | grep -q '"code":"OK"'
done

echo "SMOKE 3NODE OK"
