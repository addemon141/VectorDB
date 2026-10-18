#!/usr/bin/env bash
# RebuildCmd 已提交且快照已分发、InstallBase 之前杀掉 Leader；新 Leader 续跑完成
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/build/bin"
WORKDIR="${TMPDIR:-/tmp}/vdb_v2_rbresume_$$"
mkdir -p "$WORKDIR"/{dn,n1,n2,n3}
PIDS=()
cleanup() { for p in "${PIDS[@]:-}"; do kill "$p" 2>/dev/null || true; done; wait 2>/dev/null || true; rm -rf "$WORKDIR"; }
trap cleanup EXIT

python3 "$ROOT/scripts/fake_embed_server.py" --host 127.0.0.1 --port 50053 --dim 32 &
PIDS+=($!); sleep 0.4
"$BIN/vectordb_datanode" --listen 127.0.0.1:6100 --data-dir "$WORKDIR/dn" \
  --embed-url http://127.0.0.1:50053/embed \
  --cn-stop "$ROOT/testdata/stopwords/cn_stopwords.txt" \
  --en-stop "$ROOT/testdata/stopwords/en_stopwords.txt" --workers 2 &
PIDS+=($!); sleep 0.3

PEERS='1=127.0.0.1:7001,2=127.0.0.1:7002,3=127.0.0.1:7003'
HTTP_PEERS='1=127.0.0.1:8001,2=127.0.0.1:8002,3=127.0.0.1:8003'
declare -a NODE_PID
for id in 1 2 3; do
  "$BIN/vectordb_service" --node-id "$id" --peers "$PEERS" --http-peers "$HTTP_PEERS" \
    --http-addr "127.0.0.1:800$id" --data-dir "$WORKDIR/n$id" --datanode 127.0.0.1:6100 \
    --embed-url http://127.0.0.1:50053/embed \
    --cn-stop "$ROOT/testdata/stopwords/cn_stopwords.txt" \
    --en-stop "$ROOT/testdata/stopwords/en_stopwords.txt" &
  NODE_PID[$id]=$!
  PIDS+=($!)
done

for port in 8001 8002 8003; do
  for _ in $(seq 1 60); do curl -sf "http://127.0.0.1:$port/admin/status" >/dev/null && break; sleep 0.25; done
done
LEADER=
for _ in $(seq 1 40); do
  for port in 8001 8002 8003; do
    st=$(curl -sf "http://127.0.0.1:$port/admin/status" || true)
    if echo "$st" | grep -q '"role":"leader"'; then LEADER=$port; break; fi
  done
  [[ -n "$LEADER" ]] && break
  sleep 0.25
done
[[ -n "$LEADER" ]]
OLD_ID=$(curl -sf "http://127.0.0.1:$LEADER/admin/status" | python3 -c "import json,sys; print(json.load(sys.stdin)['node_id'])")

curl -sf -X POST "http://127.0.0.1:$LEADER/admin/init?sync=1" -H 'Content-Type: application/json' \
  -d '[{"title":"t","link":"l","content":"alpha document about raft"}]' | grep -q '"code":"OK"'
curl -sf -X POST "http://127.0.0.1:$LEADER/admin/add" -H 'Content-Type: application/json' \
  -d '{"docs":[{"client_temp_id":1,"title":"s","link":"l","content":"staging beta document embeddings"}]}' \
  | grep -q '"code":"OK"'

RESP=$(curl -sf -X POST "http://127.0.0.1:$LEADER/admin/rebuild?async=1&hold_install=1" \
  -H 'Content-Type: application/json' -d '{"base_version":1}')
echo "held=$RESP"
JOB=$(python3 -c "import json,sys; print(json.load(sys.stdin)['job_id'])" <<<"$RESP")
for _ in $(seq 1 80); do
  J=$(curl -sf "http://127.0.0.1:$LEADER/admin/jobs/$JOB" || true)
  echo "job=$J"
  echo "$J" | grep -q '"phase":"installing"' && break
  echo "$J" | grep -q '"phase":"failed"' && exit 1
  sleep 0.25
done
echo "$J" | grep -q '"phase":"installing"'

ADD=$(curl -sf -X POST "http://127.0.0.1:$LEADER/admin/add" -H 'Content-Type: application/json' \
  -d '{"docs":[{"client_temp_id":7,"title":"late","link":"l","content":"after cutoff gamma document"}]}')
echo "add_during=$ADD"
echo "$ADD" | grep -q '"code":"OK"'
LATE_ID=$(python3 -c "import json,sys; print(json.load(sys.stdin)['ids'][0]['staging_id'])" <<<"$ADD")
[[ "$LATE_ID" -lt 0 ]]

kill "${NODE_PID[$OLD_ID]}"
for i in "${!PIDS[@]}"; do
  if [[ "${PIDS[$i]}" == "${NODE_PID[$OLD_ID]}" ]]; then unset 'PIDS[i]'; fi
done
wait "${NODE_PID[$OLD_ID]}" 2>/dev/null || true

NEW=
for _ in $(seq 1 80); do
  for port in 8001 8002 8003; do
    [[ "$port" == "$LEADER" ]] && continue
    st=$(curl -sf "http://127.0.0.1:$port/admin/status" || true)
    if echo "$st" | grep -q '"role":"leader"'; then NEW=$port; break; fi
  done
  [[ -n "$NEW" ]] && break
  sleep 0.25
done
[[ -n "$NEW" ]]
echo "new_leader=$NEW"

RB=$(curl -sf -X POST "http://127.0.0.1:$NEW/admin/rebuild" -H 'Content-Type: application/json' -d '{}')
echo "resume=$RB"
echo "$RB" | grep -q '"new_version":2'
HASH=$(python3 -c "import json,sys; print(json.load(sys.stdin)['content_hash'])" <<<"$RB")

for port in 8001 8002 8003; do
  [[ "$port" == "$LEADER" ]] && continue
  st=$(curl -sf "http://127.0.0.1:$port/admin/status")
  echo "n$port=$st"
  echo "$st" | grep -q '"base_version":2'
  echo "$st" | grep -q '"rebuilding":false'
  echo "$st" | grep -q "\"content_hash\":\"$HASH\""
  SEARCH=$(curl -sf -X POST "http://127.0.0.1:$port/query/search" -H 'Content-Type: application/json' \
    -d '{"query":"after cutoff gamma"}')
  echo "$SEARCH" | grep -q "\"id\":$LATE_ID"
done
echo "SMOKE V2 REBUILD RESUME OK"
