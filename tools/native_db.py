#!/usr/bin/env python3
import hashlib
import json
import os
import struct
import sys
import time
import urllib.request
from collections import defaultdict

CACHE_DIR = os.path.join(os.path.dirname(__file__), ".cache")
CACHE_TTL = 3600

CFX_URL = "https://runtime.fivem.net/doc/natives_cfx.json"
GAME_URLS = {
        "fivem": "https://raw.githubusercontent.com/alloc8or/gta5-nativedb-data/master/natives.json",
        "redm": "https://raw.githubusercontent.com/alloc8or/rdr3-nativedb-data/master/natives.json",
}
CFX_GAME_NAMES = {"fivem": "gta5", "redm": "rdr3"}
SERVER_APISETS = ("server", "shared")

PARAM_TYPE_MAP = {
        "int": "int",
        "float": "float",
        "BOOL": "bool",
        "bool": "bool",
        "char*": "const char*",
        "Hash": "int",
        "Entity": "int",
        "Player": "int",
        "Vehicle": "int",
        "Ped": "int",
        "Object": "int",
        "Pickup": "int",
        "Blip": "int",
        "Cam": "int",
        "FireId": "int",
        "Interior": "int",
        "ScrHandle": "int",
        "Any": "int",
        "long": "int64_t",
        "int*": "int*",
        "float*": "float*",
        "BOOL*": "int*",
        "Hash*": "int*",
        "Entity*": "int*",
        "Player*": "int*",
        "Vehicle*": "int*",
        "Ped*": "int*",
        "Object*": "int*",
        "Pickup*": "int*",
        "Blip*": "int*",
        "Cam*": "int*",
        "FireId*": "int*",
        "Interior*": "int*",
        "ScrHandle*": "int*",
        "Any*": "int*",
        "long*": "int64_t*",
        "Vector3*": "Vector3*",
}

RUST_PARAM_TYPE_MAP = {
        "int": ("i32", "val", "{p} as u32 as u64"),
        "float": ("f32", "val", "{p}.to_bits() as u64"),
        "BOOL": ("bool", "val", "if {p} {{ 1u64 }} else {{ 0u64 }}"),
        "bool": ("bool", "val", "if {p} {{ 1u64 }} else {{ 0u64 }}"),
        "char*": ("&str", "str", None),
        "Hash": ("i32", "val", "{p} as u32 as u64"),
        "Entity": ("i32", "val", "{p} as u32 as u64"),
        "Player": ("i32", "val", "{p} as u32 as u64"),
        "Vehicle": ("i32", "val", "{p} as u32 as u64"),
        "Ped": ("i32", "val", "{p} as u32 as u64"),
        "Object": ("i32", "val", "{p} as u32 as u64"),
        "Pickup": ("i32", "val", "{p} as u32 as u64"),
        "Blip": ("i32", "val", "{p} as u32 as u64"),
        "Cam": ("i32", "val", "{p} as u32 as u64"),
        "FireId": ("i32", "val", "{p} as u32 as u64"),
        "Interior": ("i32", "val", "{p} as u32 as u64"),
        "ScrHandle": ("i32", "val", "{p} as u32 as u64"),
        "Any": ("i32", "val", "{p} as u32 as u64"),
        "long": ("i64", "val", "{p} as u64"),
        "int*": ("&mut i32", "ptr", "{p} as *mut i32 as u64"),
        "float*": ("&mut f32", "ptr", "{p} as *mut f32 as u64"),
        "BOOL*": ("&mut i32", "ptr", "{p} as *mut i32 as u64"),
        "Hash*": ("&mut i32", "ptr", "{p} as *mut i32 as u64"),
        "Entity*": ("&mut i32", "ptr", "{p} as *mut i32 as u64"),
        "Player*": ("&mut i32", "ptr", "{p} as *mut i32 as u64"),
        "Vehicle*": ("&mut i32", "ptr", "{p} as *mut i32 as u64"),
        "Ped*": ("&mut i32", "ptr", "{p} as *mut i32 as u64"),
        "Object*": ("&mut i32", "ptr", "{p} as *mut i32 as u64"),
        "Pickup*": ("&mut i32", "ptr", "{p} as *mut i32 as u64"),
        "Blip*": ("&mut i32", "ptr", "{p} as *mut i32 as u64"),
        "Cam*": ("&mut i32", "ptr", "{p} as *mut i32 as u64"),
        "FireId*": ("&mut i32", "ptr", "{p} as *mut i32 as u64"),
        "Interior*": ("&mut i32", "ptr", "{p} as *mut i32 as u64"),
        "ScrHandle*": ("&mut i32", "ptr", "{p} as *mut i32 as u64"),
        "Any*": ("&mut i32", "ptr", "{p} as *mut i32 as u64"),
        "long*": ("&mut i64", "ptr", "{p} as *mut i64 as u64"),
        "Vector3*": ("&mut crate::Vector3", "ptr", "{p} as *mut crate::Vector3 as u64"),
}

RUST_RETURN_TYPE_MAP = {
        "void": (None, "invoke_native_void"),
        "int": ("i32", "invoke_native_int"),
        "float": ("f32", "invoke_native_float"),
        "BOOL": ("bool", "invoke_native_bool"),
        "bool": ("bool", "invoke_native_bool"),
        "char*": ("String", "invoke_native_string"),
        "Hash": ("i32", "invoke_native_int"),
        "Entity": ("i32", "invoke_native_int"),
        "Player": ("i32", "invoke_native_int"),
        "Vehicle": ("i32", "invoke_native_int"),
        "Ped": ("i32", "invoke_native_int"),
        "Object": ("i32", "invoke_native_int"),
        "Pickup": ("i32", "invoke_native_int"),
        "Blip": ("i32", "invoke_native_int"),
        "Cam": ("i32", "invoke_native_int"),
        "FireId": ("i32", "invoke_native_int"),
        "Interior": ("i32", "invoke_native_int"),
        "ScrHandle": ("i32", "invoke_native_int"),
        "Any": ("i32", "invoke_native_int"),
        "long": ("i64", "invoke_native_i64"),
        "Vector3": ("crate::Vector3", "invoke_native_vector3"),
}

RUST_KEYWORDS = {
        "as", "async", "await", "break", "const", "continue", "crate", "dyn", "else",
        "enum", "extern", "false", "fn", "for", "if", "impl", "in", "let", "loop",
        "match", "mod", "move", "mut", "pub", "ref", "return", "self", "Self", "static",
        "struct", "super", "trait", "true", "type", "unsafe", "use", "where", "while",
        "abstract", "become", "box", "do", "final", "macro", "override", "priv", "try",
        "typeof", "unsized", "virtual", "yield", "union", "bool",
}

RETURN_TYPE_MAP = {
        "void": ("void", None),
        "int": ("int", "int"),
        "float": ("float", "float"),
        "BOOL": ("bool", "bool"),
        "bool": ("bool", "bool"),
        "char*": ("std::string", "std::string"),
        "Hash": ("int", "int"),
        "Entity": ("int", "int"),
        "Player": ("int", "int"),
        "Vehicle": ("int", "int"),
        "Ped": ("int", "int"),
        "Object": ("int", "int"),
        "Pickup": ("int", "int"),
        "Blip": ("int", "int"),
        "Cam": ("int", "int"),
        "FireId": ("int", "int"),
        "Interior": ("int", "int"),
        "ScrHandle": ("int", "int"),
        "Any": ("int", "int"),
        "long": ("int64_t", "int64_t"),
        "Vector3": ("Vector3", "Vector3"),
}


def screaming_to_pascal(name: str) -> str:
        return "".join(word.capitalize() for word in name.split("_"))


def to_safe_param(name: str) -> str:
        keywords = {"class", "struct", "enum", "union", "template", "operator", "new", "delete", "this", "return", "switch", "case", "default", "break", "continue", "goto", "if", "else", "for", "while", "do", "try", "catch", "throw", "register", "auto", "const", "static", "extern", "volatile", "inline", "virtual", "explicit", "friend", "namespace", "using", "typedef", "typename", "public", "private", "protected", "override", "final", "nullptr", "true", "false", "bool", "int", "float", "double", "char", "long", "short", "signed", "unsigned", "void", "object", "near", "far"}
        return name + "_" if name in keywords else name


ENTITY_LIST_NATIVES = {"GET_ALL_OBJECTS", "GET_ALL_PEDS", "GET_ALL_VEHICLES", "GET_GAME_POOL", "GET_ENTITIES_IN_RADIUS"}


def generate_wrapper(native: dict) -> str | None:
        name = native.get("name")
        if not name or name.startswith("0x") or name.startswith("_0x"):
                return None
        hash_str = native.get("hash", "0x0")
        params = native.get("params", [])
        result_type = native.get("results", "void")
        if result_type == "object":
                cpp_ret, invoke_ret = ("std::vector<int>", "entities") if name in ENTITY_LIST_NATIVES else ("cfx::Value", "object")
        elif result_type in RETURN_TYPE_MAP:
                cpp_ret, invoke_ret = RETURN_TYPE_MAP[result_type]
        else:
                return None
        func_name = screaming_to_pascal(name)
        cpp_params = []
        call_args = []
        prelude = []
        skip = False
        for p in params:
                ptype = p.get("type", "int")
                pname = to_safe_param(p.get("name", "p"))
                if ptype == "object":
                        cpp_params.append(f"const cfx::Value& {pname}")
                        prelude.append(f"auto {pname}Data = fxw_internal::encode({pname});")
                        call_args += [f"{pname}Data.data()", f"static_cast<int>({pname}Data.size())"]
                        continue
                if ptype not in PARAM_TYPE_MAP:
                        skip = True
                        break
                cpp_type = PARAM_TYPE_MAP[ptype]
                cpp_params.append(f"{cpp_type} {pname}")
                call_args.append(pname)
        if skip:
                return None
        param_str = ", ".join(cpp_params)
        args_str = ", ".join(call_args)
        comma_args = f", {args_str}" if args_str else ""
        if invoke_ret is None:
                body = f"invoke({hash_str}{comma_args});"
        elif invoke_ret == "object":
                body = f"return invokeObject({hash_str}{comma_args});"
        elif invoke_ret == "entities":
                body = f"return invokeEntities({hash_str}{comma_args});"
        else:
                body = f"return invoke<{invoke_ret}>({hash_str}{comma_args});"
        body = "".join(f"{line}\n                " for line in prelude) + body

        wrapper = f"        inline {cpp_ret} {func_name}({param_str})\n        {{\n                {body}\n        }}"
        if not any(p.get("type") == "char*" and p.get("name") == "playerSrc" for p in params):
                return wrapper
        int_params = ", ".join("int playerSrc" if param == "const char* playerSrc" else param for param in cpp_params)
        forward_args = ", ".join("std::to_string(playerSrc).c_str()" if arg == "playerSrc" else arg for arg in call_args)
        ret = "" if invoke_ret is None else "return "
        return f"{wrapper}\n\n        inline {cpp_ret} {func_name}({int_params})\n        {{\n                {ret}{func_name}({forward_args});\n        }}"


def generate_cpp_wrappers(natives: list[dict]) -> list[tuple[str, str]]:
        seen: dict[tuple, dict] = {}
        for n in natives:
                name = n.get("name")
                if not name:
                        continue
                cpp_types = tuple(PARAM_TYPE_MAP.get(p.get("type", "int"), p.get("type", "")) for p in n.get("params", []))
                key = (screaming_to_pascal(name), cpp_types)
                if key not in seen:
                        seen[key] = n
        wrappers = []
        for n in sorted(seen.values(), key=lambda x: x.get("name", "")):
                w = generate_wrapper(n)
                if w:
                        wrappers.append((screaming_to_pascal(n["name"]), w))
        return wrappers


def fetch_json(url: str, no_cache: bool = False):
        cache_file = os.path.join(CACHE_DIR, hashlib.sha1(url.encode()).hexdigest() + ".json")
        if not no_cache and os.path.exists(cache_file):
                age = time.time() - os.path.getmtime(cache_file)
                if age < CACHE_TTL:
                        print(f"Using cached {url} ({int(age)}s old)")
                        with open(cache_file) as f:
                                return json.load(f)
        print(f"Fetching {url}...")
        req = urllib.request.Request(url, headers={"User-Agent": "citizen-scripting-wasm-nativedb"})
        with urllib.request.urlopen(req, timeout=30) as resp:
                data = resp.read()
        os.makedirs(CACHE_DIR, exist_ok=True)
        with open(cache_file, "wb") as f:
                f.write(data)
        return json.loads(data)


def parse_cfx_natives(data: list | dict) -> dict[str, list[dict]]:
        by_ns = defaultdict(list)
        if isinstance(data, list):
                for entry in data:
                        ns = entry.get("ns", "CFX")
                        by_ns[ns].append(entry)
        elif isinstance(data, dict):
                for key, entry in data.items():
                        if isinstance(entry, dict) and "name" in entry:
                                ns = entry.get("ns", "CFX")
                                by_ns[ns].append(entry)
                        elif isinstance(entry, dict):
                                for hash_key, native in entry.items():
                                        if isinstance(native, dict) and "name" in native:
                                                ns = native.get("ns", key)
                                                by_ns[ns].append(native)
        return dict(by_ns)


def normalize_native(native: dict, hash_key: str) -> dict:
        if "hash" not in native:
                native["hash"] = hash_key
        if "results" not in native and "return_type" in native:
                native["results"] = native["return_type"]
        return native


def parse_game_natives(data: dict) -> dict[str, list[dict]]:
        by_ns = defaultdict(list)
        for ns, natives in data.items():
                if not isinstance(natives, dict):
                        continue
                for hash_key, native in natives.items():
                        if isinstance(native, dict) and "name" in native:
                                by_ns[ns].append(normalize_native(native, hash_key))
        return dict(by_ns)


def screaming_to_snake(name: str) -> str:
        return name.lower()


def to_safe_rust_param(name: str) -> str:
        return name + "_" if name in RUST_KEYWORDS else name


def generate_rust_wrapper(native: dict) -> str | None:
        name = native.get("name")
        if not name or name.startswith("0x") or name.startswith("_0x"):
                return None
        hash_str = native.get("hash", "0x0")
        params = native.get("params", [])
        result_type = native.get("results", "void")
        if result_type == "object":
                rust_ret, invoke_fn = ("Vec<i32>", "invoke_native_entities") if name in ENTITY_LIST_NATIVES else ("crate::Value", "invoke_native_object")
        elif result_type in RUST_RETURN_TYPE_MAP:
                rust_ret, invoke_fn = RUST_RETURN_TYPE_MAP[result_type]
        else:
                return None
        func_name = screaming_to_snake(name)

        rust_params = []
        cstring_decls = []
        arg_exprs = []
        skip = False
        str_idx = 0
        obj_idx = 0

        for p in params:
                ptype = p.get("type", "int")
                pname = to_safe_rust_param(p.get("name", "p").lower())
                if ptype == "object":
                        var = f"_o{obj_idx}"
                        obj_idx += 1
                        rust_params.append(f"{pname}: impl Into<crate::Value>")
                        cstring_decls.append(f"                let {var} = crate::encode(&{pname}.into());")
                        arg_exprs += [f"crate::NativeArg::ptr({var}.as_ptr() as u64)", f"crate::NativeArg::val({var}.len() as u64)"]
                        continue
                if ptype not in RUST_PARAM_TYPE_MAP:
                        skip = True
                        break
                rtype, kind, expr_template = RUST_PARAM_TYPE_MAP[ptype]
                player = ptype == "char*" and p.get("name") == "playerSrc"
                rust_params.append(f"{pname}: impl ::std::fmt::Display" if player else f"{pname}: {rtype}")
                if kind == "str":
                        var = f"_s{str_idx}"
                        str_idx += 1
                        value = f"{pname}.to_string()" if player else pname
                        cstring_decls.append(f"                let {var} = ::std::ffi::CString::new({value}).unwrap_or_default();")
                        arg_exprs.append(f"crate::NativeArg::ptr({var}.as_ptr() as u64)")
                elif kind == "ptr":
                        arg_exprs.append(f"crate::NativeArg::ptr({expr_template.format(p=pname)})")
                else:
                        arg_exprs.append(f"crate::NativeArg::val({expr_template.format(p=pname)})")
        if skip:
                return None

        param_str = ", ".join(rust_params)
        args_str = ", ".join(arg_exprs)
        cstring_block = "\n".join(cstring_decls)
        if cstring_block:
                cstring_block += "\n"

        if rust_ret is None:
                sig = f"pub fn {func_name}({param_str})"
                body = f"crate::{invoke_fn}({hash_str}, &[{args_str}]);"
        else:
                sig = f"pub fn {func_name}({param_str}) -> {rust_ret}"
                body = f"crate::{invoke_fn}({hash_str}, &[{args_str}])"

        return f"        {sig} {{\n{cstring_block}                {body}\n        }}"


def generate_rust_wrappers(natives: list[dict]) -> list[tuple[str, str]]:
        by_name: dict[str, dict] = {}
        for n in natives:
                name = n.get("name")
                if not name:
                        continue
                prev = by_name.get(name)
                if prev is None or len(n.get("params", [])) > len(prev.get("params", [])):
                        by_name[name] = n
        wrappers = []
        for n in sorted(by_name.values(), key=lambda x: x.get("name", "")):
                w = generate_rust_wrapper(n)
                if w:
                        wrappers.append((screaming_to_snake(n["name"]), w))
        return wrappers


SCALAR_KINDS = {
        "int": "i", "float": "i", "BOOL": "i", "bool": "i", "Hash": "i", "Entity": "i", "Player": "i",
        "Vehicle": "i", "Ped": "i", "Object": "i", "Pickup": "i", "Blip": "i", "Cam": "i", "FireId": "i",
        "Interior": "i", "ScrHandle": "i", "Any": "i", "long": "l", "Vector3": "iii",
}


def native_hash(native: dict) -> int | None:
        try:
                return int(native.get("hash", ""), 16)
        except ValueError:
                return None


def runs_on_server(native: dict, cfx_games: set[str]) -> bool:
        return native.get("apiset") in SERVER_APISETS and native.get("game") in (None, *cfx_games)


def native_type_kinds(native: dict) -> tuple[str, str] | None:
        params = native.get("params", [])
        args = ""
        for i, p in enumerate(params):
                ptype = p.get("type", "")
                if ptype in SCALAR_KINDS:
                        args += SCALAR_KINDS[ptype]
                elif ptype == "char*":
                        nxt = params[i + 1] if i + 1 < len(params) else None
                        sized = nxt is not None and nxt.get("type") in ("int", "long") and nxt.get("name", "").lower().endswith("length")
                        args += "b" if sized else "s"
                elif ptype == "func":
                        args += "s"
                elif ptype == "object":
                        args += "bi"
                elif ptype == "Vector3*":
                        args += "v"
                elif ptype.endswith("*") and ptype.rstrip("*") in SCALAR_KINDS:
                        args += "p"
                else:
                        return None
        result = native.get("results", "void")
        if result == "void":
                results = ""
        elif result in SCALAR_KINDS:
                results = SCALAR_KINDS[result]
        elif result == "char*":
                results = "s"
        elif result == "object":
                results = "bi"
        elif result.endswith("*"):
                results = "p"
        else:
                return None
        if len(args) > 32 or len(results) > 32:
                return None
        return args, results


def generate_type_table(natives: list[dict]) -> tuple[str, int]:
        table: dict[int, tuple[str, str]] = {}
        names: dict[int, str] = {}
        for n in natives:
                try:
                        h = int(n.get("hash", ""), 16)
                except ValueError:
                        continue
                kinds = native_type_kinds(n)
                if kinds is None:
                        continue
                if h in table and table[h] != kinds:
                        print(f"Conflicting signatures for native 0x{h:X}, leaving it out of the type table")
                        table[h] = None
                        continue
                table.setdefault(h, kinds)
                names.setdefault(h, n.get("name", ""))
        rows = [f"        {{ 0x{h:016X}ULL, \"{names[h]}\", \"{a}\", \"{r}\" }}," for h, k in sorted(table.items()) if k is not None for a, r in [k]]
        out = "// Auto-generated, do not edit.\n#pragma once\n\nstatic constexpr NativeTypeInfo g_nativeTypes[] = {\n"
        out += "\n".join(rows)
        out += "\n};\n"
        return out, len(rows)


def embed_sdk(base_dir, output):
        files = {}
        for rel_path in ("include/cfx.h", "include/cfx.rs", "src/natives/all.h", "src/natives/all.rs"):
                with open(os.path.join(base_dir, rel_path), "rb") as f:
                        files[rel_path] = f.read()
        blob = struct.pack("<I", len(files))
        for rel_path, data in files.items():
                path_bytes = rel_path.encode("utf-8")
                blob += struct.pack("<I", len(path_bytes)) + path_bytes
                blob += struct.pack("<I", len(data)) + data
        with open(output, "wb") as f:
                f.write(blob)
        print(f"Embedded {len(files)} SDK files ({len(blob)} bytes) into {output}")


def main():
        import argparse
        parser = argparse.ArgumentParser()
        parser.add_argument("--game", choices=list(GAME_URLS.keys()) + ["all"], nargs="*", default=["fivem"])
        parser.add_argument("--output", default="src/natives")
        parser.add_argument("--embed", default="src/sdk.blob")
        parser.add_argument("--no-cache", action="store_true")
        parser.add_argument("--embed-only", action="store_true")
        args = parser.parse_args()
        base_dir = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
        if args.embed_only:
                embed_sdk(base_dir, args.embed)
                return
        output_dir = args.output
        games = list(GAME_URLS.keys()) if "all" in args.game else args.game
        no_cache = args.no_cache
        os.makedirs(output_dir, exist_ok=True)
        all_ns: dict[str, list[dict]] = {}
        cfx_natives: list[dict] = []
        try:
                cfx_data = fetch_json(CFX_URL, no_cache)
                for ns, natives in parse_cfx_natives(cfx_data).items():
                        all_ns.setdefault(ns, []).extend(natives)
                        cfx_natives.extend(natives)
        except Exception as e:
                print(f"Failed to fetch CFX natives: {e}")
        if not cfx_natives:
                print("The native type table needs the CFX natives")
                sys.exit(1)
        for game in games:
                try:
                        game_data = fetch_json(GAME_URLS[game], no_cache)
                        for ns, natives in parse_game_natives(game_data).items():
                                all_ns.setdefault(ns, []).extend(natives)
                except Exception as e:
                        print(f"Failed to fetch {game} natives: {e}")
        if not all_ns:
                print("No natives fetched")
                sys.exit(1)
        def param_sig(native):
                return tuple(p.get("type", "") for p in native.get("params", []))
        cfx_games = {CFX_GAME_NAMES[g] for g in games}
        server_natives = [n for n in cfx_natives if runs_on_server(n, cfx_games)]
        server_hashes = {native_hash(n) for n in server_natives}
        cfx_ids = {id(n) for n in cfx_natives}
        cfx_names = {n.get("name") for n in cfx_natives}
        global_seen: dict[tuple, tuple[str, tuple[bool, int]]] = {}
        for ns in all_ns:
                for native in all_ns[ns]:
                        name = native.get("name")
                        if not name:
                                continue
                        key = (name, param_sig(native))
                        rank = (id(native) in cfx_ids, len(native.get("hash", "")))
                        prev = global_seen.get(key)
                        if prev is None or rank > prev[1]:
                                global_seen[key] = (ns, rank)
        for ns in all_ns:
                all_ns[ns] = [n for n in all_ns[ns]
                                if not n.get("name") or (global_seen.get((n["name"], param_sig(n)), (None,))[0] == ns
                                        and (id(n) in cfx_ids or n["name"] not in cfx_names))]
        cpp_owner: dict[str, str] = {}
        cpp_wrappers: list[tuple[str, str]] = []
        rust_owner: dict[str, str] = {}
        rust_wrappers: list[tuple[str, str]] = []
        for ns in sorted(all_ns.keys(), key=lambda n: (n.lower() != "cfx", n)):
                natives = [n for n in all_ns[ns] if native_hash(n) in server_hashes]
                for name, wrapper in generate_cpp_wrappers(natives):
                        if cpp_owner.setdefault(name, ns) == ns:
                                cpp_wrappers.append((name, wrapper))
                for name, wrapper in generate_rust_wrappers(natives):
                        if rust_owner.setdefault(name, ns) == ns:
                                rust_wrappers.append((name, wrapper))

        for f in os.listdir(output_dir):
                if f.endswith(".h") and f not in ("all.h", "types.h"):
                        os.remove(os.path.join(output_dir, f))

        all_h = "// Auto-generated, do not edit.\n#pragma once\n\nnamespace cfx::natives\n{\n\n"
        all_h += "\n\n".join(w for _, w in sorted(cpp_wrappers, key=lambda x: x[0]))
        all_h += "\n\n} // namespace cfx::natives\n"
        with open(os.path.join(output_dir, "all.h"), "w") as f:
                f.write(all_h)

        all_rs = "// Auto-generated, do not edit.\n\n"
        all_rs += "\n\n".join(w for _, w in sorted(rust_wrappers, key=lambda x: x[0]))
        with open(os.path.join(output_dir, "all.rs"), "w") as f:
                f.write(all_rs + "\n")

        types_h, types_count = generate_type_table(server_natives)
        with open(os.path.join(output_dir, "types.h"), "w") as f:
                f.write(types_h)

        print(f"\nGenerated {len(cpp_wrappers)} C++ wrappers, {len(rust_wrappers)} Rust wrappers, {types_count} native types")
        print(f"Output: {output_dir}/")

        embed_sdk(base_dir, args.embed)


if __name__ == "__main__":
        main()
