#!/usr/bin/env bash
# 同一 request_id 重试不产生第二篇；Rebuild 裁日志后仍返回并入基座的正式 id。
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/build/bin"
WORKDIR="${TMPDIR:-/tmp}/vdb_v2_idem_$$"
mkdir -p "$WORKDIR"/{dn,n1}
PIDS=()
cleanup() { for p in "${PIDS[@]:-}"; do kill "$p" 2>/dev/null || true; done; wait 2>/dev/null || true; rm -rf "$WORKDIR"; }
trap cleanup EXIT

HTTP=127.0.0.1:18011
python3 "$ROOT/scripts/fake_embed_server.py" --host 127.0.0.1 --port 18053 --dim 32 &
PIDS+=($!)
sleep 0.4
"$BIN/vectordb_datanode" --listen 127.0.0.1:16111 --data-dir "$WORKDIR/dn" \
  --embed-url http://127.0.0.1:18053/embed \
  --cn-stop "$ROOT/testdata/stopwords/cn_stopwords.txt" \
  --en-stop "$ROOT/testdata/stopwords/en_stopwords.txt" --workers 2 &
PIDS+=($!)
sleep 0.3

start_service() {
  "$BIN/vectordb_service" --node-id 1 --peers 1=127.0.0.1:17011 --http-addr "$HTTP" \
    --data-dir "$WORKDIR/n1" --datanode 127.0.0.1:16111 \
    --embed-url http://127.0.0.1:18053/embed \
    --cn-stop "$ROOT/testdata/stopwords/cn_stopwords.txt" \
    --en-stop "$ROOT/testdata/stopwords/en_stopwords.txt" &
  PIDS+=($!)
  for _ in $(seq 1 40); do curl -sf "http://$HTTP/admin/status" >/dev/null && break; sleep 0.25; done
  for _ in $(seq 1 40); do
    curl -sf "http://$HTTP/admin/status" | grep -q '"role":"leader"' && break
    sleep 0.25
  done
}
start_service

BODY='{"request_id":"doc-gamma","docs":[{"client_temp_id":1,"title":"s","link":"l","content":"idempotent gamma document unique"}]}'
curl -sf -X POST "http://$HTTP/admin/init?sync=1" -H 'Content-Type: application/json' \
  -d '[{"title":"t","link":"l","content":"alpha document about raft"}]' | grep -q '"code":"OK"'

ADD1=$(curl -sf -X POST "http://$HTTP/admin/add" -H 'Content-Type: application/json' -d "$BODY")
echo "add1=$ADD1"
SID=$(python3 -c "import json,sys; print(json.load(sys.stdin)['ids'][0]['staging_id'])" <<<"$ADD1")
[[ "$SID" -lt 0 ]]

ADD2=$(curl -sf -X POST "http://$HTTP/admin/add" -H 'Content-Type: application/json' -d "$BODY")
echo "add2=$ADD2"
echo "$ADD2" | grep -q '"idempotent":true'
SID2=$(python3 -c "import json,sys; print(json.load(sys.stdin)['ids'][0]['staging_id'])" <<<"$ADD2")
[[ "$SID2" == "$SID" ]]

DIFF=$(curl -s -o "$WORKDIR/diff.json" -w '%{http_code}' -X POST "http://$HTTP/admin/add" \
  -H 'Content-Type: application/json' \
  -d '{"request_id":"doc-gamma","docs":[{"client_temp_id":1,"title":"s","link":"l","content":"different body"}]}')
[[ "$DIFF" == "400" ]]
grep -q '"code":"INVALID"' "$WORKDIR/diff.json"

PLAIN=$(curl -sf -X POST "http://$HTTP/admin/add" -H 'Content-Type: application/json' \
  -d '{"docs":[{"client_temp_id":2,"title":"s","link":"l","content":"plain add without request id zeta"}]}')
echo "$PLAIN" | grep -q '"staging_id"'
echo "$PLAIN" | grep -vq '"idempotent":true'

RB=$(curl -sf -X POST "http://$HTTP/admin/rebuild" -H 'Content-Type: application/json' -d '{"base_version":1}')
echo "rebuild=$RB"
echo "$RB" | grep -q '"new_version":2'

ADD3=$(curl -sf -X POST "http://$HTTP/admin/add" -H 'Content-Type: application/json' -d "$BODY")
echo "add3=$ADD3"
echo "$ADD3" | grep -q '"idempotent":true'
FID=$(python3 -c "import json,sys; print(json.load(sys.stdin)['ids'][0]['staging_id'])" <<<"$ADD3")
[[ "$FID" -gt 0 ]]

SEARCH=$(curl -sf -X POST "http://$HTTP/query/search" -H 'Content-Type: application/json' \
  -d '{"query":"idempotent gamma document unique"}')
echo "search=$SEARCH"
python3 -c "import json,sys; h=json.load(sys.stdin)['hits']; assert sum(1 for x in h if x['id']==$FID)==1, h" <<<"$SEARCH"

kill "${PIDS[-1]}"
wait "${PIDS[-1]}" 2>/dev/null || true
unset 'PIDS[-1]'
sleep 0.3
start_service
ADD4=$(curl -sf -X POST "http://$HTTP/admin/add" -H 'Content-Type: application/json' -d "$BODY")
echo "add4=$ADD4"
echo "$ADD4" | grep -q '"idempotent":true'
FID2=$(python3 -c "import json,sys; print(json.load(sys.stdin)['ids'][0]['staging_id'])" <<<"$ADD4")
[[ "$FID2" == "$FID" ]]
SEARCH2=$(curl -sf -X POST "http://$HTTP/query/search" -H 'Content-Type: application/json' \
  -d '{"query":"idempotent gamma document unique"}')
python3 -c "import json,sys; h=json.load(sys.stdin)['hits']; assert sum(1 for x in h if x['id']==$FID2)==1, h" <<<"$SEARCH2"

echo "SMOKE V2 ADD IDEM OK"
