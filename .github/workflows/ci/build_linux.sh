#!/bin/bash
set -euo pipefail

cd "$(cd "$(dirname "$0")/../../.." && pwd)"

echo "Fetching wasmtime..."
tools/ext/wasmtime

echo "Building runtime..."
premake5 --os=linux gmake
make -C build config=release \
        CC="zig cc -target x86_64-linux-musl" \
        CXX="zig c++ -target x86_64-linux-musl" \
        -j"$(nproc)"

echo "Building examples..."
tools/ext/build tools/example/cpp/server.cpp
tools/ext/build tools/example/rust/server.rs

echo "All checks passed."
