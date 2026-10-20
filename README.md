# VectorDB 第二版（Service + DataNode）

Service 负责 HTTP、Raft 和查询。DataNode 负责把语料建成只读基座。语料和快照文件不进 Raft；Raft 日志里只有命令（Add / Delete / Rebuild / InstallBase / Noop）。

## 构建

平台：Linux x86-64。编译器是支持 C++17 的 g++，CMake 不低于 3.16。配置时会把 `$HOME/.local` 和 `/usr/local` 加到 `CMAKE_PREFIX_PATH` 前面，本机自编译的 protobuf / gRPC 放在这两处即可被找到。

仓库内自带、不需要另装：`third_party/nlohmann/json.hpp`、`third_party/httplib.h`、`libs/vector_index/third_party/hnswlib`。

顶层 `CMakeLists.txt` 总会进入 `service/`，所以**编译机**上 protobuf 和 gRPC 必须齐全，即使这次只想产出 DataNode。运行时只有 Service 进程加载它们。

| 依赖 | Service | DataNode | 说明 |
|------|---------|----------|------|
| g++、CMake ≥ 3.16、pthread | 要 | 要 | |
| tinyxml2 | 要 | 要 | `exact_index_lib` 解析语料 |
| liburing | 可选 | 可选 | 默认 `VDB_IO_URING=ON`；找不到库时自动关掉 |
| protobuf、`protoc` | 要 | 编译机要 | `proto/raft.proto`；本机为 `/usr/local` 的 3.20 |
| gRPC C++、`grpc_cpp_plugin` | 要 | 编译机要 | CMake `gRPC CONFIG`；本机在 `$HOME/.local` |
| embedding HTTP | 运行时要 | 运行时要 | 仓库不编译这个服务。冒烟用 `scripts/fake_embed_server.py`（Python 3） |

在 Ubuntu 22.04 x86-64 上，发行版的 protobuf 3.12 不够。下面这组和当前仓库的 CMake 查找路径一致：apt 装编译器和 tinyxml2 / liburing，protobuf **3.20.1** 装到 `/usr/local`，gRPC **1.54.3** 装到 `$HOME/.local`。脚本是 `scripts/install_deps.sh`（已装对版本会跳过源码编译）：

```bash
chmod +x scripts/install_deps.sh
./scripts/install_deps.sh
```

等价的命令如下。源码目录默认 `$HOME/src`，可用 `VDB_DEPS_SRC` 改。

```bash
sudo apt-get update
sudo apt-get install -y \
  build-essential cmake pkg-config git ca-certificates \
  autoconf automake libtool python3 \
  libtinyxml2-dev liburing-dev zlib1g-dev libssl-dev

git clone --depth 1 --branch v3.20.1 \
  https://github.com/protocolbuffers/protobuf.git "$HOME/src/protobuf"
cmake -S "$HOME/src/protobuf" -B "$HOME/src/protobuf/build" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr/local \
  -Dprotobuf_BUILD_TESTS=OFF \
  -Dprotobuf_BUILD_SHARED_LIBS=ON
cmake --build "$HOME/src/protobuf/build" -j"$(nproc)"
sudo cmake --install "$HOME/src/protobuf/build"
sudo ldconfig

git clone --depth 1 --recurse-submodules --shallow-submodules --branch v1.54.3 \
  https://github.com/grpc/grpc.git "$HOME/src/grpc"
cmake -S "$HOME/src/grpc" -B "$HOME/src/grpc/build" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$HOME/.local" \
  -DgRPC_INSTALL=ON \
  -DgRPC_BUILD_TESTS=OFF \
  -DgRPC_PROTOBUF_PROVIDER=package \
  -DProtobuf_ROOT=/usr/local \
  -DgRPC_ABSL_PROVIDER=module \
  -DgRPC_CARES_PROVIDER=module \
  -DgRPC_RE2_PROVIDER=module \
  -DgRPC_SSL_PROVIDER=package \
  -DgRPC_ZLIB_PROVIDER=package
cmake --build "$HOME/src/grpc/build" -j"$(nproc)"
cmake --install "$HOME/src/grpc/build"
export PATH="$HOME/.local/bin:$PATH"
```

gRPC 使用 `/usr/local` 里的 protobuf，abseil / c-ares / re2 随 gRPC 源码编进 `$HOME/.local`。`grpc_cpp_plugin` 在 `$HOME/.local/bin`。

```bash
cd VectorDB
mkdir -p build && cd build
cmake ..                      # 默认 VDB_IO_URING=ON
# cmake -DVDB_IO_URING=OFF ..
cmake --build . -j
```

只编某一个可执行文件：

```bash
cmake --build . -j --target vectordb_service
cmake --build . -j --target vectordb_datanode
cmake --build . -j --target raft_log_test
```

产物在 `build/bin/`：`vectordb_service`、`vectordb_datanode`、`raft_log_test`。`service/` 和 `datanode/` 不是独立 CMake 工程，不要在子目录里单独 `cmake`。各节点自己的依赖说明见 [service/README.md](service/README.md) 和 [datanode/README.md](datanode/README.md)。

## 进程与端口

| 进程 | 作用 |
|------|------|
| `vectordb_datanode` | TLV 收语料、建库、按块提供快照 |
| `vectordb_service` | 管理 HTTP + 查询 HTTP；Raft gRPC 与 peers 通信 |
| embedding HTTP | DataNode 建库、Leader 处理 Add 时调用。冒烟用 `scripts/fake_embed_server.py` |

多节点时每个 Service 必须带 `--http-peers`。Leader 用它通知其他节点执行 `POST /internal/fetch_snapshot`，从 DataNode 拉同一份快照。

```text
vectordb_datanode --listen host:port --data-dir DIR --embed-url URL \
  [--cn-stop FILE] [--en-stop FILE] [--workers N]

vectordb_service --node-id N --peers 1=host:raft,... --http-addr host:port \
  --data-dir DIR --datanode host:port \
  [--datanode-backup host:port] [--http-peers 1=host:http,...] \
  [--raft-dir DIR] [--embed-url URL] [--cn-stop FILE] [--en-stop FILE]
```

`--peers` 含自己。Raft 地址取本节点在 `--peers` 里的那一项。`--raft-dir` 默认是 `{data-dir}/raft_n{id}`。

### 单节点

```bash
python3 scripts/fake_embed_server.py --host 127.0.0.1 --port 50053 --dim 32 &
build/bin/vectordb_datanode --listen 127.0.0.1:6100 --data-dir /tmp/dn \
  --embed-url http://127.0.0.1:50053/embed \
  --cn-stop testdata/stopwords/cn_stopwords.txt \
  --en-stop testdata/stopwords/en_stopwords.txt &
build/bin/vectordb_service --node-id 1 --peers 1=127.0.0.1:7001 \
  --http-addr 127.0.0.1:8001 --data-dir /tmp/n1 --datanode 127.0.0.1:6100 \
  --embed-url http://127.0.0.1:50053/embed \
  --cn-stop testdata/stopwords/cn_stopwords.txt \
  --en-stop testdata/stopwords/en_stopwords.txt
```

空目录可以直接启动。`GET /admin/status` 里 `ready` 为 false，直到 Init 完成。

## 磁盘上有什么

Service 的 `--data-dir`：

| 路径 | 内容 |
|------|------|
| `base/` | 当前基座：`docs.data`、`offset.data`、倒排、`vectors.hnsw`、`SNAPSHOT.hash` |
| `raft_n{id}/log.bin` | Raft 日志。每条是 `term + index + Command` |
| `raft_n{id}/hard_state.pb` | 必须落盘的 `current_term`、`voted_for`。重启不归零 |
| `raft_n{id}/snapshot_meta.pb` | 日志裁剪书签：裁到哪个 index/term、对应基座 hash，以及已被裁掉的 Add `request_id` |
| `jobs/*.json` | 本机 Init/Rebuild 任务进度，不随 Leader 迁移 |

`commit_index` 不写入 `hard_state.pb`。进程重启后从 `snapshot_meta` 和日志重放恢复。

DataNode 的 `--data-dir`：`corpus.json`、`snapshot/`（建库产物）、`job.json`（`job_id`、已收字节、状态、hash）。两台 DataNode 互相同步；backup 是冷备，失败后整次任务从头在 backup 上做。

## 数据路径

### Init

`POST /admin/init` 默认 **202** + `job_id`。Leader 把语料交给 DataNode 建库，本机拉取，再通知 peer 拉取，最后 `propose(InstallBase)`，`base_version` 变为 1。`?sync=1` 会阻塞到结束。进度：`GET /admin/jobs/{id}` 或 `GET /admin/init/{id}`。

同一 `job_id` 再提交时，Leader 看 DataNode 的 `job.json`，从已收偏移续传，或在快照已就绪时跳过上传和建库。

### Add / Delete

只发给当前 Leader。Add 在 propose 前按篇算 embedding；超过 `max_batch_docs` / `max_batch_bytes` 会拆成多条顺序 propose，合并返回 `staging_id`（负数）。单篇超过 `max_content_bytes` 返回 400。进行中的 propose 达到上限返回 `503` / `BUSY`。

这些命令 apply 后只改内存 staging，不改 `base/` 文件。耐久性在 Raft 日志里。重启后重放 `index` 大于裁剪点的日志，把 staging 重建出来。Rebuild 期间仍接受 Add / Delete：它们的日志下标大于 `cutoff_index`，不会进入正在建的那份快照。

选举期间（本节点不是 Leader）写请求立刻返回 `421` / `NOT_LEADER`，`leader_hint` 可能为空。已经 append 但领导权丢失时返回 `504` / `TIMEOUT`，结果未知。带上稳定的 `request_id` 再提交同一份 `docs` 时，apply 发现该 id 已经执行过，就返回原来的 id，不再新建文档。响应里会有 `"idempotent": true`。`request_id` 为空则和以前一样，每次都是一篇新文档。同一 id 配了不同正文返回 `400` / `INVALID`。

一次 Add 可能按批拆成多条日志，每条的去重键是 `request_id#起始下标`。重试要带上原来的整份 `docs` 和同一个 `request_id`。并入基座后，`log_index <= cutoff` 的记录改写成新的正式 id，写进 `snapshot_meta.pb`，日志裁掉之后重试仍然对得上。

### Rebuild

1. Leader `propose(RebuildCmd)`。apply 时置 `rebuilding`，`cutoff_index` 等于该日志下标，并冻结这一刻的合并语料。此后 Init 仍返回 `503` / `REBUILDING`。Add / Delete 可以继续，但不会改这份冻结语料。
2. 导出冻结语料：基座 id 升序且非墓碑，再 staging id 从 -1 到 -N，重编为正式 id `1..K`，得到 `mapping`。
3. 与 Init 相同：DataNode 建库，各节点拉取。拉上新基座时丢掉 `log_index <= cutoff` 的暂存项，避免和快照里的副本叠在一起；更晚的暂存项保留。
4. `propose(InstallBase{hash, old_version+1, cutoff})`。apply 后升版本、退出 rebuilding，再按 cutoff 收一次：只删除 cutoff 及之前创建的 staging，并把 cutoff 之后对旧 id 的删除改记到新基座 id。同时裁掉 `index <= cutoff` 的日志。

`POST /admin/rebuild` 默认同步，返回 `mapping`、`new_version`、`cutoff_index`。`?async=1` 返回 202。若集群已经 `rebuilding`（例如旧 Leader 在 InstallBase 前下台），再调一次不会发第二条 `RebuildCmd`：基座仍是旧 hash 就重新导出；本机已经换上新快照则只做分发和 InstallBase。连不上的 peer 会跳过。

`?async=1&hold_install=1` 停在 InstallBase 之前，只给换主冒烟用。

查询在 Rebuild 完成前可能同时看到新基座，以及 cutoff 之后新增、尚未并入基座的 staging。

### 查询

`POST /query/search` 读本节点已经 apply 的视图，允许比 Leader 略旧。不要求把读请求打到 Leader。

## HTTP API

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/admin/status` | `ready`、`rebuilding`、`role`、`base_version`、`cutoff_index`、`content_hash`、`datanode` |
| POST | `/admin/init` | 语料 JSON 数组，或 `{"docs":[...]}`。续传可带 `job_id` |
| GET | `/admin/jobs/{id}` | `phase`：queued / uploading / building / distributing / installing / done / failed |
| POST | `/admin/add` | `{"request_id"?,"docs":[{"client_temp_id","title","link","content"}]}`。相同 `request_id` 与正文返回原 id |
| POST | `/admin/delete` | `{"id","base_version"}` |
| POST | `/admin/rebuild` | `{"base_version"}` 可省略，默认当前版本 |
| POST | `/query/search` | `{"query"}` |
| POST | `/internal/fetch_snapshot` | `{"datanode","content_hash"}`。非 rebuilding 且 hash 不同则 409 |

常见错误码：`NOT_LEADER` 421，`NOT_READY` / `BUSY` / `REBUILDING` 503，`INVALID` / `STALE_VERSION` 400，`TIMEOUT` 504。

## Raft（运维需要知道的部分）

每个远程 peer 一条 `peer_loop`：Follower 不主动发；PreCandidate / Candidate 发 `RequestVote`；Leader 发 `AppendEntries`（空包即心跳）。入站在 gRPC 线程里处理，先拿 `RaftNode` 的锁再改任期、日志和 `commit_index`。

选举超时先 PreVote（不升 term、不落盘投票），多数同意后再正式选举。Leader 上任会追加一条 no-op，用来提交上一任期留下的日志。

`commit_index` 在一次进程运行中只增不减。`last_applied` 是本节点已经交给状态机执行到的下标，可以落后于 `commit_index`。

## 冒烟

需要本机可绑定端口。脚本会拉起 fake embedding、DataNode 和 Service，结束时清理。

```bash
./scripts/smoke_phase1.sh              # 单节点 Init / search / add / delete / rebuild
./scripts/smoke_3node.sh               # 三节点 Init，全员 ready
./scripts/smoke_v2_rebuild.sh          # 单节点 add → rebuild → 新 id
./scripts/smoke_v2_rebuild_3node.sh    # 三节点 Rebuild，hash 一致
./scripts/smoke_v2_rebuild_resume.sh   # InstallBase 前杀掉 Leader，新 Leader 续跑
./scripts/smoke_v2_add_idem.sh         # 同一 request_id 重试；Rebuild 与重启后仍是同一篇
./scripts/smoke_v2_init_async.sh
./scripts/smoke_v2_ha.sh               # primary 不可达时改打 backup
build/bin/raft_log_test
```

## 已交付与未做

已交付：Rebuild 与 `cutoff_index` 裁剪、io_uring 快照读写、Init 异步与 DataNode 续传、Add 拆批与背压、PreVote、DataNode primary/backup、多节点 Rebuild 与换主续跑。

未做：

- Admin 鉴权、TLS、指标、正式 CI
- 强制读 Leader（当前允许略旧）
- embedding 服务的生产部署
- `TIMEOUT` 之后若不带同一个 `request_id` 重试，仍可能产生第二条 Add
