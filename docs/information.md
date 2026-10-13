# Information for citizen-scripting-wasm

[citizen-scripting-wasm](https://github.com/dmitripetrovich/citizen-scripting-wasm) lets you write resources in C++ or Rust, compiled as WebAssembly modules (`.wasm`).

It provides:
1. Near native performance via wasmtime
2. Sandboxed execution so resources can only interact with the server through defined host imports
3. Stackless coroutines (`co_await` / `co_return`) for async scheduling (C++)

### How it works

The runtime plugin (`libcitizen-scripting-wasm.so`) is loaded by FXServer and handles the lifecycle:
1. Author writes `server.cpp` or `server.rs` and references it in `fxmanifest.lua` (see [building.md](./building.md))
2. Server reads `fxmanifest.lua` -> sees `server_script 'server.cpp'` or `server_script 'server.rs'`
3. Runtime compiles the source to `.wasm` automatically (cached until source changes)
4. Wasmtime loads the module and calls `__cfx_init`
5. `Server { }` (C++) or `Server! { }` (Rust) block runs and registers events, exports, timers, etc.
6. Runtime dispatches ticks, events, and ref calls to your handlers
7. On resource stop, `onStop` / `on_stop` handlers run and everything is cleaned up

### Notes

- Resources are compiled automatically on load; [Zig](https://ziglang.org/) must be available for C++ and [Rust](https://www.rust-lang.org/) (with `wasm32-wasip1` target) for Rust.
- Resources are sandboxed, they can only interact with the server through the defined host imports.
- Child process spawning requires `set sv_wasmChildProcess "resource-name"` (or `"*"` for all) in .cfg.
- Worker threads require `set sv_wasmWorkerThreads "resource-name"` (or `"*"` for all) in .cfg.
- Resources are limited to 256MB of linear memory.
- Net events are filtered, only events registered with `cfx::onNet` / `cfx::on_net` accept calls from clients.
- Worker threads are capped at 8 per resource.
- Timers are capped at 8192 per resource.

### Limitations

- Runtime is **server-side** only, client-side is not yet supported.
- Linux is the only supported platform (all contributions are accepted for Windows support).
- C++ resources cannot use C++ exceptions (`-fno-exceptions` is required).
- `IScriptStackWalkingRuntime` submits a basic frame but the host may not call it in all error scenarios.
- `IScriptProfiler` is not yet implemented. `profiler record` won't produce scope events from resources.
- RedM is untested.
- Both the C++ and Rust APIs (events, exports, natives, timers, statebags, etc.) have full parity with Lua, JS, and Mono-v2.
