#!/usr/bin/env bash
# 单节点冒烟：vdb_server 以单节点 Raft 集群运行。
# 覆盖：Search（BM25）→ Add（staging_id）→ 搜到暂存文档 → Delete 基座文档 → 版本/重复删除校验
#       → Rebuild 未实现 → 重启后日志重放恢复暂存区与墓碑
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
OUT="${TMPDIR:-/tmp}/vdb_single_smoke_$$"
rm -rf "$OUT" && mkdir -p "$OUT"
source "$ROOT/scripts/smoke_lib.sh"
require_bins
trap 'rc=$?; [[ $rc -ne 0 ]] && dump_logs; cleanup_all; exit $rc' EXIT

ADMIN=127.0.0.1:50161
QUERY=127.0.0.1:50162
PEERS="1=$ADMIN"

build_base
start_embed 50163
start_node 1 "$PEERS" "$QUERY"
wait_leader "$ADMIN" >/dev/null

echo "=== search base ==="
EXPECT='^id=1 ' expect "$CLIENT" --query "$QUERY" search "苹果 手机"

echo "=== add ==="
cat >"$OUT/add.jsonl" <<'EOF'
{"client_temp_id": 7, "title": "测试文档", "link": "http://t", "content": "这是一篇关于向量检索的测试文档"}
EOF
EXPECT='client_temp_id=7 staging_id=-1' expect "$CLIENT" --admin "$ADMIN" add "$OUT/add.jsonl"

echo "=== search staging ==="
EXPECT='^id=-1 source=both' expect "$CLIENT" --query "$QUERY" search "向量检索"

echo "=== delete base id=1 ==="
EXPECT='code=OK' expect "$CLIENT" --admin "$ADMIN" delete --version 0 --id 1
"$CLIENT" --query "$QUERY" search "苹果 手机" | tee "$OUT/after_del.txt"
absent "$OUT/after_del.txt" '^id=1 '

echo "=== invalid deletes ==="
EXPECT='code=STALE_VERSION' expect "$CLIENT" --admin "$ADMIN" delete --version 5 --id 2
EXPECT='code=INVALID' expect "$CLIENT" --admin "$ADMIN" delete --version 0 --id 1
EXPECT='code=INVALID' expect "$CLIENT" --admin "$ADMIN" delete --version 0 --id -99

echo "=== rebuild (interface only) ==="
EXPECT='code=INTERNAL.*not implemented' expect "$CLIENT" --admin "$ADMIN" rebuild --version 0

echo "=== restart and replay ==="
stop_node 1
start_node 1 "$PEERS" "$QUERY"
wait_leader "$ADMIN" >/dev/null
wait_search "$QUERY" "向量检索" '^id=-1 '
"$CLIENT" --query "$QUERY" search "苹果 手机" | tee "$OUT/after_restart.txt"
absent "$OUT/after_restart.txt" '^id=1 '
cat >"$OUT/add2.jsonl" <<'EOF'
{"title": "第二篇", "link": "http://t2", "content": "重启之后继续追加的暂存文档"}
EOF
EXPECT='client_temp_id=1 staging_id=-2' expect "$CLIENT" --admin "$ADMIN" add "$OUT/add2.jsonl"

echo "smoke_query_service OK out=$OUT"
