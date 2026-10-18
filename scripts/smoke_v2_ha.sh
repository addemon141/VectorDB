#!/usr/bin/env bash
# primary DataNode 不可达时，Init 切到 backup
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/build/bin"
WORKDIR="${TMPDIR:-/tmp}/vdb_v2_ha_$$"
mkdir -p "$WORKDIR"/{dn2,n1}
PIDS=()
cleanup() { for p in "${PIDS[@]:-}"; do kill "$p" 2>/dev/null || true; done; wait 2>/dev/null || true; rm -rf "$WORKDIR"; }
trap cleanup EXIT

python3 "$ROOT/scripts/fake_embed_server.py" --host 127.0.0.1 --port 50053 --dim 32 &
PIDS+=($!); sleep 0.3
# 只起 backup（6101），primary 6100 故意不听
"$BIN/vectordb_datanode" --listen 127.0.0.1:6101 --data-dir "$WORKDIR/dn2" \
  --embed-url http://127.0.0.1:50053/embed \
  --cn-stop "$ROOT/testdata/stopwords/cn_stopwords.txt" \
  --en-stop "$ROOT/testdata/stopwords/en_stopwords.txt" &
PIDS+=($!); sleep 0.3
"$BIN/vectordb_service" --node-id 1 --peers 1=127.0.0.1:7001 --http-addr 127.0.0.1:8001 \
  --data-dir "$WORKDIR/n1" --datanode 127.0.0.1:6100 --datanode-backup 127.0.0.1:6101 \
  --embed-url http://127.0.0.1:50053/embed \
  --cn-stop "$ROOT/testdata/stopwords/cn_stopwords.txt" \
  --en-stop "$ROOT/testdata/stopwords/en_stopwords.txt" &
PIDS+=($!)
for _ in $(seq 1 40); do curl -sf http://127.0.0.1:8001/admin/status >/dev/null && break; sleep 0.25; done
for _ in $(seq 1 40); do
  curl -sf http://127.0.0.1:8001/admin/status | grep -q '"role":"leader"' && break
  sleep 0.25
done

INIT=$(curl -sf -X POST 'http://127.0.0.1:8001/admin/init?sync=1' -H 'Content-Type: application/json' \
  -d '[{"title":"a","link":"l","content":"ha failover corpus"}]')
echo "init=$INIT"
echo "$INIT" | grep -q '"code":"OK"'
curl -sf http://127.0.0.1:8001/admin/status | grep -q '"datanode":"127.0.0.1:6101"'
echo "SMOKE V2 HA OK"
