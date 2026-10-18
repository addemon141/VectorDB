#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/build/bin"
WORKDIR="${TMPDIR:-/tmp}/vdb_v2_async_$$"
mkdir -p "$WORKDIR"/{dn,n1}
PIDS=()
cleanup() { for p in "${PIDS[@]:-}"; do kill "$p" 2>/dev/null || true; done; wait 2>/dev/null || true; rm -rf "$WORKDIR"; }
trap cleanup EXIT

python3 "$ROOT/scripts/fake_embed_server.py" --host 127.0.0.1 --port 50053 --dim 32 &
PIDS+=($!); sleep 0.3
"$BIN/vectordb_datanode" --listen 127.0.0.1:6100 --data-dir "$WORKDIR/dn" \
  --embed-url http://127.0.0.1:50053/embed \
  --cn-stop "$ROOT/testdata/stopwords/cn_stopwords.txt" \
  --en-stop "$ROOT/testdata/stopwords/en_stopwords.txt" &
PIDS+=($!); sleep 0.3
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

RESP=$(curl -sf -X POST http://127.0.0.1:8001/admin/init -H 'Content-Type: application/json' \
  -d '[{"title":"a","link":"l","content":"async init corpus raft"}]')
echo "init=$RESP"
echo "$RESP" | grep -q '"job_id"'
JOB=$(python3 -c "import json,sys; print(json.load(sys.stdin)['job_id'])" <<<"$RESP")

DONE=0
for _ in $(seq 1 80); do
  J=$(curl -sf "http://127.0.0.1:8001/admin/jobs/$JOB" || curl -sf "http://127.0.0.1:8001/admin/init/$JOB")
  echo "job=$J"
  if echo "$J" | grep -q '"phase":"done"'; then DONE=1; break; fi
  if echo "$J" | grep -q '"phase":"failed"'; then exit 1; fi
  sleep 0.25
done
[[ "$DONE" == "1" ]]
curl -sf http://127.0.0.1:8001/admin/status | grep -q '"ready":true'
echo "SMOKE V2 ASYNC INIT OK"
