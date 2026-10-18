#!/usr/bin/env bash
# 三节点冒烟：选主 → 经 Follower 提交（客户端按 NOT_LEADER 改投）→ 各节点可见
#   → 杀 Leader 后重新选主并继续写 → 旧 Leader 重启后重放并追上 → 全体重启后状态一致
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
OUT="${TMPDIR:-/tmp}/vdb_cluster_smoke_$$"
rm -rf "$OUT" && mkdir -p "$OUT"
source "$ROOT/scripts/smoke_lib.sh"
require_bins
trap 'rc=$?; [[ $rc -ne 0 ]] && dump_logs; cleanup_all; exit $rc' EXIT

declare -A ADMIN=([1]=127.0.0.1:50261 [2]=127.0.0.1:50271 [3]=127.0.0.1:50281)
declare -A QUERY=([1]=127.0.0.1:50262 [2]=127.0.0.1:50272 [3]=127.0.0.1:50282)
PEERS="1=${ADMIN[1]},2=${ADMIN[2]},3=${ADMIN[3]}"

id_of() { for i in 1 2 3; do if [[ "${ADMIN[$i]}" == "$1" ]]; then echo "$i"; fi; done; }
start_all() { for i in 1 2 3; do start_node "$i" "$PEERS" "${QUERY[$i]}"; done; }

build_base
start_embed 50263
start_all

LEADER=$(wait_leader "${ADMIN[1]}" "${ADMIN[2]}" "${ADMIN[3]}")
L=$(id_of "$LEADER")
F=$(( L % 3 + 1 ))
echo "leader=node$L follower=node$F"

echo "=== add via follower (redirect) ==="
cat >"$OUT/add1.jsonl" <<'EOF'
{"client_temp_id": 100, "title": "集群一", "link": "http://c1", "content": "三节点集群复制的第一篇暂存文档"}
{"client_temp_id": 101, "title": "集群二", "link": "http://c2", "content": "另一篇关于日志复制的暂存文档"}
EOF
EXPECT='client_temp_id=101 staging_id=-2' expect "$CLIENT" --admin "${ADMIN[$F]}" add "$OUT/add1.jsonl"
for i in 1 2 3; do wait_search "${QUERY[$i]}" "集群复制" '^id=-1 '; done
echo "all nodes see staging -1"

echo "=== kill leader node$L ==="
kill_node "$L"
OTHERS=()
for i in 1 2 3; do if [[ $i -ne $L ]]; then OTHERS+=("${ADMIN[$i]}"); fi; done
NEW_LEADER=$(wait_leader "${OTHERS[@]}")
echo "new leader=node$(id_of "$NEW_LEADER")"

cat >"$OUT/add2.jsonl" <<'EOF'
{"client_temp_id": 200, "title": "切主后", "link": "http://c3", "content": "切换领导者之后提交的文档"}
EOF
EXPECT='client_temp_id=200 staging_id=-3' expect "$CLIENT" --admin "$NEW_LEADER" add "$OUT/add2.jsonl"
EXPECT='code=OK' expect "$CLIENT" --admin "$NEW_LEADER" delete --version 0 --id -2

echo "=== restart old leader node$L ==="
start_node "$L" "$PEERS" "${QUERY[$L]}"
wait_search "${QUERY[$L]}" "切换领导者" '^id=-3 '
"$CLIENT" --query "${QUERY[$L]}" search "日志复制" | tee "$OUT/old_leader.txt"
absent "$OUT/old_leader.txt" '^id=-2 '

echo "=== restart all ==="
for i in 1 2 3; do stop_node "$i"; done
start_all
LEADER=$(wait_leader "${ADMIN[1]}" "${ADMIN[2]}" "${ADMIN[3]}")
for i in 1 2 3; do
  wait_search "${QUERY[$i]}" "切换领导者" '^id=-3 '
  "$CLIENT" --query "${QUERY[$i]}" search "日志复制" | tee "$OUT/final_$i.txt"
  absent "$OUT/final_$i.txt" '^id=-2 '
done
cat >"$OUT/add3.jsonl" <<'EOF'
{"title": "重启后", "link": "http://c4", "content": "全体重启之后的新文档"}
EOF
EXPECT='staging_id=-4' expect "$CLIENT" --admin "$LEADER" add "$OUT/add3.jsonl"
"$CLIENT" --admin "$LEADER" info

echo "smoke_cluster OK out=$OUT"
