# Service 节点（`vectordb_service`）

HTTP 管理与查询、Raft、内存 staging。基座文件从 DataNode 拉到本机 `base/` 后只读打开。

平台：Linux x86-64，C++17 g++，CMake ≥ 3.16。

## 外部依赖

编译期：

- protobuf 与 `protoc`（本机 `/usr/local`，3.20），用来生成 `proto/raft.proto`
- gRPC C++，且 CMake 能 `find_package(gRPC CONFIG)`；`grpc_cpp_plugin` 在 `$HOME/.local/bin` 或 `/usr/local/bin`。本机库在 `$HOME/.local`
- tinyxml2、pthread（经 `exact_index_lib`）
- liburing（可选）。顶层默认 `VDB_IO_URING=ON`，找不到 `liburing` 时该库按关闭编译

仓库内自带，不必另装：`third_party/nlohmann/json.hpp`、`third_party/httplib.h`、hnswlib 头文件。

运行期还要：DataNode 的 TLV 地址，以及一份 embedding HTTP（Add 在 propose 前算向量）。冒烟脚本用仓库里的 `scripts/fake_embed_server.py`。

顶层配置总会编译本目录，所以编 DataNode 的机器同样要装好 protobuf 和 gRPC。

安装命令（Ubuntu 22.04 x86-64；已装对版本可改跑 `../scripts/install_deps.sh`）：

```bash
sudo apt-get update
sudo apt-get install -y \
  build-essential cmake pkg-config git ca-certificates \
  autoconf automake libtool python3 \
  libtinyxml2-dev liburing-dev zlib1g-dev libssl-dev

git clone --depth 1 --branch v3.20.1 \
  https://github.com/protocolbuffers/protobuf.git "$HOME/src/protobuf"
cmake -S "$HOME/src/protobuf" -B "$HOME/src/protobuf/build" \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local \
  -Dprotobuf_BUILD_TESTS=OFF -Dprotobuf_BUILD_SHARED_LIBS=ON
cmake --build "$HOME/src/protobuf/build" -j"$(nproc)"
sudo cmake --install "$HOME/src/protobuf/build" && sudo ldconfig

git clone --depth 1 --recurse-submodules --shallow-submodules --branch v1.54.3 \
  https://github.com/grpc/grpc.git "$HOME/src/grpc"
cmake -S "$HOME/src/grpc" -B "$HOME/src/grpc/build" \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$HOME/.local" \
  -DgRPC_INSTALL=ON -DgRPC_BUILD_TESTS=OFF \
  -DgRPC_PROTOBUF_PROVIDER=package -DProtobuf_ROOT=/usr/local \
  -DgRPC_ABSL_PROVIDER=module -DgRPC_CARES_PROVIDER=module \
  -DgRPC_RE2_PROVIDER=module -DgRPC_SSL_PROVIDER=package \
  -DgRPC_ZLIB_PROVIDER=package
cmake --build "$HOME/src/grpc/build" -j"$(nproc)"
cmake --install "$HOME/src/grpc/build"
export PATH="$HOME/.local/bin:$PATH"
```

## 编译

在仓库根目录 `VectorDB/`（本文件的上一级），不要在 `service/` 里单独跑 CMake。

```bash
mkdir -p build && cd build
cmake ..                      # 默认打开 io_uring；关闭：cmake -DVDB_IO_URING=OFF ..
cmake --build . -j --target vectordb_service
cmake --build . -j --target raft_log_test
```

可执行文件：

- `build/bin/vectordb_service`
- `build/bin/raft_log_test`（只测 Raft 日志落盘，不听端口）

启动参数和 HTTP 见仓库根目录的 [README.md](../README.md)。
