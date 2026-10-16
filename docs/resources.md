# Writing resources

Resources are written in C++ or Rust. Reference `.cpp` or `.rs` files in your `fxmanifest.lua` and the runtime compiles automatically.

See [tools/example](../tools/example) for a C++/Rust resource using events, net events, commands, exports, natives, state bags, timers and workers.

### Entry files

An entry file runs on its own and has one entry block:

```cpp
#include <cfx.h>

Server
{
    std::println("hello from C++");
}
```

```rust
use cfx::*;

Server! {
    println!("hello from Rust");
}
```

`#include <cfx.h>` and `use cfx::*;` bring in the whole SDK and every native, C++ also gets the common standard headers such as `<print>`, `<string>`, `<vector>` and `<unordered_map>`.

Anything else still needs its own include or `use`, just like any other C++ or Rust program, for example `#include <regex>` or `use std::collections::HashMap;`.

Reference it in your manifest:

```lua
server_script 'server.cpp'
```

```lua
server_script 'server.rs'
```

List several entry files:

```lua
server_scripts {
    'server/admin.cpp',
    'server/mods.cpp',
    'server/jobs.rs',
}
```

Or use globs:

```lua
server_scripts {
    'server/*.cpp',
    'server/*.rs',
}
```

Files from another resource are referenced with `@`:

```lua
server_scripts {
    '@lib/server/logger.cpp',
    'server/main.cpp',
}
```

Each entry file loads in manifest order and keeps its own state.

Give every entry file a unique name: `admin.cpp` and `admin.rs` both compile to `admin.wasm`, so the second one fails to load.

### Helper files

Files without an entry block are helpers. They are compiled into every entry file that uses them and are skipped when a glob matches them:

```
server/
  main.cpp
  util.h
  util.cpp
  jobs.rs
  payroll.rs
```

C++ uses a helper through its header. `util.cpp` is linked into `main.cpp` because `main.cpp` includes `util.h`.

`util.h` declares what the helper provides:

```cpp
#pragma once
#include <string>

std::string greet();
```

`util.cpp` is the helper, no entry block:

```cpp
#include <cfx.h>
#include "util.h"

std::string greet()
{
    return "hello from " + cfx::getCurrentResourceName();
}
```

`main.cpp` is the entry file that uses it:

```cpp
#include <cfx.h>
#include "util.h"

Server
{
    std::println("{}", greet());
}
```

Rust uses a helper through `mod`. `payroll.rs` is compiled into `jobs.rs` because `jobs.rs` declares `mod payroll;`.

`payroll.rs` is the helper, no entry block:

```rust
pub fn start() {
    cfx::set_interval(60000, || {
        println!("paying everyone");
    });
}
```

`jobs.rs` is the entry file that uses it:

```rust
use cfx::*;
mod payroll;

Server! {
    payroll::start();
}
```

Helpers can use the SDK and can include or declare further helpers. Editing a helper rebuilds every entry file that uses it.

### Caching and pre-compiling

The compiled `.wasm` is cached next to the source and only rebuilt when the source or anything it uses changes.

You can pre-compile with `tools/ext/build` and reference the `.wasm` directly. Pass C++ helpers after the entry file:

```bash
tools/ext/build server/main.cpp server/util.cpp
tools/ext/build server/jobs.rs
```

A `.wasm` with its source next to it is treated as build output and skipped, so ship pre-compiled resources without the source.
