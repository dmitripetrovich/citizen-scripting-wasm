# Building citizen-scripting-wasm

To build the runtime plugin on Linux you need the following dependencies:

- [premake5](https://premake.github.io/) - build system generator
- [Zig](https://ziglang.org/) - compiler (used for both the runtime and C++ resource compilation)
- [Rust](https://www.rust-lang.org/) - compiler (used for Rust resource compilation, with `wasm32-wasip1` target)

Then clone:

```bash
git clone https://github.com/dmitripetrovich/citizen-scripting-wasm.git
cd citizen-scripting-wasm
```

### Configure and build runtime

```bash
premake5 --os=linux gmake
make -C build -f citizen-scripting-wasm.make config=release \
  CC="zig cc -target x86_64-linux-musl" \
  CXX="zig c++ -target x86_64-linux-musl" \
  -j$(nproc)
```

`premake5` will automatically fetch wasmtime and generate `src/natives/` on first run.

### Install runtime

Copy `build/bin/Release/libcitizen-scripting-wasm.so` and `libcitizen-scripting-wasm.json` next to your FXServer binary.
