# DataNode（`vectordb_datanode`）

TLV 收语料、调用 embedding、建 BM25 与 HNSW，再按块把快照传给 Service。不跑 Raft，不链 gRPC。

平台：Linux x86-64，C++17 g++，CMake ≥ 3.16。

## 外部依赖

这个进程链接：

- tinyxml2、pthread（`exact_index_lib`）
- liburing（可选，经 `vdb_tlv`）。默认随顶层 `VDB_IO_URING=ON`；找不到库时自动关闭
- hnswlib：仓库内 `libs/vector_index/third_party/hnswlib`，header-only

JSON 使用仓库内 `third_party/nlohmann/json.hpp`。

运行期要一份 embedding HTTP（`--embed-url`）。仓库不编译这个服务；冒烟用 `scripts/fake_embed_server.py`。

本目录没有自己的 `find_package`。顶层 `CMakeLists.txt` 仍会配置 `service/`，因此**编译机**上还是要有 protobuf、`protoc`、gRPC 和 `grpc_cpp_plugin`。编出来的 `vectordb_datanode` 不加载它们。

本进程自己要的系统包：

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake libtinyxml2-dev liburing-dev
```

因为同一次 `cmake` 会配置 Service，编译机还要装 protobuf 3.20.1 和 gRPC 1.54.3。完整命令在 [service/README.md](../service/README.md)，或在仓库根目录执行 `./scripts/install_deps.sh`。

## 编译

在仓库根目录 `VectorDB/`（本文件的上一级）：

```bash
mkdir -p build && cd build
cmake ..                      # 默认打开 io_uring；关闭：cmake -DVDB_IO_URING=OFF ..
cmake --build . -j --target vectordb_datanode
```

产物：`build/bin/vectordb_datanode`。

```bash
build/bin/vectordb_datanode --listen host:port --data-dir DIR --embed-url URL \
  [--cn-stop FILE] [--en-stop FILE] [--workers N]
```

集群里怎么和 Service 配合，见仓库根目录的 [README.md](../README.md)。
