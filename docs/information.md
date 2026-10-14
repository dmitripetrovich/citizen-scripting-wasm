# Information for citizen-scripting-wasm

[citizen-scripting-wasm](https://github.com/dmitripetrovich/citizen-scripting-wasm) lets you write resources in C++ or Rust, compiled as WebAssembly modules (`.wasm`).

It provides:
1. Near native performance via wasmtime
2. Sandboxed execution so resources can only interact with the server through defined host imports
3. Stackless coroutines (`co_await` / `co_return`) for async scheduling (C++)

### How it works

The runtime plugin (`libcitizen-scripting-wasm.so`) is loaded by FXServer and handles the lifecycle:
1. Author writes one or more `.cpp` / `.rs` files and references them in `fxmanifest.lua` (see [resources.md](./resources.md))
2. Server reads `fxmanifest.lua` -> sees `server_script 'server.cpp'`, `server_scripts { 'server/*.rs' }`, etc.
3. Runtime compiles each source to `.wasm` automatically (cached until source changes)
4. Wasmtime loads each file as its own module, in manifest order, and calls its `__cfx_init`
5. `Server { }` (C++) or `Server! { }` (Rust) block runs and registers events, exports, timers, etc.
6. Runtime dispatches ticks, events, and ref calls to the handlers of every loaded file
7. On resource stop, `onStop` / `on_stop` handlers run and everything is cleaned up

### Notes

- Resources are compiled automatically on load; [Zig](https://ziglang.org/) for C++ and [Rust](https://www.rust-lang.org/) (with `wasm32-wasip1` target) for Rust.
- Resources are sandboxed, they can only interact with the server through the defined host imports.
- Child process spawning requires `set sv_wasmChildProcess "resource-name"` (or `"*"` for all) in .cfg.
- Worker threads require `set sv_wasmWorkerThreads "resource-name"` (or `"*"` for all) in .cfg.
- Resources are limited to 256MB of linear memory.
- Sources can only read files inside their own resource at compile time (`#include`, `#embed`, `include_bytes!`, `#[path]`, etc.).
- Native calls are checked against the argument types in the CFX native database, natives missing from it are rejected.
- Net events are filtered, only events registered with `cfx::onNet` / `cfx::on_net` accept calls from clients.
- Worker threads are capped at 8 per resource.
- Timers are capped at 8192 per resource.

### Limitations

- Runtime is **server-side** only, client-side is not yet supported.
- Linux is the only supported platform (all contributions are accepted for Windows support).
- C++ resources cannot use C++ exceptions (`-fno-exceptions` is required).
- `IScriptStackWalkingRuntime` submits one frame naming the running script file, and the host may not call it in all error scenarios.
- `IScriptProfiler` is not yet implemented. `profiler record` won't produce scope events from resources.
- RedM is untested.
- Both the C++ and Rust APIs (events, exports, natives, timers, statebags, etc.) have full parity with Lua, JS, and Mono-v2.
