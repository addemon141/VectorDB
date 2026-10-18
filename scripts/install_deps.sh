#!/usr/bin/env bash
# Linux x86-64（在 Ubuntu 22.04 上核对过）编译依赖。
# apt：编译器、CMake、tinyxml2、liburing、OpenSSL、zlib。
# 源码：protobuf 3.20.1 → /usr/local；gRPC 1.54.3 → $HOME/.local。
# 发行版自带的 protobuf 3.12 / gRPC 太旧，CMake 会优先用这两处前缀。
set -euo pipefail

PROTO_TAG=v3.20.1
GRPC_TAG=v1.54.3
SRC_ROOT="${VDB_DEPS_SRC:-$HOME/src}"
JOBS="${JOBS:-$(nproc)}"

if [[ "$(uname -s)" != "Linux" || "$(uname -m)" != "x86_64" ]]; then
  echo "本脚本只覆盖 Linux x86-64，当前是 $(uname -s)/$(uname -m)" >&2
  exit 1
fi

sudo apt-get update
sudo apt-get install -y \
  build-essential \
  cmake \
  pkg-config \
  git \
  ca-certificates \
  autoconf \
  automake \
  libtool \
  python3 \
  libtinyxml2-dev \
  liburing-dev \
  zlib1g-dev \
  libssl-dev

mkdir -p "$SRC_ROOT"

if ! protoc --version 2>/dev/null | grep -q "3.20.1"; then
  if [[ ! -d "$SRC_ROOT/protobuf" ]]; then
    git clone --depth 1 --branch "$PROTO_TAG" \
      https://github.com/protocolbuffers/protobuf.git "$SRC_ROOT/protobuf"
  fi
  cmake -S "$SRC_ROOT/protobuf" -B "$SRC_ROOT/protobuf/build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr/local \
    -Dprotobuf_BUILD_TESTS=OFF \
    -Dprotobuf_BUILD_SHARED_LIBS=ON
  cmake --build "$SRC_ROOT/protobuf/build" -j"$JOBS"
  sudo cmake --install "$SRC_ROOT/protobuf/build"
  sudo ldconfig
fi
protoc --version

if [[ ! -f "$HOME/.local/lib/cmake/grpc/gRPCConfigVersion.cmake" ]] \
   || ! grep -q '1.54.3' "$HOME/.local/lib/cmake/grpc/gRPCConfigVersion.cmake"; then
  if [[ ! -d "$SRC_ROOT/grpc" ]]; then
    git clone --depth 1 --recurse-submodules --shallow-submodules \
      --branch "$GRPC_TAG" \
      https://github.com/grpc/grpc.git "$SRC_ROOT/grpc"
  fi
  cmake -S "$SRC_ROOT/grpc" -B "$SRC_ROOT/grpc/build" \
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
  cmake --build "$SRC_ROOT/grpc/build" -j"$JOBS"
  cmake --install "$SRC_ROOT/grpc/build"
fi

export PATH="$HOME/.local/bin:$PATH"
command -v grpc_cpp_plugin
echo "依赖已就绪：protoc=$(protoc --version)，grpc_cpp_plugin=$(command -v grpc_cpp_plugin)"
