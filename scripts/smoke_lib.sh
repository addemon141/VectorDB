# 冒烟脚本公共函数（source 使用）。调用方需先设置 ROOT、OUT。
BIN="$ROOT/build/bin"
CLIENT="$BIN/vdb_client"
CN_STOP="$ROOT/testdata/stopwords/cn_stopwords.txt"
EN_STOP="$ROOT/testdata/stopwords/en_stopwords.txt"
declare -A NODE_PID=()
EMBED_PID=""

require_bins() {
  for b in exact_index_build vdb_server vdb_client; do
    if [[ ! -x "$BIN/$b" ]]; then
      echo "缺少 $BIN/$b，请先: cmake -S . -B build && cmake --build build -j" >&2
      exit 1
    fi
  done
}

# 用 testdata/corpus_tiny 建 BM25 基座到 $OUT/base（无 HNSW，基座向量路自动关闭）
build_base() {
  EXACT_INDEX_CN_STOP="$CN_STOP" EXACT_INDEX_EN_STOP="$EN_STOP" \
    "$BIN/exact_index_build" --xml-dir "$ROOT/testdata/corpus_tiny" --out-dir "$OUT/base" \
    --threads 2 >"$OUT/build_base.log" 2>&1
}

# start_embed PORT
start_embed() {
  python3 "$ROOT/scripts/fake_embed_server.py" --port "$1" >"$OUT/embed.log" 2>&1 &
  EMBED_PID=$!
  EMBED_URL="http://127.0.0.1:$1/embed"
  for _ in $(seq 1 50); do
    curl -sS -f -m 1 -d '{"text":"ping"}' "$EMBED_URL" >/dev/null 2>&1 && return 0
    sleep 0.1
  done
  echo "fake embed server not ready" >&2
  exit 1
}

# start_node ID PEERS QUERY_ADDR   （管理地址取 PEERS 中自己的地址）
start_node() {
  local id=$1 peers=$2 qaddr=$3
  "$BIN/vdb_server" --node-id "$id" --peers "$peers" --query-addr "$qaddr" \
    --data-dir "$OUT/base" --raft-dir "$OUT/raft_n$id" --embed-url "$EMBED_URL" \
    --cn-stop "$CN_STOP" --en-stop "$EN_STOP" >>"$OUT/node$id.log" 2>&1 &
  NODE_PID[$id]=$!
}

# stop_node ID：SIGTERM 并等待退出
stop_node() {
  local pid=${NODE_PID[$1]:-}
  [[ -n "$pid" ]] || return 0
  kill "$pid" 2>/dev/null || true
  wait "$pid" 2>/dev/null || true
  unset "NODE_PID[$1]"
}

# kill_node ID：SIGKILL，模拟崩溃
kill_node() {
  local pid=${NODE_PID[$1]:-}
  [[ -n "$pid" ]] || return 0
  kill -9 "$pid" 2>/dev/null || true
  wait "$pid" 2>/dev/null || true
  unset "NODE_PID[$1]"
}

cleanup_all() {
  for id in "${!NODE_PID[@]}"; do stop_node "$id"; done
  [[ -n "$EMBED_PID" ]] && { kill "$EMBED_PID" 2>/dev/null || true; wait "$EMBED_PID" 2>/dev/null || true; }
}

# wait_leader ADMIN_ADDR...：轮询直到所有给出的节点都报告同一个 Leader，且该 Leader 自认 Leader，
# 输出其管理地址。超时（~60s）返回 1
wait_leader() {
  local leader info a ok
  for _ in $(seq 1 300); do
    leader=""
    ok=1
    for a in "$@"; do
      info=$("$CLIENT" --admin "$a" info 2>/dev/null) || { ok=0; break; }
      local l
      l=$(sed -n 's/.* leader=\([^ ]*\).*/\1/p' <<<"$info")
      if [[ -z "$l" || ( -n "$leader" && "$l" != "$leader" ) ]]; then ok=0; break; fi
      leader=$l
    done
    if [[ $ok == 1 ]] && "$CLIENT" --admin "$leader" info 2>/dev/null | grep -q 'is_leader=1'; then
      echo "$leader"
      return 0
    fi
    sleep 0.2
  done
  echo "no agreed leader among $*" >&2
  for a in "$@"; do "$CLIENT" --admin "$a" info >&2 || true; done
  return 1
}

# wait_search QUERY_ADDR TEXT REGEX：轮询直到搜索结果匹配 REGEX（~10s）
wait_search() {
  for _ in $(seq 1 50); do
    "$CLIENT" --query "$1" search "$2" 2>/dev/null | grep -qE "$3" && return 0
    sleep 0.2
  done
  echo "search '$2' on $1 never matched /$3/; last output:" >&2
  "$CLIENT" --query "$1" search "$2" >&2 || true
  return 1
}

# expect CMD... ：运行命令、打印输出，并要求输出匹配 $EXPECT（正则）
expect() {
  local out
  out=$("$@" 2>&1) || true
  echo "$out"
  if ! grep -qE "$EXPECT" <<<"$out"; then
    echo "expected /$EXPECT/ from: $*" >&2
    return 1
  fi
}

# absent FILE REGEX：FILE 中不得出现 REGEX（set -e 下 `! grep` 不会使脚本失败，故单独封装）
absent() {
  if grep -qE "$2" "$1"; then
    echo "unexpected /$2/ in $1" >&2
    return 1
  fi
}

dump_logs() {
  for f in "$OUT"/node*.log; do
    echo "---- $f (tail) ----" >&2
    tail -20 "$f" >&2 || true
  done
}
