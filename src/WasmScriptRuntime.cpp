#include "WasmScriptRuntime.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <exception>
#include <string>
#include <string_view>
#include <climits>
#include <unordered_set>
#include <vector>

#include <atomic>
#include <cstdarg>
#include <cstdlib>
#include <dirent.h>
#include <dlfcn.h>
#include <ftw.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>

using namespace fx::cpp;

static std::atomic<uint32_t> s_nextInstanceId{ 1 };
static std::mutex s_instanceIdMutex;
static std::unordered_set<int32_t> s_activeInstanceIds;

static constexpr int64_t WASM_MEMORY_LIMIT = 256 * 1024 * 1024;
static constexpr uint64_t WASM_FUEL_AMOUNT = 1000000000ULL;
static constexpr uint32_t RESULT_BUF_SIZE = 65536;
static constexpr int EPOCH_TICK_MS = 10;
static constexpr uint64_t WASM_CALL_TIMEOUT_TICKS = 5000 / EPOCH_TICK_MS;
static constexpr int WORKER_STOP_TIMEOUT_MS = 1000;

static std::string GetResourcePath(IScriptHost* host, const std::string& name)
{
        if (name.empty())
                return { };
        fxNativeContext ctx{ };
        ctx.nativeIdentifier = HashString("GET_RESOURCE_PATH");
        ctx.arguments[0] = reinterpret_cast<uintptr_t>(name.c_str());
        ctx.numArguments = 1;
        ctx.numResults = 1;
        host->InvokeNative(ctx);
        const char* path = reinterpret_cast<const char*>(ctx.arguments[0]);
        return path ? std::string(path) : std::string{ };
}

static std::string GetConvar(IScriptHost* host, const char* name, const char* defaultValue)
{
        fxNativeContext ctx{ };
        ctx.nativeIdentifier = HashString("GET_CONVAR");
        ctx.arguments[0] = reinterpret_cast<uintptr_t>(name);
        ctx.arguments[1] = reinterpret_cast<uintptr_t>(defaultValue);
        ctx.numArguments = 2;
        ctx.numResults = 1;
        if (FX_FAILED(host->InvokeNative(ctx)))
                return defaultValue;
        const char* result = reinterpret_cast<const char*>(ctx.arguments[0]);
        return result ? std::string(result) : std::string(defaultValue);
}

using CoreTraceFn = void (*)(const char* channel, const char* func, const char* file, int line, const char* string);

static CoreTraceFn GetCoreTrace()
{
        static CoreTraceFn s_fn = reinterpret_cast<CoreTraceFn>(dlsym(RTLD_DEFAULT, "CoreTrace"));
        return s_fn;
}

static void ConsolePrint(const std::string& channel, std::string text)
{
        if (text.empty() || text.back() != '\n')
                text.push_back('\n');
        if (auto* coreTrace = GetCoreTrace())
        {
                coreTrace(channel.c_str(), "", "", 0, text.c_str());
                return;
        }
        flockfile(stderr);
        fprintf(stderr, "[%s] %s", channel.c_str(), text.c_str());
        funlockfile(stderr);
}

static void ScriptPrint(IScriptHost* host, const std::string& resourceName, std::string msg)
{
        if (host)
                host->ScriptTrace(msg.data());
        ConsolePrint("script:" + resourceName, std::move(msg));
}

enum class LogLevel { Info, Warning, Error };

__attribute__((format(printf, 2, 3)))
static void Log(LogLevel level, const char* fmt, ...)
{
        va_list ap, aq;
        va_start(ap, fmt);
        va_copy(aq, ap);
        int len = vsnprintf(nullptr, 0, fmt, aq);
        va_end(aq);
        std::string msg(len > 0 ? static_cast<size_t>(len) : 0, '\0');
        if (len > 0)
                vsnprintf(msg.data(), msg.size() + 1, fmt, ap);
        va_end(ap);
        while (!msg.empty() && msg.back() == '\n')
                msg.pop_back();
        if (level == LogLevel::Error)
                msg = "^1" + msg + "^7";
        else if (level == LogLevel::Warning)
                msg = "^3" + msg + "^7";
        ConsolePrint("citizen-scripting-wasm", std::move(msg));
}

#define LogInfo(...) Log(LogLevel::Info, __VA_ARGS__)
#define LogError(...) Log(LogLevel::Error, __VA_ARGS__)
#define LogWarning(...) Log(LogLevel::Warning, __VA_ARGS__)

static inline char* Mut(std::string& s) { return s.data(); }

static std::string UTF8Truncate(const std::string& s, size_t maxBytes)
{
        if (s.size() <= maxBytes)
                return s;
        size_t cut = maxBytes;
        while (cut > 0)
        {
                unsigned char c = static_cast<unsigned char>(s[cut]);
                if ((c & 0xC0) != 0x80)
                        break;
                --cut;
        }
        return s.substr(0, cut) + "...";
}

static bool ValidateScriptPath(const char* scriptFile, const std::string& root, std::string& resolvedPath, std::string& resolvedRoot)
{
        std::string fullPath = root + "/" + scriptFile;
        char* resolvedFull = realpath(fullPath.c_str(), nullptr);
        if (!resolvedFull)
        {
                LogError("realpath failed for '%s'", fullPath.c_str());
                return false;
        }
        resolvedPath = resolvedFull;
        free(resolvedFull);
        char* resolvedRootBuf = realpath(root.c_str(), nullptr);
        if (!resolvedRootBuf)
        {
                LogError("realpath failed for root '%s'", root.c_str());
                return false;
        }
        resolvedRoot = resolvedRootBuf;
        free(resolvedRootBuf);
        if (!resolvedPath.starts_with(resolvedRoot) || (resolvedPath.size() > resolvedRoot.size() && resolvedPath[resolvedRoot.size()] != '/'))
        {
                LogError("Script path '%s' resolves outside resource root", scriptFile);
                return false;
        }
        return true;
}

static wasmtime_val_t I32Val(int32_t v)
{
        wasmtime_val_t r{ };
        r.kind = WASMTIME_I32;
        r.of.i32 = v;
        return r;
}

static constexpr size_t MAX_FUNC_TYPE_PARAMS = 16;

static wasm_functype_t* MakeFuncType(std::initializer_list<wasm_valkind_t> params, std::initializer_list<wasm_valkind_t> results)
{
        if (params.size() > MAX_FUNC_TYPE_PARAMS || results.size() > MAX_FUNC_TYPE_PARAMS)
                abort();
        wasm_valtype_t* pv[MAX_FUNC_TYPE_PARAMS];
        wasm_valtype_t* rv[MAX_FUNC_TYPE_PARAMS];
        size_t i = 0;
        for (auto k : params)
                pv[i++] = wasm_valtype_new(k);
        i = 0;
        for (auto k : results)
                rv[i++] = wasm_valtype_new(k);
        wasm_valtype_vec_t p_vec{ }, r_vec{ };
        wasm_valtype_vec_new(&p_vec, params.size(), pv);
        wasm_valtype_vec_new(&r_vec, results.size(), rv);
        return wasm_functype_new(&p_vec, &r_vec);
}

static bool CallerMemory(wasmtime_caller_t* caller, wasmtime_context_t* ctx, uint8_t** base, size_t* sz)
{
        wasmtime_extern_t ext{ };
        if (!wasmtime_caller_export_get(caller, "memory", 6, &ext))
                return false;
        if (ext.kind != WASMTIME_EXTERN_MEMORY)
                return false;
        *base = wasmtime_memory_data(ctx, &ext.of.memory);
        *sz = wasmtime_memory_data_size(ctx, &ext.of.memory);
        return *base != nullptr;
}

static bool InBounds(size_t memSz, uint32_t offset, size_t len)
{
        return static_cast<size_t>(offset) <= memSz && len <= memSz - static_cast<size_t>(offset);
}

static inline uint32_t ArgU32(const wasmtime_val_t& v) { return static_cast<uint32_t>(v.of.i32); }

struct WasmMem
{
        uint8_t* base;
        size_t sz;

        bool init(wasmtime_caller_t* caller)
        {
                auto* ctx = wasmtime_caller_context(caller);
                return CallerMemory(caller, ctx, &base, &sz);
        }

        bool check(uint32_t offset, size_t len) const { return InBounds(sz, offset, len); }

        std::string readStr(uint32_t ptr, uint32_t len) const
        {
                return std::string(reinterpret_cast<const char*>(base + ptr), len);
        }

        int32_t writeStr(uint32_t bufPtr, int32_t bufMax, std::string_view data)
        {
                if (bufMax > 0)
                {
                        size_t copy = std::min<size_t>(data.size(), static_cast<size_t>(bufMax) - 1);
                        if (!check(bufPtr, copy + 1))
                                return 0;
                        if (copy > 0)
                                memcpy(base + bufPtr, data.data(), copy);
                        base[bufPtr + copy] = '\0';
                }
                return static_cast<int32_t>(std::min<size_t>(data.size(), static_cast<size_t>(INT32_MAX)));
        }
};

static std::string ExtractWasmError(wasmtime_error_t* err, wasm_trap_t* trap)
{
        wasm_name_t msg{ };
        if (err)
        {
                wasmtime_error_message(err, &msg);
                wasmtime_error_delete(err);
        }
        else if (trap)
        {
                wasm_trap_message(trap, &msg);
                wasm_trap_delete(trap);
        }
        std::string out(msg.data, msg.size);
        wasm_byte_vec_delete(&msg);
        return out;
}

static bool WasmCall(wasmtime_store_t* store, const wasmtime_func_t& fn, const wasmtime_val_t* args, size_t nargs, wasmtime_val_t* results, size_t nresults, const char* resourceName, const char* label, bool quietLimits = false, bool* hitLimit = nullptr)
{
        wasm_trap_t* trap = nullptr;
        auto* err = wasmtime_func_call(wasmtime_store_context(store), &fn, args, nargs, results, nresults, &trap);
        if (err || trap)
        {
                wasmtime_trap_code_t code = 0;
                bool hasCode = trap && wasmtime_trap_code(trap, &code);
                bool limit = hasCode && (code == WASMTIME_TRAP_CODE_INTERRUPT || code == WASMTIME_TRAP_CODE_OUT_OF_FUEL);
                if (hitLimit)
                        *hitLimit = limit;
                auto msg = ExtractWasmError(err, trap);
                if (limit && quietLimits)
                        return false;
                if (hasCode && code == WASMTIME_TRAP_CODE_INTERRUPT)
                        LogError("[%s] %s: stopped after running for more than %llu ms\n%s", resourceName, label, static_cast<unsigned long long>(WASM_CALL_TIMEOUT_TICKS * EPOCH_TICK_MS), msg.c_str());
                else if (hasCode && code == WASMTIME_TRAP_CODE_OUT_OF_FUEL)
                        LogError("[%s] %s: stopped after using its whole instruction budget in one call\n%s", resourceName, label, msg.c_str());
                else
                        LogError("[%s] %s: %s", resourceName, label, msg.c_str());
                return false;
        }
        return true;
}

static bool IsFilteredE2(const std::string& msg, size_t i)
{
        if (i + 2 >= msg.size())
                return false;
        unsigned char c1 = static_cast<unsigned char>(msg[i + 1]);
        unsigned char c2 = static_cast<unsigned char>(msg[i + 2]);
        if (c1 == 0x80 && ((c2 >= 0x8B && c2 <= 0x8F) || (c2 == 0xA8 || c2 == 0xA9) || (c2 >= 0xAA && c2 <= 0xAE)))
                return true;
        if (c1 == 0x81 && ((c2 >= 0xA0 && c2 <= 0xA4) || (c2 >= 0xA6 && c2 <= 0xA9)))
                return true;
        return false;
}

static bool IsFilteredEF(const std::string& msg, size_t i)
{
        if (i + 2 >= msg.size())
                return false;
        return static_cast<unsigned char>(msg[i + 1]) == 0xBB && static_cast<unsigned char>(msg[i + 2]) == 0xBF;
}

static void SanitizeTraceMsg(std::string& msg)
{
        size_t scanStart = msg.size();
        for (size_t i = 0; i < msg.size(); ++i)
        {
                unsigned char c = static_cast<unsigned char>(msg[i]);
                if ((c < 0x20 && c != '\n' && c != '\t') || c == 0x7F)
                {
                        scanStart = i;
                        break;
                }
                if (c == 0xE2 && IsFilteredE2(msg, i))
                {
                        scanStart = i;
                        break;
                }
                if (c == 0xEF && IsFilteredEF(msg, i))
                {
                        scanStart = i;
                        break;
                }
        }
        if (scanStart == msg.size())
                return;
        size_t out = scanStart;
        for (size_t i = scanStart; i < msg.size(); ++i)
        {
                unsigned char c = static_cast<unsigned char>(msg[i]);
                if (c == '\n' || c == '\t' || (c >= 0x20 && c != 0x1B && c != 0x7F))
                {
                        if (c == 0xE2 && IsFilteredE2(msg, i)) { i += 2; continue; }
                        if (c == 0xEF && IsFilteredEF(msg, i)) { i += 2; continue; }
                        msg[out++] = msg[i];
                }
        }
        msg.resize(out);
}

static constexpr int32_t WASI_EBADF = 8;
static constexpr int32_t WASI_EFAULT = 21;
static constexpr size_t MAX_STDIO_LINE = 64 * 1024;

static int32_t WasiFdWrite(wasmtime_caller_t* caller, const wasmtime_val_t* args, std::string& buffer)
{
        int32_t fd = args[0].of.i32;
        if (fd != 1 && fd != 2)
                return WASI_EBADF;
        WasmMem mem;
        if (!mem.init(caller))
                return WASI_EFAULT;
        uint32_t iovs = ArgU32(args[1]), iovsLen = ArgU32(args[2]), nwrittenPtr = ArgU32(args[3]);
        if (iovsLen > 1024 || !mem.check(iovs, static_cast<size_t>(iovsLen) * 8) || !mem.check(nwrittenPtr, 4))
                return WASI_EFAULT;
        uint32_t total = 0;
        for (uint32_t i = 0; i < iovsLen; ++i)
        {
                uint32_t ptr, len;
                memcpy(&ptr, mem.base + iovs + i * 8, 4);
                memcpy(&len, mem.base + iovs + i * 8 + 4, 4);
                if (!mem.check(ptr, len))
                        return WASI_EFAULT;
                buffer.append(reinterpret_cast<const char*>(mem.base + ptr), len);
                total += len;
        }
        memcpy(mem.base + nwrittenPtr, &total, 4);
        return 0;
}

template<typename F>
static void EmitStdioLines(std::string& buffer, bool flushAll, F&& print)
{
        size_t start = 0;
        for (size_t nl = buffer.find('\n'); nl != std::string::npos; nl = buffer.find('\n', start))
        {
                std::string line = buffer.substr(start, nl - start);
                SanitizeTraceMsg(line);
                print(std::move(line));
                start = nl + 1;
        }
        buffer.erase(0, start);
        if (!buffer.empty() && (flushAll || buffer.size() >= MAX_STDIO_LINE))
        {
                std::string line = std::move(buffer);
                buffer.clear();
                SanitizeTraceMsg(line);
                print(std::move(line));
        }
}

static wasm_functype_t* FdWriteFuncType()
{
        static wasm_functype_t* s_type = MakeFuncType({ WASM_I32, WASM_I32, WASM_I32, WASM_I32 }, { WASM_I32 });
        return s_type;
}

static void FlushModuleStdio(WasmModule& mod, bool flushAll)
{
        EmitStdioLines(mod.stdioBuffer, flushAll, [&](std::string line)
        {
                ScriptPrint(mod.rt->host(), mod.rt->resourceName(), std::move(line));
        });
}

static wasm_trap_t* CbFdWrite(void* env, wasmtime_caller_t* caller, const wasmtime_val_t* args, size_t, wasmtime_val_t* results, size_t)
{
        auto* mod = static_cast<WasmModule*>(env);
        results[0] = I32Val(WasiFdWrite(caller, args, mod->stdioBuffer));
        FlushModuleStdio(*mod, false);
        return nullptr;
}

static void FlushWorkerStdio(WasmScriptRuntime::WorkerState& state, bool flushAll)
{
        EmitStdioLines(state.stdioBuffer, flushAll, [&](std::string line)
        {
                ConsolePrint("worker:" + state.resourceName, std::move(line));
        });
}

static wasm_trap_t* CbWorkerFdWrite(void* env, wasmtime_caller_t* caller, const wasmtime_val_t* args, size_t, wasmtime_val_t* results, size_t)
{
        auto* state = static_cast<WasmScriptRuntime::WorkerState*>(env);
        results[0] = I32Val(WasiFdWrite(caller, args, state->stdioBuffer));
        FlushWorkerStdio(*state, false);
        return nullptr;
}

static wasm_trap_t* CbTrace(void* env, wasmtime_caller_t* caller, const wasmtime_val_t* args, size_t, wasmtime_val_t*, size_t)
{
        auto* rt = static_cast<WasmModule*>(env)->rt;
        WasmMem mem;
        if (!mem.init(caller))
                return nullptr;
        uint32_t ptr = ArgU32(args[0]);
        uint32_t len = ArgU32(args[1]);
        static constexpr uint32_t MAX_TRACE_LEN = 1u << 20;
        if (len > MAX_TRACE_LEN)
                len = MAX_TRACE_LEN;
        if (!mem.check(ptr, len))
                return nullptr;
        std::string msg = mem.readStr(ptr, len);
        SanitizeTraceMsg(msg);
        if (!msg.empty())
                ScriptPrint(rt->host(), rt->resourceName(), std::move(msg));
        return nullptr;
}

struct NativeTypeInfo
{
        uint64_t hash;
        const char* args;
        const char* results;
};

#include "natives/types.h"

static const NativeTypeInfo* FindNativeType(uint64_t hash)
{
        auto it = std::lower_bound(std::begin(g_nativeTypes), std::end(g_nativeTypes), hash, [](const NativeTypeInfo& info, uint64_t h) { return info.hash < h; });
        return it != std::end(g_nativeTypes) && it->hash == hash ? it : nullptr;
}

static bool IsPointerKind(char kind)
{
        return kind == 's' || kind == 'b' || kind == 'p' || kind == 'v';
}

static std::string TranslateNativeArgs(const WasmMem& mem, std::string_view kinds, const WasmNativeCtx& wctx, fxNativeContext& hostCtx)
{
        for (size_t i = 0; i < kinds.size(); ++i)
        {
                uint64_t value = wctx.args[i];
                if (!IsPointerKind(kinds[i]))
                {
                        hostCtx.arguments[i] = kinds[i] == 'l' ? value : (value & 0xFFFFFFFFu);
                        continue;
                }
                uint32_t off = static_cast<uint32_t>(value);
                int64_t len = kinds[i] == 'b' && i + 1 < kinds.size() ? static_cast<int32_t>(wctx.args[i + 1]) : -1;
                bool valid;
                if (off == 0)
                        valid = kinds[i] == 's' || (kinds[i] == 'b' && len <= 0);
                else if (kinds[i] == 'p')
                        valid = mem.check(off, 8);
                else if (kinds[i] == 'v')
                        valid = mem.check(off, 24);
                else if (len >= 0)
                        valid = mem.check(off, static_cast<size_t>(len));
                else
                        valid = off < mem.sz && memchr(mem.base + off, 0, mem.sz - off) != nullptr;
                if (!valid)
                        return "argument " + std::to_string(i) + " does not point to valid wasm memory";
                hostCtx.arguments[i] = off == 0 ? 0 : reinterpret_cast<uintptr_t>(mem.base + off);
        }
        return { };
}

static wasm_trap_t* CbInvokeNative(void* env, wasmtime_caller_t* caller, const wasmtime_val_t* args, size_t, wasmtime_val_t*, size_t)
{
        auto* mod = static_cast<WasmModule*>(env);
        auto* rt = mod->rt;
        mod->hasValidNativeResult = false;
        WasmMem mem;
        if (!mem.init(caller))
                return nullptr;
        uint32_t ctxPtr = ArgU32(args[0]);
        if (!mem.check(ctxPtr, sizeof(WasmNativeCtx)))
                return nullptr;
        WasmNativeCtx wctx{ };
        memcpy(&wctx, mem.base + ctxPtr, sizeof(WasmNativeCtx));
        const NativeTypeInfo* info = FindNativeType(wctx.hash);
        std::string_view argKinds = info ? info->args : "";
        std::string_view resultKinds = info ? info->results : "";
        fxNativeContext hostCtx{ };
        hostCtx.nativeIdentifier = wctx.hash;
        hostCtx.numArguments = static_cast<int>(argKinds.size());
        hostCtx.numResults = static_cast<int>(resultKinds.size());
        if (!info)
                mod->lastNativeError = "native is not in the native type table";
        else if (!rt->host())
                mod->lastNativeError = "script host is gone";
        else
                mod->lastNativeError = TranslateNativeArgs(mem, argKinds, wctx, hostCtx);
        if (mod->lastNativeError.empty() && FX_FAILED(rt->host()->InvokeNative(hostCtx)))
        {
                char* errorText = nullptr;
                rt->host()->GetLastErrorText(&errorText);
                mod->lastNativeError = errorText && *errorText ? errorText : "unknown error";
        }
        if (!mod->lastNativeError.empty())
        {
                char hash[19];
                snprintf(hash, sizeof(hash), "0x%016llx", static_cast<unsigned long long>(wctx.hash));
                ScriptPrint(rt->host(), rt->resourceName(), "^1SCRIPT ERROR: @" + rt->resourceName() + "/" + mod->scriptFile + ": native " + hash + " failed: " + mod->lastNativeError + "^7");
                std::fill(std::begin(hostCtx.arguments), std::end(hostCtx.arguments), 0);
        }
        mod->lastNativeCtx = hostCtx;
        mod->lastResultKinds = resultKinds;
        mod->hasValidNativeResult = true;
        size_t numResults = std::max<size_t>(std::min<uint32_t>(wctx.numResults, 32), resultKinds.size());
        for (size_t i = 0; i < numResults; ++i)
        {
                uint64_t value = hostCtx.arguments[i];
                char kind = i < resultKinds.size() ? resultKinds[i] : '\0';
                if (IsPointerKind(kind))
                        wctx.args[i] = value != 0 ? 1 : 0;
                else if (kind == 'l')
                        wctx.args[i] = value;
                else if (kind == 'i')
                        wctx.args[i] = value & 0xFFFFFFFFu;
                else
                        wctx.args[i] = 0;
        }
        if (!mem.init(caller))
                return nullptr;
        if (!mem.check(ctxPtr, sizeof(WasmNativeCtx)))
                return nullptr;
        memcpy(mem.base + ctxPtr, &wctx, sizeof(WasmNativeCtx));
        return nullptr;
}

static wasm_trap_t* CbGetLastNativeError(void* env, wasmtime_caller_t* caller, const wasmtime_val_t* args, size_t, wasmtime_val_t* results, size_t)
{
        auto* mod = static_cast<WasmModule*>(env);
        WasmMem mem;
        if (!mem.init(caller))
        {
                results[0] = I32Val(0);
                return nullptr;
        }
        results[0] = I32Val(mem.writeStr(ArgU32(args[0]), args[1].of.i32, mod->lastNativeError));
        return nullptr;
}

static wasm_trap_t* CbCopyStringResult(void* env, wasmtime_caller_t* caller, const wasmtime_val_t* args, size_t, wasmtime_val_t* results, size_t)
{
        auto* mod = static_cast<WasmModule*>(env);
        if (!mod->hasValidNativeResult)
        {
                results[0] = I32Val(0);
                return nullptr;
        }
        WasmMem mem;
        if (!mem.init(caller))
        {
                results[0] = I32Val(0);
                return nullptr;
        }
        int32_t resultIdx = args[1].of.i32;
        uint32_t bufPtr = ArgU32(args[2]);
        int32_t bufMax = args[3].of.i32;
        if (resultIdx < 0 || static_cast<size_t>(resultIdx) >= mod->lastResultKinds.size() || mod->lastResultKinds[resultIdx] != 's')
        {
                results[0] = I32Val(0);
                return nullptr;
        }
        const char* str = reinterpret_cast<const char*>(mod->lastNativeCtx.arguments[resultIdx]);
        if (!str)
        {
                results[0] = I32Val(0);
                return nullptr;
        }
        static constexpr size_t MAX_STRING_RESULT = 1u << 20;
        size_t len = strnlen(str, MAX_STRING_RESULT);
        results[0] = I32Val(mem.writeStr(bufPtr, bufMax, std::string_view(str, len)));
        return nullptr;
}

static wasm_trap_t* CbCopyBinaryResult(void* env, wasmtime_caller_t* caller, const wasmtime_val_t* args, size_t, wasmtime_val_t* results, size_t)
{
        auto* mod = static_cast<WasmModule*>(env);
        if (!mod->hasValidNativeResult)
        {
                results[0] = I32Val(0);
                return nullptr;
        }
        WasmMem mem;
        if (!mem.init(caller))
        {
                results[0] = I32Val(0);
                return nullptr;
        }
        int32_t ptrIdx = args[1].of.i32;
        int32_t sizeIdx = args[2].of.i32;
        uint32_t bufPtr = ArgU32(args[3]);
        int32_t bufMax = args[4].of.i32;
        if (ptrIdx < 0 || static_cast<size_t>(ptrIdx) + 1 >= mod->lastResultKinds.size() || sizeIdx != ptrIdx + 1 || mod->lastResultKinds[ptrIdx] != 'b')
        {
                results[0] = I32Val(0);
                return nullptr;
        }
        const void* data = reinterpret_cast<const void*>(mod->lastNativeCtx.arguments[ptrIdx]);
        if (!data)
        {
                results[0] = I32Val(0);
                return nullptr;
        }
        static constexpr size_t MAX_BINARY_RESULT = 1u << 24;
        size_t dataSize = static_cast<size_t>(mod->lastNativeCtx.arguments[sizeIdx]);
        if (dataSize > MAX_BINARY_RESULT)
        {
                LogWarning("Dropping a %zu byte native result in '%s', the limit is %zu bytes", dataSize, mod->rt->resourceName().c_str(), MAX_BINARY_RESULT);
                results[0] = I32Val(0);
                return nullptr;
        }
        size_t copy = (bufMax > 0) ? std::min<size_t>(dataSize, static_cast<size_t>(bufMax)) : 0;
        if (copy)
        {
                if (!mem.check(bufPtr, copy))
                {
                        results[0] = I32Val(0);
                        return nullptr;
                }
                memcpy(mem.base + bufPtr, data, copy);
        }
        results[0] = I32Val(static_cast<int32_t>(dataSize));
        return nullptr;
}

static wasm_trap_t* CbEmitEvent(void* env, wasmtime_caller_t* caller, const wasmtime_val_t* args, size_t, wasmtime_val_t*, size_t)
{
        auto* rt = static_cast<WasmModule*>(env)->rt;
        WasmMem mem;
        if (!mem.init(caller))
                return nullptr;
        uint32_t namePtr = ArgU32(args[0]), nameLen = ArgU32(args[1]);
        uint32_t argsPtr = ArgU32(args[2]), argsLen = ArgU32(args[3]);
        if (!mem.check(namePtr, nameLen) || !mem.check(argsPtr, argsLen))
                return nullptr;
        std::string evName = mem.readStr(namePtr, nameLen);
        std::vector<uint8_t> argsCopy(mem.base + argsPtr, mem.base + argsPtr + argsLen);
        if (!rt->host())
                return nullptr;
        fxNativeContext nctx{ };
        nctx.nativeIdentifier = HashString("TRIGGER_EVENT_INTERNAL");
        nctx.arguments[0] = reinterpret_cast<uintptr_t>(evName.c_str());
        nctx.arguments[1] = reinterpret_cast<uintptr_t>(argsCopy.data());
        nctx.arguments[2] = static_cast<uintptr_t>(argsLen);
        nctx.arguments[3] = reinterpret_cast<uintptr_t>("-1");
        nctx.numArguments = 4;
        nctx.numResults = 0;
        rt->host()->InvokeNative(nctx);
        return nullptr;
}

static wasm_trap_t* CbEmitNetEvent(void* env, wasmtime_caller_t* caller, const wasmtime_val_t* args, size_t, wasmtime_val_t*, size_t)
{
        auto* rt = static_cast<WasmModule*>(env)->rt;
        WasmMem mem;
        if (!mem.init(caller))
                return nullptr;
        uint32_t namePtr = ArgU32(args[0]), nameLen = ArgU32(args[1]);
        int32_t target = args[2].of.i32;
        uint32_t argsPtr = ArgU32(args[3]), argsLen = ArgU32(args[4]);
        if (!mem.check(namePtr, nameLen) || !mem.check(argsPtr, argsLen))
                return nullptr;
        std::string evName = mem.readStr(namePtr, nameLen);
        std::vector<uint8_t> argsCopy(mem.base + argsPtr, mem.base + argsPtr + argsLen);
        std::string targetStr = std::to_string(target);
        if (!rt->host())
                return nullptr;
        fxNativeContext nctx{ };
        nctx.nativeIdentifier = HashString("TRIGGER_CLIENT_EVENT_INTERNAL");
        nctx.arguments[0] = reinterpret_cast<uintptr_t>(evName.c_str());
        nctx.arguments[1] = reinterpret_cast<uintptr_t>(targetStr.c_str());
        nctx.arguments[2] = reinterpret_cast<uintptr_t>(argsCopy.data());
        nctx.arguments[3] = static_cast<uintptr_t>(argsLen);
        nctx.numArguments = 4;
        nctx.numResults = 0;
        rt->host()->InvokeNative(nctx);
        return nullptr;
}

static wasm_trap_t* CbCancelEvent(void* env, wasmtime_caller_t*, const wasmtime_val_t*, size_t, wasmtime_val_t*, size_t)
{
        auto* rt = static_cast<WasmModule*>(env)->rt;
        rt->m_eventCanceled = true;
        if (rt->host())
        {
                fxNativeContext nctx{ };
                nctx.nativeIdentifier = HashString("CANCEL_EVENT");
                nctx.numArguments = 0;
                nctx.numResults = 0;
                rt->host()->InvokeNative(nctx);
        }
        return nullptr;
}

static wasm_trap_t* CbWasEventCanceled(void* env, wasmtime_caller_t*, const wasmtime_val_t*, size_t, wasmtime_val_t* results, size_t)
{
        results[0] = I32Val(static_cast<WasmModule*>(env)->rt->m_eventCanceled ? 1 : 0);
        return nullptr;
}

static wasm_trap_t* CbGetResourceMetadata(void* env, wasmtime_caller_t* caller, const wasmtime_val_t* args, size_t, wasmtime_val_t* results, size_t)
{
        auto* rt = static_cast<WasmModule*>(env)->rt;
        WasmMem mem;
        if (!mem.init(caller))
        {
                results[0] = I32Val(0);
                return nullptr;
        }
        uint32_t keyPtr = ArgU32(args[0]), keyLen = ArgU32(args[1]);
        int32_t index = args[2].of.i32;
        uint32_t bufPtr = ArgU32(args[3]);
        int32_t bufMax = args[4].of.i32;
        if (!mem.check(keyPtr, keyLen))
        {
                results[0] = I32Val(0);
                return nullptr;
        }
        std::string key = mem.readStr(keyPtr, keyLen);
        auto* md = rt->metadataHost();
        std::string value;
        if (md)
        {
                char* val = nullptr;
                if (FX_SUCCEEDED(md->GetResourceMetaData(Mut(key), index, &val)) && val)
                        value = val;
        }
        results[0] = I32Val(mem.writeStr(bufPtr, bufMax, value));
        return nullptr;
}

static wasm_trap_t* CbGetNumResourceMetadata(void* env, wasmtime_caller_t* caller, const wasmtime_val_t* args, size_t, wasmtime_val_t* results, size_t)
{
        auto* rt = static_cast<WasmModule*>(env)->rt;
        WasmMem mem;
        if (!mem.init(caller))
        {
                results[0] = I32Val(0);
                return nullptr;
        }
        uint32_t keyPtr = ArgU32(args[0]), keyLen = ArgU32(args[1]);
        if (!mem.check(keyPtr, keyLen))
        {
                results[0] = I32Val(0);
                return nullptr;
        }
        std::string key = mem.readStr(keyPtr, keyLen);
        auto* md = rt->metadataHost();
        int32_t count = 0;
        if (md)
                md->GetNumResourceMetaData(Mut(key), &count);
        results[0] = I32Val(count);
        return nullptr;
}

static wasm_trap_t* CbIsManifestVersionV2Between(void* env, wasmtime_caller_t* caller, const wasmtime_val_t* args, size_t, wasmtime_val_t* results, size_t)
{
        auto* rt = static_cast<WasmModule*>(env)->rt;
        WasmMem mem;
        if (!mem.init(caller))
        {
                results[0] = I32Val(0);
                return nullptr;
        }
        uint32_t lowerPtr = ArgU32(args[0]), lowerLen = ArgU32(args[1]);
        uint32_t upperPtr = ArgU32(args[2]), upperLen = ArgU32(args[3]);
        if (!mem.check(lowerPtr, lowerLen) || !mem.check(upperPtr, upperLen))
        {
                results[0] = I32Val(0);
                return nullptr;
        }
        std::string lower = mem.readStr(lowerPtr, lowerLen);
        std::string upper = mem.readStr(upperPtr, upperLen);
        auto* mh = rt->manifestHost();
        bool result = false;
        if (mh)
                mh->IsManifestVersionV2Between(Mut(lower), Mut(upper), &result);
        results[0] = I32Val(result ? 1 : 0);
        return nullptr;
}

static wasm_trap_t* CbCreateRef(void* env, wasmtime_caller_t*, const wasmtime_val_t* args, size_t, wasmtime_val_t* results, size_t)
{
        auto* mod = static_cast<WasmModule*>(env);
        auto* rt = mod->rt;
        uint32_t callbackId = ArgU32(args[0]);
        std::weak_ptr<WasmScriptRuntime::RefGuard> guardWeak = rt->m_refGuard;
        std::weak_ptr<WasmModule> modWeak = mod->weak_from_this();
        int32_t refIdx = rt->AddFuncRef([callbackId, guardWeak, modWeak](const char* argsSerialized, uint32_t argsSize) -> std::vector<char>
        {
                auto guard = guardWeak.lock();
                if (!guard)
                        throw std::runtime_error("Runtime destroyed, ref callback invalid");
                std::lock_guard<std::recursive_mutex> lk(guard->mu);
                if (!guard->alive)
                        throw std::runtime_error("Runtime destroyed, ref callback invalid");
                auto target = modWeak.lock();
                if (!target || !target->store)
                        throw std::runtime_error("Script unloaded, ref callback invalid");
                std::vector<char> result;
                if (!guard->rt->callInvokeRef(*target, callbackId, argsSerialized, argsSize, result))
                {
                        if (target->limitExceeded && target->callDepth > 0)
                                return std::vector<char>{ static_cast<char>(MSGPACK_EMPTY_ARRAY) };
                        throw std::runtime_error("WASM trap in ref callback");
                }
                return result;
        });
        if (refIdx >= 0)
                rt->m_refToCallbackId[refIdx] = { mod, static_cast<int32_t>(callbackId) };
        results[0] = I32Val(refIdx);
        return nullptr;
}

static wasm_trap_t* CbCanonicalizeRef(void* env, wasmtime_caller_t* caller, const wasmtime_val_t* args, size_t, wasmtime_val_t* results, size_t)
{
        auto* rt = static_cast<WasmModule*>(env)->rt;
        WasmMem mem;
        if (!mem.init(caller))
        {
                results[0] = I32Val(0);
                return nullptr;
        }
        int32_t refIdx = args[0].of.i32;
        uint32_t bufPtr = ArgU32(args[1]);
        int32_t bufMax = args[2].of.i32;
        if (!rt->host())
        {
                results[0] = I32Val(0);
                return nullptr;
        }
        char* refString = nullptr;
        rt->host()->CanonicalizeRef(refIdx, rt->GetInstanceId(), &refString);
        if (!refString)
        {
                results[0] = I32Val(0);
                return nullptr;
        }
        size_t len = strlen(refString);
        int32_t written = mem.writeStr(bufPtr, bufMax, std::string_view(refString, len));
        fwFree(refString);
        results[0] = I32Val(written);
        return nullptr;
}

static wasm_trap_t* CbRemoveRef(void* env, wasmtime_caller_t*, const wasmtime_val_t* args, size_t, wasmtime_val_t*, size_t)
{
        auto* rt = static_cast<WasmModule*>(env)->rt;
        int32_t refIdx = args[0].of.i32;
        rt->RemoveRef(refIdx);
        return nullptr;
}

static wasm_trap_t* CbInvokeFunctionReference(void* env, wasmtime_caller_t* caller, const wasmtime_val_t* args, size_t, wasmtime_val_t*, size_t)
{
        auto* mod = static_cast<WasmModule*>(env);
        auto* rt = mod->rt;
        WasmMem mem;
        if (!mem.init(caller))
                return nullptr;
        uint32_t refPtr = ArgU32(args[0]), refLen = ArgU32(args[1]);
        uint32_t argsPtr = ArgU32(args[2]), argsLen = ArgU32(args[3]);
        uint32_t outPtr = ArgU32(args[4]);
        if (!mem.check(refPtr, refLen) || !mem.check(argsPtr, argsLen) || !mem.check(outPtr, 8))
                return nullptr;
        std::string refStr = mem.readStr(refPtr, refLen);
        std::vector<char> argsCopy(reinterpret_cast<char*>(mem.base + argsPtr), reinterpret_cast<char*>(mem.base + argsPtr) + argsLen);
        if (!rt->host())
                return nullptr;
        fx::OMPtr<IScriptBuffer> retBuf;
        rt->host()->InvokeFunctionReference(Mut(refStr), argsCopy.data(), argsLen, retBuf.ReleaseAndGetAddressOf());
        uint32_t dataPtr = 0, dataLen = 0;
        if (retBuf.GetRef() && retBuf->GetLength() > 0)
        {
                dataLen = static_cast<uint32_t>(retBuf->GetLength());
                dataPtr = mod->wasmAlloc(dataLen);
                if (!dataPtr)
                        dataLen = 0;
        }
        if (!mem.init(caller))
        {
                if (dataPtr)
                        mod->wasmFree(dataPtr, dataLen);
                return nullptr;
        }
        if (dataPtr && dataLen && mem.check(dataPtr, dataLen))
                memcpy(mem.base + dataPtr, retBuf->GetBytes(), dataLen);
        else if (dataPtr)
        {
                mod->wasmFree(dataPtr, dataLen);
                if (!mem.init(caller))
                        return nullptr;
                dataPtr = 0;
                dataLen = 0;
        }
        if (mem.check(outPtr, 8))
        {
                memcpy(mem.base + outPtr, &dataPtr, 4);
                memcpy(mem.base + outPtr + 4, &dataLen, 4);
        }
        else if (dataPtr)
        {
                mod->wasmFree(dataPtr, dataLen);
        }
        return nullptr;
}

static wasm_trap_t* CbGetInstanceId(void* env, wasmtime_caller_t*, const wasmtime_val_t*, size_t, wasmtime_val_t* results, size_t)
{
        results[0] = I32Val(static_cast<WasmModule*>(env)->rt->GetInstanceId());
        return nullptr;
}

static bool HasWasmPermission(IScriptHost* host, const char* convarName, const std::string& resourceName)
{
        struct CacheEntry
        {
                std::string raw;
                std::unordered_set<std::string> allowed;
                bool wildcard = false;
        };
        static std::mutex s_mu;
        static std::unordered_map<std::string, CacheEntry> s_cache;
        std::string current = GetConvar(host, convarName, "");
        std::lock_guard<std::mutex> lk(s_mu);
        auto& entry = s_cache[convarName];
        if (entry.raw != current)
        {
                entry.raw = current;
                entry.allowed.clear();
                entry.wildcard = false;
                size_t pos = 0;
                while (pos < current.size())
                {
                        size_t end = current.find(',', pos);
                        if (end == std::string::npos)
                                end = current.size();
                        size_t start = pos;
                        while (start < end && current[start] == ' ')
                                ++start;
                        size_t last = end;
                        while (last > start && current[last - 1] == ' ')
                                --last;
                        if (last > start)
                        {
                                std::string token(current.data() + start, last - start);
                                if (token == "*")
                                        entry.wildcard = true;
                                else
                                        entry.allowed.insert(std::move(token));
                        }
                        pos = end + 1;
                }
        }
        return entry.wildcard || entry.allowed.count(resourceName) > 0;
}

static wasm_trap_t* CbSpawnProcess(void* env, wasmtime_caller_t* caller, const wasmtime_val_t* args, size_t, wasmtime_val_t* results, size_t)
{
        auto* mod = static_cast<WasmModule*>(env);
        auto* rt = mod->rt;
        WasmMem mem;
        if (!mem.init(caller))
        {
                results[0] = I32Val(-2);
                return nullptr;
        }
        uint32_t cmdPtr = ArgU32(args[0]), cmdLen = ArgU32(args[1]);
        if (!mem.check(cmdPtr, cmdLen))
        {
                results[0] = I32Val(-2);
                return nullptr;
        }
        if (!rt->host() || !HasWasmPermission(rt->host(), "sv_wasmChildProcess", rt->resourceName()))
        {
                LogError("Resource '%s' denied child_process permission. Add 'set sv_wasmChildProcess \"%s\"' to your server.cfg", rt->resourceName().c_str(), rt->resourceName().c_str());
                results[0] = I32Val(-1);
                return nullptr;
        }
        if (rt->m_processes.size() >= static_cast<size_t>(WasmScriptRuntime::MAX_PROCESSES_PER_RESOURCE))
        {
                LogError("Resource '%s' exceeded max child process limit (%d)", rt->resourceName().c_str(), WasmScriptRuntime::MAX_PROCESSES_PER_RESOURCE);
                results[0] = I32Val(-2);
                return nullptr;
        }
        int32_t processId = fx::allocateId(rt->m_nextProcessId, rt->m_processes);
        if (processId < 0)
        {
                results[0] = I32Val(-2);
                return nullptr;
        }
        std::string cmd = mem.readStr(cmdPtr, cmdLen);
        std::string displayCmd = UTF8Truncate(cmd, 256);
        LogWarning("Resource '%s' spawning child process: %s", rt->resourceName().c_str(), displayCmd.c_str());
        auto state = std::make_shared<WasmScriptRuntime::ProcessState>();
        state->owner = mod;
        try
        {
                std::thread([state, cmd = std::move(cmd)]
                {
                        fx::ProcessResult pr = fx::spawnProcess(cmd, 1048576, 30000, &state->cancelled);
                        std::lock_guard<std::mutex> lk(state->mutex);
                        state->result = std::move(pr);
                        state->done = true;
                }).detach();
        }
        catch (...)
        {
                results[0] = I32Val(-2);
                return nullptr;
        }
        rt->m_processes[processId] = std::move(state);
        results[0] = I32Val(processId);
        return nullptr;
}

static wasm_trap_t* CbPollProcess(void* env, wasmtime_caller_t* caller, const wasmtime_val_t* args, size_t, wasmtime_val_t* results, size_t)
{
        auto* mod = static_cast<WasmModule*>(env);
        auto* rt = mod->rt;
        int32_t processId = args[0].of.i32;
        auto it = rt->m_processes.find(processId);
        if (it == rt->m_processes.end() || it->second->owner != mod)
        {
                results[0] = I32Val(-2);
                return nullptr;
        }
        auto state = it->second;
        std::lock_guard<std::mutex> lk(state->mutex);
        if (!state->done)
        {
                results[0] = I32Val(-4);
                return nullptr;
        }
        WasmMem mem;
        uint32_t outBuf = ArgU32(args[1]);
        int32_t outMax = args[2].of.i32;
        uint32_t infoPtr = ArgU32(args[3]);
        if (!mem.init(caller) || outMax < 0 || !mem.check(outBuf, static_cast<size_t>(outMax)) || !mem.check(infoPtr, 8))
        {
                results[0] = I32Val(-2);
                return nullptr;
        }
        const fx::ProcessResult& pr = state->result;
        if (pr.status >= 0 && pr.output.size() > static_cast<size_t>(outMax))
        {
                results[0] = I32Val(static_cast<int32_t>(pr.output.size()));
                return nullptr;
        }
        int32_t info[2] = { pr.exitCode, pr.truncated ? 1 : 0 };
        memcpy(mem.base + infoPtr, info, sizeof(info));
        int32_t status = pr.status;
        if (status >= 0)
        {
                if (!pr.output.empty())
                        memcpy(mem.base + outBuf, pr.output.data(), pr.output.size());
                status = static_cast<int32_t>(pr.output.size());
        }
        rt->m_processes.erase(it);
        results[0] = I32Val(status);
        return nullptr;
}

static wasm_trap_t* CbCreateWorker(void*, wasmtime_caller_t*, const wasmtime_val_t*, size_t, wasmtime_val_t*, size_t);
static wasm_trap_t* CbPollWorker(void*, wasmtime_caller_t*, const wasmtime_val_t*, size_t, wasmtime_val_t*, size_t);
static wasm_trap_t* CbScheduleBookmark(void*, wasmtime_caller_t*, const wasmtime_val_t*, size_t, wasmtime_val_t*, size_t);
static wasm_trap_t* CbWorkerTrace(void*, wasmtime_caller_t*, const wasmtime_val_t*, size_t, wasmtime_val_t*, size_t);
static wasm_trap_t* CbWorkerStub(void*, wasmtime_caller_t*, const wasmtime_val_t*, size_t, wasmtime_val_t*, size_t);

struct ImportDesc
{
        const char* name;
        std::initializer_list<wasm_valkind_t> params;
        std::initializer_list<wasm_valkind_t> results;
        wasmtime_func_callback_t hostCb;
        wasmtime_func_callback_t workerCb;
};

static const ImportDesc g_imports[] = {
        { "trace", { WASM_I32, WASM_I32 }, { }, CbTrace, CbWorkerTrace },
        { "invoke_native", { WASM_I32 }, { }, CbInvokeNative, CbWorkerStub },
        { "get_last_native_error", { WASM_I32, WASM_I32 }, { WASM_I32 }, CbGetLastNativeError, CbWorkerStub },
        { "copy_string_result", { WASM_I32, WASM_I32, WASM_I32, WASM_I32 }, { WASM_I32 }, CbCopyStringResult, CbWorkerStub },
        { "copy_binary_result", { WASM_I32, WASM_I32, WASM_I32, WASM_I32, WASM_I32 }, { WASM_I32 }, CbCopyBinaryResult, CbWorkerStub },
        { "emit_event", { WASM_I32, WASM_I32, WASM_I32, WASM_I32 }, { }, CbEmitEvent, CbWorkerStub },
        { "emit_net_event", { WASM_I32, WASM_I32, WASM_I32, WASM_I32, WASM_I32 }, { }, CbEmitNetEvent, CbWorkerStub },
        { "cancel_event", { }, { }, CbCancelEvent, CbWorkerStub },
        { "was_event_canceled", { }, { WASM_I32 }, CbWasEventCanceled, CbWorkerStub },
        { "get_resource_metadata", { WASM_I32, WASM_I32, WASM_I32, WASM_I32, WASM_I32 }, { WASM_I32 }, CbGetResourceMetadata, CbWorkerStub },
        { "get_num_resource_metadata", { WASM_I32, WASM_I32 }, { WASM_I32 }, CbGetNumResourceMetadata, CbWorkerStub },
        { "create_ref", { WASM_I32 }, { WASM_I32 }, CbCreateRef, CbWorkerStub },
        { "canonicalize_ref", { WASM_I32, WASM_I32, WASM_I32 }, { WASM_I32 }, CbCanonicalizeRef, CbWorkerStub },
        { "remove_ref", { WASM_I32 }, { }, CbRemoveRef, CbWorkerStub },
        { "invoke_function_reference", { WASM_I32, WASM_I32, WASM_I32, WASM_I32, WASM_I32 }, { }, CbInvokeFunctionReference, CbWorkerStub },
        { "get_instance_id", { }, { WASM_I32 }, CbGetInstanceId, CbWorkerStub },
        { "spawn_process", { WASM_I32, WASM_I32 }, { WASM_I32 }, CbSpawnProcess, CbWorkerStub },
        { "poll_process", { WASM_I32, WASM_I32, WASM_I32, WASM_I32 }, { WASM_I32 }, CbPollProcess, CbWorkerStub },
        { "create_worker", { WASM_I32, WASM_I32, WASM_I32, WASM_I32 }, { WASM_I32 }, CbCreateWorker, CbWorkerStub },
        { "poll_worker", { WASM_I32, WASM_I32, WASM_I32 }, { WASM_I32 }, CbPollWorker, CbWorkerStub },
        { "schedule_bookmark", { WASM_I32, WASM_I32 }, { }, CbScheduleBookmark, CbWorkerStub },
        { "is_manifest_version_v2_between", { WASM_I32, WASM_I32, WASM_I32, WASM_I32 }, { WASM_I32 }, CbIsManifestVersionV2Between, CbWorkerStub },
};

static constexpr size_t NUM_IMPORTS = sizeof(g_imports) / sizeof(g_imports[0]);

static wasm_functype_t** CachedImportFuncTypes()
{
        static wasm_functype_t** s_types = []
        {
                static wasm_functype_t* arr[NUM_IMPORTS];
                for (size_t i = 0; i < NUM_IMPORTS; ++i)
                        arr[i] = MakeFuncType(g_imports[i].params, g_imports[i].results);
                atexit([]
                {
                        for (size_t i = 0; i < NUM_IMPORTS; ++i)
                        {
                                if (arr[i])
                                {
                                        wasm_functype_delete(arr[i]);
                                        arr[i] = nullptr;
                                }
                        }
                });
                return arr;
        }();
        return s_types;
}

static wasm_trap_t* CbWorkerTrace(void* env, wasmtime_caller_t* caller, const wasmtime_val_t* args, size_t, wasmtime_val_t*, size_t)
{
        WasmMem mem;
        if (!mem.init(caller))
                return nullptr;
        uint32_t ptr = ArgU32(args[0]);
        uint32_t len = ArgU32(args[1]);
        static constexpr uint32_t MAX_TRACE_LEN = 1u << 20;
        if (len > MAX_TRACE_LEN)
                len = MAX_TRACE_LEN;
        if (!mem.check(ptr, len))
                return nullptr;
        std::string msg = mem.readStr(ptr, len);
        SanitizeTraceMsg(msg);
        auto* state = static_cast<WasmScriptRuntime::WorkerState*>(env);
        const char* resName = (state && !state->resourceName.empty()) ? state->resourceName.c_str() : "?";
        if (!msg.empty())
                ConsolePrint(std::string("worker:") + resName, std::move(msg));
        return nullptr;
}

static wasm_trap_t* CbWorkerStub(void*, wasmtime_caller_t*, const wasmtime_val_t*, size_t, wasmtime_val_t* results, size_t nresults)
{
        for (size_t i = 0; i < nresults; i++)
                results[i] = I32Val(0);
        return nullptr;
}

static wasmtime_error_t* WorkerEpochCallback(wasmtime_context_t*, void* env, uint64_t* delta, wasmtime_update_deadline_kind_t* kind)
{
        if (static_cast<WasmScriptRuntime::WorkerState*>(env)->cancelled.load(std::memory_order_relaxed))
                return wasmtime_error_new("worker cancelled");
        *delta = 1;
        *kind = WASMTIME_UPDATE_DEADLINE_CONTINUE;
        return nullptr;
}

static void RunWorker(std::shared_ptr<WasmScriptRuntime::WorkerState> state, wasmtime_module_t* mod, std::string fnName, std::vector<char> inputData)
{
        auto* eng = WasmScriptRuntime::engine();
        auto* store = wasmtime_store_new(eng, nullptr, nullptr);
        wasmtime_store_limiter(store, WASM_MEMORY_LIMIT, -1, -1, -1, -1);
        wasmtime_context_set_fuel(wasmtime_store_context(store), UINT64_MAX);
        wasmtime_context_set_epoch_deadline(wasmtime_store_context(store), 1);
        wasmtime_store_epoch_deadline_callback(store, WorkerEpochCallback, state.get(), nullptr);
        auto* linker = wasmtime_linker_new(eng);
        wasmtime_linker_allow_shadowing(linker, true);
        if (auto* wasi_err = wasmtime_linker_define_wasi(linker))
                wasmtime_error_delete(wasi_err);
        wasi_config_t* wasi = wasi_config_new();
        wasmtime_context_set_wasi(wasmtime_store_context(store), wasi);
        auto** types = CachedImportFuncTypes();
        for (size_t i = 0; i < NUM_IMPORTS; ++i)
        {
                const auto& imp = g_imports[i];
                wasmtime_linker_define_func(linker, "cfx", 3, imp.name, strlen(imp.name), types[i], imp.workerCb, state.get(), nullptr);
        }
        if (auto* fdErr = wasmtime_linker_define_func(linker, "wasi_snapshot_preview1", 22, "fd_write", 8, FdWriteFuncType(), CbWorkerFdWrite, state.get(), nullptr))
                wasmtime_error_delete(fdErr);
        auto finalize = [&](WasmScriptRuntime::WorkerState::Status s)
        {
                FlushWorkerStdio(*state, true);
                {
                        std::lock_guard<std::mutex> lk(state->mutex);
                        state->status = s;
                }
                wasmtime_linker_delete(linker);
                wasmtime_store_delete(store);
                wasmtime_module_delete(mod);
        };
        wasmtime_instance_t instance{ };
        wasm_trap_t* trap = nullptr;
        auto* err = wasmtime_linker_instantiate(linker, wasmtime_store_context(store), mod, &instance, &trap);
        if (err || trap)
        {
                if (err) wasmtime_error_delete(err);
                if (trap) wasm_trap_delete(trap);
                finalize(WasmScriptRuntime::WorkerState::Error);
                return;
        }
        auto* storeCtx = wasmtime_store_context(store);
        wasmtime_extern_t ctorExt{ };
        if (wasmtime_instance_export_get(storeCtx, &instance, "_initialize", 11, &ctorExt) && ctorExt.kind == WASMTIME_EXTERN_FUNC && !WasmCall(store, ctorExt.of.func, nullptr, 0, nullptr, 0, "worker", "_initialize"))
        {
                finalize(WasmScriptRuntime::WorkerState::Error);
                return;
        }
        wasmtime_extern_t memExt{ };
        bool hasMem = wasmtime_instance_export_get(storeCtx, &instance, "memory", 6, &memExt) && memExt.kind == WASMTIME_EXTERN_MEMORY;
        wasmtime_extern_t allocExt{ };
        bool hasAlloc = wasmtime_instance_export_get(storeCtx, &instance, "__cfx_alloc", 11, &allocExt) && allocExt.kind == WASMTIME_EXTERN_FUNC;
        wasmtime_extern_t fnExt{ };
        if (!wasmtime_instance_export_get(storeCtx, &instance, fnName.c_str(), fnName.size(), &fnExt) || fnExt.kind != WASMTIME_EXTERN_FUNC)
        {
                LogError("Worker export '%s' not found", fnName.c_str());
                finalize(WasmScriptRuntime::WorkerState::Error);
                return;
        }
        if (!hasMem || !hasAlloc)
        {
                LogError("Worker '%s' in '%s' needs the module to export memory and __cfx_alloc", fnName.c_str(), state->resourceName.c_str());
                finalize(WasmScriptRuntime::WorkerState::Error);
                return;
        }
        auto workerAlloc = [&](uint32_t size) -> uint32_t
        {
                wasmtime_val_t allocArgs[1] = { I32Val(static_cast<int32_t>(size)) };
                wasmtime_val_t allocResult[1]{ };
                if (!WasmCall(store, allocExt.of.func, allocArgs, 1, allocResult, 1, "worker", "__cfx_alloc"))
                        return 0;
                uint32_t ptr = static_cast<uint32_t>(allocResult[0].of.i32);
                return InBounds(wasmtime_memory_data_size(storeCtx, &memExt.of.memory), ptr, size) ? ptr : 0;
        };
        uint32_t inputPtr = 0;
        if (!inputData.empty())
        {
                inputPtr = workerAlloc(static_cast<uint32_t>(inputData.size()));
                if (!inputPtr)
                {
                        LogError("Worker '%s' in '%s' failed to allocate %zu bytes of input", fnName.c_str(), state->resourceName.c_str(), inputData.size());
                        finalize(WasmScriptRuntime::WorkerState::Error);
                        return;
                }
                memcpy(wasmtime_memory_data(storeCtx, &memExt.of.memory) + inputPtr, inputData.data(), inputData.size());
        }
        constexpr uint32_t resultBufSize = RESULT_BUF_SIZE;
        uint32_t resultPtr = workerAlloc(resultBufSize);
        if (!resultPtr)
        {
                LogError("Worker '%s' in '%s' failed to allocate its result buffer", fnName.c_str(), state->resourceName.c_str());
                finalize(WasmScriptRuntime::WorkerState::Error);
                return;
        }
        wasmtime_val_t callArgs[4] = {
                I32Val(static_cast<int32_t>(inputPtr)),
                I32Val(static_cast<int32_t>(inputData.size())),
                I32Val(static_cast<int32_t>(resultPtr)),
                I32Val(static_cast<int32_t>(resultBufSize))
        };
        wasmtime_val_t callResult[1]{ };
        trap = nullptr;
        err = wasmtime_func_call(storeCtx, &fnExt.of.func, callArgs, 4, callResult, 1, &trap);
        if (err || trap)
        {
                if (err) wasmtime_error_delete(err);
                if (trap) wasm_trap_delete(trap);
                finalize(WasmScriptRuntime::WorkerState::Error);
                return;
        }
        int32_t resultLen = callResult[0].of.i32;
        if (resultLen > static_cast<int32_t>(resultBufSize))
                resultLen = static_cast<int32_t>(resultBufSize);
        {
                std::lock_guard<std::mutex> lk(state->mutex);
                if (resultLen > 0)
                {
                        uint8_t* wbase = wasmtime_memory_data(storeCtx, &memExt.of.memory);
                        size_t wsz = wasmtime_memory_data_size(storeCtx, &memExt.of.memory);
                        if (InBounds(wsz, resultPtr, static_cast<size_t>(resultLen)))
                                state->result.assign(wbase + resultPtr, wbase + resultPtr + resultLen);
                }
        }
        finalize(WasmScriptRuntime::WorkerState::Done);
}

static wasm_trap_t* CbCreateWorker(void* env, wasmtime_caller_t* caller, const wasmtime_val_t* args, size_t, wasmtime_val_t* results, size_t)
{
        auto* mod = static_cast<WasmModule*>(env);
        auto* rt = mod->rt;
        WasmMem mem;
        if (!mem.init(caller))
        {
                results[0] = I32Val(-2);
                return nullptr;
        }
        uint32_t fnPtr = ArgU32(args[0]), fnLen = ArgU32(args[1]);
        uint32_t inPtr = ArgU32(args[2]), inLen = ArgU32(args[3]);
        if (!mem.check(fnPtr, fnLen) || (inLen > 0 && !mem.check(inPtr, inLen)))
        {
                results[0] = I32Val(-2);
                return nullptr;
        }
        if (!rt->host() || !HasWasmPermission(rt->host(), "sv_wasmWorkerThreads", rt->resourceName()))
        {
                LogError("Resource '%s' denied worker_threads permission. Add 'set sv_wasmWorkerThreads \"%s\"' to your server.cfg", rt->resourceName().c_str(), rt->resourceName().c_str());
                results[0] = I32Val(-1);
                return nullptr;
        }
        if (rt->m_workers.size() >= static_cast<size_t>(WasmScriptRuntime::MAX_WORKERS_PER_RESOURCE))
        {
                LogError("Resource '%s' exceeded max worker limit (%d)", rt->resourceName().c_str(), WasmScriptRuntime::MAX_WORKERS_PER_RESOURCE);
                results[0] = I32Val(-3);
                return nullptr;
        }
        std::string fnName = mem.readStr(fnPtr, fnLen);
        std::vector<char> inputData(mem.base + inPtr, mem.base + inPtr + inLen);
        int32_t workerId = fx::allocateId(rt->m_nextWorkerId, rt->m_workers);
        if (workerId < 0)
        {
                results[0] = I32Val(-3);
                return nullptr;
        }
        auto state = std::make_shared<WasmScriptRuntime::WorkerState>();
        state->resourceName = rt->resourceName();
        state->owner = mod;
        wasmtime_module_t* workerModule = wasmtime_module_clone(mod->module);
        try
        {
                state->thread = std::thread(RunWorker, state, workerModule, std::move(fnName), std::move(inputData));
        }
        catch (...)
        {
                wasmtime_module_delete(workerModule);
                results[0] = I32Val(-3);
                return nullptr;
        }
        rt->m_workers[workerId] = state;
        results[0] = I32Val(workerId);
        return nullptr;
}

static wasm_trap_t* CbPollWorker(void* env, wasmtime_caller_t* caller, const wasmtime_val_t* args, size_t, wasmtime_val_t* results, size_t)
{
        auto* mod = static_cast<WasmModule*>(env);
        auto* rt = mod->rt;
        int32_t workerId = args[0].of.i32;
        auto it = rt->m_workers.find(workerId);
        if (it == rt->m_workers.end() || it->second->owner != mod)
        {
                results[0] = I32Val(-2);
                return nullptr;
        }
        auto state = it->second;
        WasmScriptRuntime::WorkerState::Status status;
        {
                std::lock_guard<std::mutex> lk(state->mutex);
                status = state->status;
        }
        if (status == WasmScriptRuntime::WorkerState::Running)
        {
                results[0] = I32Val(-3);
                return nullptr;
        }
        if (status == WasmScriptRuntime::WorkerState::Error)
        {
                if (state->thread.joinable())
                        state->thread.join();
                rt->m_workers.erase(it);
                results[0] = I32Val(-1);
                return nullptr;
        }
        WasmMem mem;
        uint32_t outBuf = ArgU32(args[1]);
        int32_t outMax = args[2].of.i32;
        if (!mem.init(caller) || outMax < 0 || !mem.check(outBuf, static_cast<size_t>(outMax)))
        {
                results[0] = I32Val(-2);
                return nullptr;
        }
        std::vector<char> workerResult;
        {
                std::lock_guard<std::mutex> lk(state->mutex);
                if (state->result.size() > static_cast<size_t>(outMax))
                {
                        results[0] = I32Val(static_cast<int32_t>(state->result.size()));
                        return nullptr;
                }
                workerResult = std::move(state->result);
        }
        int32_t written = 0;
        {
                size_t copy = std::min<size_t>(workerResult.size(), static_cast<size_t>(outMax));
                if (copy > 0)
                        memcpy(mem.base + outBuf, workerResult.data(), copy);
                written = static_cast<int32_t>(copy);
        }
        if (state->thread.joinable())
                state->thread.join();
        rt->m_workers.erase(it);
        results[0] = I32Val(written);
        return nullptr;
}

static wasm_trap_t* CbScheduleBookmark(void* env, wasmtime_caller_t*, const wasmtime_val_t* args, size_t, wasmtime_val_t*, size_t)
{
        auto* mod = static_cast<WasmModule*>(env);
        mod->rt->scheduleWasmBookmark(*mod, args[0].of.i32, args[1].of.i32);
        return nullptr;
}

WasmScriptRuntime::WasmScriptRuntime()
{
        std::lock_guard<std::mutex> lk(s_instanceIdMutex);
        int32_t id;
        do {
                uint32_t raw = s_nextInstanceId.fetch_add(1, std::memory_order_relaxed);
                id = static_cast<int32_t>(raw & 0x7FFFFFFFu);
        } while (id == 0 || s_activeInstanceIds.count(id));
        s_activeInstanceIds.insert(id);
        m_instanceId = id;
        m_refGuard->rt = this;
}

WasmScriptRuntime::~WasmScriptRuntime()
{
        if (!m_destroyed)
        {
                AddRef();
                Destroy();
        }
        std::lock_guard<std::mutex> lk(s_instanceIdMutex);
        s_activeInstanceIds.erase(m_instanceId);
}

result_t OM_DECL WasmScriptRuntime::Create(IScriptHost* host)
{
        m_host = fx::OMPtr<IScriptHost>(host);
        fx::OMPtr<IScriptHostWithResourceData> md;
        if (FX_SUCCEEDED(m_host.As(&md)) && md.GetRef())
        {
                m_metadataHost = md;
                char* name = nullptr;
                if (FX_SUCCEEDED(md->GetResourceName(&name)) && name)
                        m_resourceName = name;
        }
        fx::OMPtr<IScriptHostWithManifest> mh;
        if (FX_SUCCEEDED(m_host.As(&mh)) && mh.GetRef())
                m_manifestHost = mh;
        fx::OMPtr<IScriptHostWithBookmarks> bh;
        if (FX_SUCCEEDED(m_host.As(&bh)) && bh.GetRef())
        {
                m_bookmarkHost = bh;
                m_bookmarkHost->CreateBookmarks(static_cast<IScriptTickRuntimeWithBookmarks*>(this));
        }
        return FX_S_OK;
}

result_t OM_DECL WasmScriptRuntime::Destroy()
{
        if (m_destroyed)
                return FX_S_OK;
        m_destroyed = true;
        {
                std::lock_guard<std::recursive_mutex> lk(m_refGuard->mu);
                m_refGuard->alive = false;
        }
        if (m_bookmarkHost.GetRef())
        {
                try
                {
                        m_bookmarkHost->RemoveBookmarks(static_cast<IScriptTickRuntimeWithBookmarks*>(this));
                }
                catch (...)
                {
                }
                m_bookmarkHost = { };
        }
        {
                auto modules = m_modules;
                fx::PushEnvironment env(static_cast<IScriptRuntime*>(this));
                for (auto& mod : modules)
                {
                        if (!mod->hasStopFn || !mod->store)
                                continue;
                        BoundaryGuard boundary(m_host.GetRef(), static_cast<int64_t>(nextBoundaryId()));
                        mod->callVoid(mod->fnStop);
                }
        }
        m_pendingLoads.clear();
        m_refs.clear();
        m_refToCallbackId.clear();
        for (auto& [id, process] : m_processes)
                process->cancelled.store(true, std::memory_order_relaxed);
        m_processes.clear();
        destroyWorkers();
        for (auto& mod : m_modules)
                destroyModule(*mod);
        m_modules.clear();
        m_host = { };
        m_metadataHost = { };
        m_manifestHost = { };
        return FX_S_OK;
}

void OM_DECL WasmScriptRuntime::SetParentObject(void* obj)
{
        m_parentObject = obj;
}

uint64_t WasmScriptRuntime::nextBoundaryId()
{
        uint64_t id = m_nextBoundaryId;
        if (++m_nextBoundaryId == 0)
                m_nextBoundaryId = 1;
        return id;
}

int32_t WasmScriptRuntime::allocRefIdx()
{
        return fx::allocateId(m_nextRefIdx, m_refs);
}

int32_t WasmScriptRuntime::AddFuncRef(fx::RefCallback cb)
{
        int32_t idx = allocRefIdx();
        if (idx < 0)
                return -1;
        m_refs[idx] = std::move(cb);
        return idx;
}

wasm_engine_t* WasmScriptRuntime::engine()
{
        static wasm_engine_t* g_engine = []()
        {
                wasm_config_t* config = wasm_config_new();
                wasmtime_config_consume_fuel_set(config, true);
                wasmtime_config_epoch_interruption_set(config, true);
                if (auto* err = wasmtime_config_cache_config_load(config, nullptr))
                        LogWarning("Wasmtime compilation cache is unavailable: %s", ExtractWasmError(err, nullptr).c_str());
                wasm_engine_t* eng = wasm_engine_new_with_config(config);
                std::thread([eng]
                {
                        while (true)
                        {
                                std::this_thread::sleep_for(std::chrono::milliseconds(EPOCH_TICK_MS));
                                wasmtime_engine_increment_epoch(eng);
                        }
                }).detach();
                return eng;
        }();
        return g_engine;
}

void WasmModule::resetLimits()
{
        if (!store)
                return;
        auto* ctx = wasmtime_store_context(store);
        wasmtime_context_set_fuel(ctx, WASM_FUEL_AMOUNT);
        wasmtime_context_set_epoch_deadline(ctx, WASM_CALL_TIMEOUT_TICKS);
}

uint8_t* WasmModule::wasmBase()
{
        if (!hasMemory || !store)
                return nullptr;
        return wasmtime_memory_data(wasmtime_store_context(store), &memory);
}

size_t WasmModule::wasmMemSize()
{
        if (!hasMemory || !store)
                return 0;
        return wasmtime_memory_data_size(wasmtime_store_context(store), &memory);
}

bool WasmModule::call(const wasmtime_func_t& fn, const wasmtime_val_t* args, size_t nargs, wasmtime_val_t* results, size_t nresults, const char* label)
{
        if (!store)
                return false;
        bool nested = callDepth > 0;
        if (!nested)
        {
                resetLimits();
                limitExceeded = false;
        }
        else if (limitExceeded)
                return false;
        ++callDepth;
        WasmModule* prev = rt->m_activeModule;
        rt->m_activeModule = this;
        bool hitLimit = false;
        bool ok = WasmCall(store, fn, args, nargs, results, nresults, rt->resourceName().c_str(), label, nested, &hitLimit);
        if (hitLimit)
                limitExceeded = true;
        rt->m_activeModule = prev;
        --callDepth;
        return ok;
}

uint32_t WasmModule::wasmAlloc(uint32_t size)
{
        if (!hasAllocFn || !store)
                return 0;
        wasmtime_val_t arg = I32Val(static_cast<int32_t>(size));
        wasmtime_val_t result{ };
        if (!call(fnAlloc, &arg, 1, &result, 1, "alloc trap"))
                return 0;
        return static_cast<uint32_t>(result.of.i32);
}

void WasmModule::wasmFree(uint32_t ptr, uint32_t size)
{
        if (!hasFreeFn || !store)
                return;
        wasmtime_val_t args[2] = { I32Val(static_cast<int32_t>(ptr)), I32Val(static_cast<int32_t>(size)) };
        if (!call(fnFree, args, 2, nullptr, 0, "free trap") && !limitExceeded)
                LogWarning("wasmFree trapped in '%s' (ptr=%u, size=%u)", rt->resourceName().c_str(), ptr, size);
}

bool WasmModule::callVoid(const wasmtime_func_t& fn)
{
        return call(fn, nullptr, 0, nullptr, 0, "trap");
}

bool WasmModule::callEvent(uint32_t namePtr, uint32_t nameLen, uint32_t argsPtr, uint32_t argsLen, uint32_t srcPtr, uint32_t srcLen)
{
        if (!hasEventFn || !store)
                return false;
        wasmtime_val_t a[6] = {
                I32Val(static_cast<int32_t>(namePtr)),
                I32Val(static_cast<int32_t>(nameLen)),
                I32Val(static_cast<int32_t>(argsPtr)),
                I32Val(static_cast<int32_t>(argsLen)),
                I32Val(static_cast<int32_t>(srcPtr)),
                I32Val(static_cast<int32_t>(srcLen)),
        };
        return call(fnEvent, a, 6, nullptr, 0, "event trap");
}

void WasmModule::duplicateRef(int32_t callbackId)
{
        if (!hasDuplicateRefFn || !store)
                return;
        wasmtime_val_t a[1] = { I32Val(callbackId) };
        wasmtime_val_t ret{ };
        call(fnDuplicateRef, a, 1, &ret, 1, "duplicate_ref trap");
}

void WasmModule::removeRef(int32_t callbackId)
{
        if (!hasRemoveRefFn || !store)
                return;
        wasmtime_val_t a[1] = { I32Val(callbackId) };
        call(fnRemoveRef, a, 1, nullptr, 0, "remove_ref trap");
}

void WasmScriptRuntime::scheduleWasmBookmark(WasmModule& mod, int32_t wasmId, int32_t deadlineMs)
{
        if (!m_bookmarkHost.GetRef())
                return;
        uint64_t hostId = (static_cast<uint64_t>(mod.bookmarkSlot) << 32) | static_cast<uint32_t>(wasmId);
        if (deadlineMs < 0)
                deadlineMs = 0;
        int64_t deadline = (deadlineMs == 0) ? 0 : -static_cast<int64_t>(deadlineMs);
        m_bookmarkHost->ScheduleBookmark(static_cast<IScriptTickRuntimeWithBookmarks*>(this), hostId, deadline);
}

bool WasmScriptRuntime::callInvokeRef(WasmModule& mod, uint32_t callbackId, const char* argsSerialized, uint32_t argsSize, std::vector<char>& result)
{
        if (!mod.hasInvokeRefFn || !mod.store || !mod.hasAllocFn)
                return false;
        uint32_t argsAllocSz = argsSize > 0 ? argsSize : 1;
        uint32_t argsPtr = mod.wasmAlloc(argsAllocSz);
        if (!argsPtr)
                return false;
        uint8_t* base = mod.wasmBase();
        size_t memSz = mod.wasmMemSize();
        if (!InBounds(memSz, argsPtr, argsSize))
        {
                mod.wasmFree(argsPtr, argsAllocSz);
                return false;
        }
        if (argsSize > 0)
                memcpy(base + argsPtr, argsSerialized, argsSize);
        static constexpr uint32_t MAX_REF_RESULT_SIZE = 16u * 1024u * 1024u;
        uint32_t resultBufMax = RESULT_BUF_SIZE;
        uint32_t resultPtr = mod.wasmAlloc(resultBufMax);
        if (!resultPtr)
        {
                mod.wasmFree(argsPtr, argsAllocSz);
                return false;
        }
        wasmtime_val_t a[5] = {
                I32Val(static_cast<int32_t>(callbackId)),
                I32Val(static_cast<int32_t>(argsPtr)),
                I32Val(static_cast<int32_t>(argsSize)),
                I32Val(static_cast<int32_t>(resultPtr)),
                I32Val(static_cast<int32_t>(resultBufMax)),
        };
        wasmtime_val_t ret{ };
        if (!mod.call(mod.fnInvokeRef, a, 5, &ret, 1, "invoke_ref trap"))
        {
                mod.wasmFree(resultPtr, resultBufMax);
                mod.wasmFree(argsPtr, argsAllocSz);
                return false;
        }
        int32_t actualLen = ret.of.i32;
        if (actualLen > 0 && static_cast<uint32_t>(actualLen) > resultBufMax && static_cast<uint32_t>(actualLen) <= MAX_REF_RESULT_SIZE)
        {
                mod.wasmFree(resultPtr, resultBufMax);
                uint32_t newBufMax = static_cast<uint32_t>(actualLen);
                uint32_t newResultPtr = mod.wasmAlloc(newBufMax);
                if (!newResultPtr)
                {
                        mod.wasmFree(argsPtr, argsAllocSz);
                        return false;
                }
                resultPtr = newResultPtr;
                resultBufMax = newBufMax;
                a[3] = I32Val(static_cast<int32_t>(resultPtr));
                a[4] = I32Val(static_cast<int32_t>(resultBufMax));
                wasmtime_val_t ret2{ };
                if (!mod.call(mod.fnInvokeRef, a, 5, &ret2, 1, "invoke_ref trap"))
                {
                        mod.wasmFree(resultPtr, resultBufMax);
                        mod.wasmFree(argsPtr, argsAllocSz);
                        return false;
                }
                actualLen = ret2.of.i32;
        }
        mod.wasmFree(argsPtr, argsAllocSz);
        if (actualLen > 0 && static_cast<uint32_t>(actualLen) > resultBufMax)
                LogWarning("Dropping a %d byte ref result in '%s', the limit is %u bytes", actualLen, m_resourceName.c_str(), MAX_REF_RESULT_SIZE);
        else if (actualLen > 0)
        {
                base = mod.wasmBase();
                memSz = mod.wasmMemSize();
                if (InBounds(memSz, resultPtr, static_cast<uint32_t>(actualLen)))
                        result.assign(reinterpret_cast<char*>(base + resultPtr), reinterpret_cast<char*>(base + resultPtr) + actualLen);
        }
        mod.wasmFree(resultPtr, resultBufMax);
        if (result.empty())
                result = std::vector<char>{ static_cast<char>(MSGPACK_EMPTY_ARRAY) };
        return true;
}

void WasmScriptRuntime::defineImports(WasmModule& mod)
{
        auto** types = CachedImportFuncTypes();
        for (size_t i = 0; i < NUM_IMPORTS; ++i)
        {
                const auto& imp = g_imports[i];
                auto* err = wasmtime_linker_define_func(mod.linker, "cfx", 3, imp.name, strlen(imp.name), types[i], imp.hostCb, &mod, nullptr);
                if (err)
                        LogError("failed to define import '%s': %s", imp.name, ExtractWasmError(err, nullptr).c_str());
        }
        if (auto* err = wasmtime_linker_define_func(mod.linker, "wasi_snapshot_preview1", 22, "fd_write", 8, FdWriteFuncType(), CbFdWrite, &mod, nullptr))
                LogError("failed to define fd_write: %s", ExtractWasmError(err, nullptr).c_str());
}

bool WasmScriptRuntime::resolveExports(WasmModule& mod)
{
        auto* ctx = wasmtime_store_context(mod.store);
        auto get = [&](const char* name, wasmtime_func_t& fn) -> bool
        {
                wasmtime_extern_t ext{ };
                if (!wasmtime_instance_export_get(ctx, &mod.instance, name, strlen(name), &ext))
                        return false;
                if (ext.kind != WASMTIME_EXTERN_FUNC)
                        return false;
                fn = ext.of.func;
                return true;
        };
        wasmtime_func_t initFn{ };
        if (!get("__cfx_init", initFn))
        {
                LogError("'%s' in resource '%s' missing __cfx_init export", mod.scriptFile.c_str(), m_resourceName.c_str());
                return false;
        }
        mod.hasTickFn = get("__cfx_on_tick", mod.fnTick);
        mod.hasEventFn = get("__cfx_on_event", mod.fnEvent);
        mod.hasStopFn = get("__cfx_on_stop", mod.fnStop);
        mod.hasAllocFn = get("__cfx_alloc", mod.fnAlloc);
        mod.hasFreeFn = get("__cfx_free", mod.fnFree);
        mod.hasInvokeRefFn = get("__cfx_invoke_ref", mod.fnInvokeRef);
        mod.hasDuplicateRefFn = get("__cfx_duplicate_ref", mod.fnDuplicateRef);
        mod.hasRemoveRefFn = get("__cfx_remove_ref", mod.fnRemoveRef);
        mod.hasHasPendingWorkFn = get("__cfx_has_pending_work", mod.fnHasPendingWork);
        mod.hasTickBookmarksFn = get("__cfx_tick_bookmarks", mod.fnTickBookmarks);
        wasmtime_extern_t memExt{ };
        if (wasmtime_instance_export_get(ctx, &mod.instance, "memory", 6, &memExt) && memExt.kind == WASMTIME_EXTERN_MEMORY)
        {
                mod.memory = memExt.of.memory;
                mod.hasMemory = true;
        }
        wasmtime_func_t ctorFn{ };
        if (get("_initialize", ctorFn) && !mod.call(ctorFn, nullptr, 0, nullptr, 0, "_initialize trap"))
                return false;
        return mod.call(initFn, nullptr, 0, nullptr, 0, "__cfx_init trap");
}

static std::atomic<bool> g_serverTicking{ false };

void WasmScriptRuntime::destroyWorkers()
{
        for (auto& [id, w] : m_workers)
                w->cancelled.store(true, std::memory_order_relaxed);
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(WORKER_STOP_TIMEOUT_MS);
        for (auto& [id, w] : m_workers)
        {
                if (!w->thread.joinable())
                        continue;
                auto running = [&]
                {
                        std::lock_guard<std::mutex> lk(w->mutex);
                        return w->status == WorkerState::Running;
                };
                while (running() && std::chrono::steady_clock::now() < deadline)
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                if (running())
                {
                        LogWarning("Worker %d in '%s' is blocked outside wasm and can't be cancelled yet, detaching it", id, m_resourceName.c_str());
                        w->thread.detach();
                }
                else
                        w->thread.join();
        }
        m_workers.clear();
}

void WasmScriptRuntime::destroyModule(WasmModule& mod)
{
        FlushModuleStdio(mod, true);
        for (auto it = m_refToCallbackId.begin(); it != m_refToCallbackId.end(); )
        {
                if (it->second.mod == &mod)
                {
                        m_refs.erase(it->first);
                        it = m_refToCallbackId.erase(it);
                }
                else
                        ++it;
        }
        for (auto& [id, w] : m_workers)
        {
                if (w->owner == &mod)
                        w->owner = nullptr;
        }
        std::erase_if(m_processes, [&](const auto& p)
        {
                if (p.second->owner != &mod)
                        return false;
                p.second->cancelled.store(true, std::memory_order_relaxed);
                return true;
        });
        if (mod.linker)
        {
                wasmtime_linker_delete(mod.linker);
                mod.linker = nullptr;
        }
        if (mod.module)
        {
                wasmtime_module_delete(mod.module);
                mod.module = nullptr;
        }
        if (mod.store)
        {
                wasmtime_store_delete(mod.store);
                mod.store = nullptr;
        }
        mod.hasMemory = false;
        mod.hasTickFn = false;
        mod.hasEventFn = false;
        mod.hasStopFn = false;
        mod.hasAllocFn = false;
        mod.hasFreeFn = false;
        mod.hasInvokeRefFn = false;
        mod.hasDuplicateRefFn = false;
        mod.hasRemoveRefFn = false;
        mod.hasHasPendingWorkFn = false;
        mod.hasTickBookmarksFn = false;
}

result_t WasmScriptRuntime::loadWasm(wasmtime_module_t* module, const std::string& scriptFile, const std::string& sourcePath)
{
        auto mod = std::make_shared<WasmModule>();
        mod->rt = this;
        mod->module = module;
        mod->scriptFile = scriptFile;
        mod->bookmarkSlot = m_nextBookmarkSlot++;
        mod->store = wasmtime_store_new(engine(), mod.get(), nullptr);
        wasmtime_store_limiter(mod->store, WASM_MEMORY_LIMIT, -1, -1, -1, -1);
        mod->linker = wasmtime_linker_new(engine());
        wasmtime_linker_allow_shadowing(mod->linker, true);
        {
                auto* err = wasmtime_linker_define_wasi(mod->linker);
                if (err)
                        LogWarning("WASI linker error in '%s': %s", m_resourceName.c_str(), ExtractWasmError(err, nullptr).c_str());
                wasi_config_t* wasi = wasi_config_new();
                auto* werr = wasmtime_context_set_wasi(wasmtime_store_context(mod->store), wasi);
                if (werr)
                        LogWarning("WASI context error in '%s': %s", m_resourceName.c_str(), ExtractWasmError(werr, nullptr).c_str());
        }
        defineImports(*mod);
        mod->resetLimits();
        {
                wasm_trap_t* trap = nullptr;
                auto* err = wasmtime_linker_instantiate(mod->linker, wasmtime_store_context(mod->store), mod->module, &mod->instance, &trap);
                if (err || trap)
                {
                        LogError("Instantiate error in '%s': %s", sourcePath.c_str(), ExtractWasmError(err, trap).c_str());
                        destroyModule(*mod);
                        return FX_E_INVALIDARG;
                }
        }
        m_modules.push_back(mod);
        fx::PushEnvironment envGuard(static_cast<IScriptRuntime*>(this));
        if (!resolveExports(*mod))
        {
                std::erase(m_modules, mod);
                destroyModule(*mod);
                return FX_E_INVALIDARG;
        }
        {
                std::string msg = "Warning: WebAssembly " + m_resourceName + "/" + scriptFile + " has been loaded into the webassembly rt. This runtime is still in beta and shouldn't be used in production, crashes and breaking changes are to be expected.";
                if (m_host.GetRef())
                        m_host->ScriptTrace(Mut(msg));
                LogWarning("%s", msg.c_str());
        }
        return FX_S_OK;
}

result_t OM_DECL WasmScriptRuntime::Tick()
{
        g_serverTicking.store(true, std::memory_order_relaxed);
        drainPendingLoads();
        if (m_modules.empty())
                return FX_S_OK;
        auto modules = m_modules;
        fx::PushEnvironment env(static_cast<IScriptRuntime*>(this));
        for (auto& mod : modules)
        {
                if (!mod->hasTickFn || !mod->store)
                        continue;
                if (mod->hasHasPendingWorkFn)
                {
                        wasmtime_val_t ret{ };
                        if (!mod->call(mod->fnHasPendingWork, nullptr, 0, &ret, 1, "has_pending_work trap"))
                                continue;
                        if (ret.of.i32 == 0)
                                continue;
                }
                BoundaryGuard boundary(m_host.GetRef(), static_cast<int64_t>(nextBoundaryId()));
                if (!mod->callVoid(mod->fnTick))
                        ScriptPrint(m_host.GetRef(), m_resourceName, "^1SCRIPT ERROR: @" + m_resourceName + "/" + mod->scriptFile + ": WASM trap in tick handler^7");
        }
        return FX_S_OK;
}

result_t OM_DECL WasmScriptRuntime::TickBookmarks(uint64_t* bookmarks, int32_t numBookmarks)
{
        if (numBookmarks <= 0 || m_modules.empty())
                return FX_S_OK;
        auto modules = m_modules;
        fx::PushEnvironment env(static_cast<IScriptRuntime*>(this));
        WasmModule* runMod = nullptr;
        std::vector<int32_t> wasmIds;
        auto flush = [&]()
        {
                if (!runMod || wasmIds.empty())
                        return;
                auto& mod = *runMod;
                if (!mod.hasTickBookmarksFn || !mod.store)
                {
                        wasmIds.clear();
                        return;
                }
                uint32_t arrSize = static_cast<uint32_t>(wasmIds.size() * sizeof(int32_t));
                uint32_t arrPtr = mod.wasmAlloc(arrSize);
                if (arrPtr)
                {
                        uint8_t* base = mod.wasmBase();
                        size_t memSz = mod.wasmMemSize();
                        if (InBounds(memSz, arrPtr, arrSize))
                        {
                                memcpy(base + arrPtr, wasmIds.data(), arrSize);
                                BoundaryGuard boundary(m_host.GetRef(), static_cast<int64_t>(nextBoundaryId()));
                                wasmtime_val_t args[2] = { I32Val(static_cast<int32_t>(arrPtr)), I32Val(static_cast<int32_t>(wasmIds.size())) };
                                mod.call(mod.fnTickBookmarks, args, 2, nullptr, 0, "tick_bookmarks trap");
                        }
                        mod.wasmFree(arrPtr, arrSize);
                }
                wasmIds.clear();
        };
        for (int32_t i = 0; i < numBookmarks; ++i)
        {
                uint32_t slot = static_cast<uint32_t>(bookmarks[i] >> 32);
                if (!runMod || runMod->bookmarkSlot != slot)
                {
                        flush();
                        auto it = std::find_if(modules.begin(), modules.end(), [&](const std::shared_ptr<WasmModule>& m) { return m->bookmarkSlot == slot; });
                        runMod = it != modules.end() ? it->get() : nullptr;
                }
                if (runMod)
                        wasmIds.push_back(static_cast<int32_t>(static_cast<uint32_t>(bookmarks[i])));
        }
        flush();
        return FX_S_OK;
}

result_t OM_DECL WasmScriptRuntime::TriggerEvent(char* eventName, char* argsSerialized, uint32_t serializedSize, char* sourceId)
{
        if (!eventName)
                return FX_S_OK;
        if (std::none_of(m_modules.begin(), m_modules.end(), [](const std::shared_ptr<WasmModule>& m) { return m->hasEventFn; }))
                return FX_S_OK;
        fx::PushEnvironment env(static_cast<IScriptRuntime*>(this));
        BoundaryGuard boundary(m_host.GetRef(), static_cast<int64_t>(nextBoundaryId()));
        std::string_view name(eventName);
        std::string_view src(sourceId ? sourceId : "-1");
        uint64_t nameAllocSz64 = static_cast<uint64_t>(name.size()) + 1;
        uint64_t argsAllocSz64 = serializedSize > 0 ? static_cast<uint64_t>(serializedSize) : 1;
        uint64_t srcAllocSz64 = static_cast<uint64_t>(src.size()) + 1;
        uint64_t totalSz64 = nameAllocSz64 + argsAllocSz64 + srcAllocSz64;
        if (totalSz64 > UINT32_MAX)
                return FX_S_OK;
        uint32_t nameAllocSz = static_cast<uint32_t>(nameAllocSz64);
        uint32_t argsAllocSz = static_cast<uint32_t>(argsAllocSz64);
        uint32_t totalSz = static_cast<uint32_t>(totalSz64);
        bool outerCanceled = m_eventCanceled;
        m_eventCanceled = false;
        auto modules = m_modules;
        for (auto& mod : modules)
        {
                if (!mod->hasEventFn || !mod->store)
                        continue;
                uint32_t block = mod->wasmAlloc(totalSz);
                if (!block)
                        continue;
                uint32_t nameWasm = block;
                uint32_t argsWasm = block + nameAllocSz;
                uint32_t srcWasm = block + nameAllocSz + argsAllocSz;
                uint8_t* base = mod->wasmBase();
                size_t memSz = mod->wasmMemSize();
                if (!InBounds(memSz, block, totalSz))
                {
                        mod->wasmFree(block, totalSz);
                        continue;
                }
                memcpy(base + nameWasm, name.data(), name.size());
                base[nameWasm + name.size()] = '\0';
                if (serializedSize > 0)
                        memcpy(base + argsWasm, argsSerialized, serializedSize);
                memcpy(base + srcWasm, src.data(), src.size());
                base[srcWasm + src.size()] = '\0';
                bool eventOk = mod->callEvent(nameWasm, static_cast<uint32_t>(name.size()), argsWasm, serializedSize, srcWasm, static_cast<uint32_t>(src.size()));
                if (!eventOk && !(mod->limitExceeded && mod->callDepth > 0))
                        ScriptPrint(m_host.GetRef(), m_resourceName, "^1SCRIPT ERROR: @" + m_resourceName + "/" + mod->scriptFile + ": WASM trap in event '" + eventName + "'^7");
                mod->wasmFree(block, totalSz);
        }
        m_eventCanceled = outerCanceled;
        return FX_S_OK;
}

result_t OM_DECL WasmScriptRuntime::CallRef(int32_t refIdx, char* argsSerialized, uint32_t argsSize, IScriptBuffer** retval)
{
        auto it = m_refs.find(refIdx);
        if (it == m_refs.end())
                return FX_E_INVALIDARG;
        fx::RefCallback callback = it->second;
        fx::PushEnvironment env(static_cast<IScriptRuntime*>(this));
        BoundaryGuard boundary(m_host.GetRef(), static_cast<int64_t>(nextBoundaryId()));
        std::vector<char> result;
        try
        {
                result = callback(argsSerialized, argsSize);
        }
        catch (const std::exception& e)
        {
                ScriptPrint(m_host.GetRef(), m_resourceName, "^1SCRIPT ERROR: @" + m_resourceName + ": Unhandled exception in ref " + std::to_string(refIdx) + ": " + e.what() + "^7");
                result = std::vector<char>{ static_cast<char>(MSGPACK_EMPTY_ARRAY) };
        }
        catch (...)
        {
                ScriptPrint(m_host.GetRef(), m_resourceName, "^1SCRIPT ERROR: @" + m_resourceName + ": Unhandled non-standard exception in ref " + std::to_string(refIdx) + "^7");
                result = std::vector<char>{ static_cast<char>(MSGPACK_EMPTY_ARRAY) };
        }
        auto buf = fx::MakeNew<ScriptBuffer>(std::move(result));
        return buf->QueryInterface(IScriptBuffer::GetIID(), reinterpret_cast<void**>(retval));
}

result_t OM_DECL WasmScriptRuntime::DuplicateRef(int32_t refIdx, int32_t* newRefIdx)
{
        auto it = m_refs.find(refIdx);
        if (it == m_refs.end())
                return FX_E_INVALIDARG;
        int32_t idx = allocRefIdx();
        if (idx < 0)
                return FX_E_INVALIDARG;
        *newRefIdx = idx;
        m_refs[idx] = it->second;
        auto cit = m_refToCallbackId.find(refIdx);
        if (cit != m_refToCallbackId.end())
        {
                WasmRef ref = cit->second;
                m_refToCallbackId.emplace(*newRefIdx, ref);
                ref.mod->duplicateRef(ref.callbackId);
        }
        return FX_S_OK;
}

result_t OM_DECL WasmScriptRuntime::RemoveRef(int32_t refIdx)
{
        auto cit = m_refToCallbackId.find(refIdx);
        if (cit != m_refToCallbackId.end())
        {
                WasmRef ref = cit->second;
                m_refToCallbackId.erase(cit);
                ref.mod->removeRef(ref.callbackId);
        }
        m_refs.erase(refIdx);
        return FX_S_OK;
}

int32_t OM_DECL WasmScriptRuntime::HandlesFile(char* scriptFile, IScriptHostWithResourceData*)
{
        if (!scriptFile)
                return 0;
        std::string_view file(scriptFile);
        if (file.ends_with(".wasm") || file.ends_with(".cpp") || file.ends_with(".rs"))
                return 1;
        return 0;
}

__asm__(
        ".section .rodata\n"
        ".global g_sdkBlob\n"
        ".global g_sdkBlobEnd\n"
        "g_sdkBlob:\n"
        ".incbin \"../src/sdk.blob\"\n"
        "g_sdkBlobEnd:\n"
        ".text\n"
);
extern "C" const unsigned char g_sdkBlob[];
extern "C" const unsigned char g_sdkBlobEnd[];

static bool fileExists(const std::string& path)
{
        struct stat st;
        return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

static bool isNewer(const std::string& a, const std::string& b)
{
        struct stat sa, sb;
        if (stat(a.c_str(), &sa) != 0 || stat(b.c_str(), &sb) != 0)
                return false;
        if (sa.st_mtim.tv_sec != sb.st_mtim.tv_sec)
                return sa.st_mtim.tv_sec > sb.st_mtim.tv_sec;
        return sa.st_mtim.tv_nsec > sb.st_mtim.tv_nsec;
}

static bool extractSdk(const std::string& dir)
{
        const unsigned char* p = g_sdkBlob;
        const unsigned char* end = g_sdkBlobEnd;
        if (end - p < 4)
                return false;
        uint32_t count;
        memcpy(&count, p, 4);
        p += 4;
        for (uint32_t i = 0; i < count; i++)
        {
                if (end - p < 4) return false;
                uint32_t pathLen;
                memcpy(&pathLen, p, 4);
                p += 4;
                if (static_cast<size_t>(end - p) < pathLen) return false;
                std::string relPath(reinterpret_cast<const char*>(p), pathLen);
                p += pathLen;
                if (end - p < 4) return false;
                uint32_t dataLen;
                memcpy(&dataLen, p, 4);
                p += 4;
                if (static_cast<size_t>(end - p) < dataLen) return false;
                for (size_t slash = relPath.find('/'); slash != std::string::npos; slash = relPath.find('/', slash + 1))
                        mkdir((dir + "/" + relPath.substr(0, slash)).c_str(), 0755);
                FILE* f = fopen((dir + "/" + relPath).c_str(), "wb");
                if (!f) return false;
                bool writeOk = fwrite(p, 1, dataLen, f) == dataLen;
                fclose(f);
                if (!writeOk) return false;
                p += dataLen;
        }
        return true;
}

static std::string fnv1aHex(const unsigned char* p, const unsigned char* end)
{
        uint64_t h = 0xcbf29ce484222325ULL;
        for (; p != end; ++p)
                h = (h ^ *p) * 0x100000001b3ULL;
        char buf[17];
        snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
        return buf;
}

static const std::string& getSdkHash()
{
        static const std::string s_hash = fnv1aHex(g_sdkBlob, g_sdkBlobEnd);
        return s_hash;
}

static void removeTree(const std::string& path)
{
        nftw(path.c_str(), [](const char* p, const struct stat*, int, struct FTW*) { return remove(p); }, 16, FTW_DEPTH | FTW_PHYS);
}

static bool isPrivateDir(const std::string& path)
{
        struct stat st;
        return lstat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == geteuid() && (st.st_mode & (S_IWGRP | S_IWOTH)) == 0;
}

static std::string getSdkCacheRoot()
{
        std::string base;
        if (const char* xdg = getenv("XDG_CACHE_HOME"); xdg && *xdg)
                base = xdg;
        else if (const char* home = getenv("HOME"); home && *home)
                base = std::string(home) + "/.cache";
        else
                base = "/tmp";
        mkdir(base.c_str(), 0700);
        std::string root = base + "/citizen-scripting-wasm";
        mkdir(root.c_str(), 0700);
        return isPrivateDir(root) ? root : std::string();
}

static void pruneOldSdks(const std::string& root, const std::string& keep)
{
        DIR* d = opendir(root.c_str());
        if (!d)
                return;
        time_t cutoff = time(nullptr) - 7 * 24 * 60 * 60;
        while (dirent* e = readdir(d))
        {
                std::string_view name(e->d_name);
                if (!name.starts_with("sdk-") || name == keep)
                        continue;
                std::string path = root + "/" + std::string(name);
                struct stat st;
                if (lstat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode) && st.st_mtime < cutoff)
                        removeTree(path);
        }
        closedir(d);
}

static std::string getCachedSdkPath()
{
        std::string root = getSdkCacheRoot();
        if (root.empty())
                return { };
        std::string name = "sdk-" + getSdkHash();
        std::string path = root + "/" + name;
        if (!isPrivateDir(path))
        {
                std::string tmp = path + ".tmp-XXXXXX";
                if (!mkdtemp(tmp.data()))
                        return { };
                if (!extractSdk(tmp))
                {
                        removeTree(tmp);
                        return { };
                }
                if (rename(tmp.c_str(), path.c_str()) != 0)
                {
                        removeTree(tmp);
                        if (!isPrivateDir(path))
                                return { };
                }
        }
        utimensat(AT_FDCWD, path.c_str(), nullptr, 0);
        pruneOldSdks(root, name);
        return path;
}

static std::string getSdkPath()
{
        static std::string s_sdkPath;
        static std::once_flag s_once;
        std::call_once(s_once, [] {
                s_sdkPath = getCachedSdkPath();
                if (!s_sdkPath.empty())
                        return;
                LogWarning("SDK cache directory is unavailable, extracting the SDK to a temporary directory");
                char tmpl[] = "/tmp/citizen-scripting-wasm-sdk-XXXXXX";
                char* dir = mkdtemp(tmpl);
                if (!dir)
                        return;
                s_sdkPath = dir;
                std::atexit([] { if (!s_sdkPath.empty()) removeTree(s_sdkPath); });
                if (!extractSdk(s_sdkPath))
                        s_sdkPath.clear();
        });
        return s_sdkPath;
}

static const std::vector<std::string> g_cppFlags = { "-target", "wasm32-wasi", "-std=c++23", "-O2", "-fno-exceptions", "-mexec-model=reactor", "-g0", "-Wl,--export-memory" };
static const std::vector<std::string> g_rustFlags = { "--crate-type", "cdylib", "--edition", "2021", "-O", "--target", "wasm32-wasip1", "-C", "link-args=--export-memory", "-C", "strip=debuginfo" };

static const std::vector<std::string> g_rlibFlags = { "--crate-type", "rlib", "--crate-name", "cfx", "--edition", "2021", "-O", "--target", "wasm32-wasip1", "--cfg", "cfx_has_natives" };
static const std::vector<std::string> g_cppPostLink = { "strip-debug-sections" };

static std::string hashWithFlags(std::string key, std::initializer_list<const std::vector<std::string>*> lists)
{
        for (const auto* list : lists)
        {
                for (const auto& flag : *list)
                {
                        key += '\0';
                        key += flag;
                }
        }
        return fnv1aHex(reinterpret_cast<const unsigned char*>(key.data()), reinterpret_cast<const unsigned char*>(key.data() + key.size()));
}

static const std::string& getBuildStamp(bool rust)
{
        static const std::string s_cpp = hashWithFlags(getSdkHash(), { &g_cppFlags, &g_cppPostLink });
        static const std::string s_rust = hashWithFlags(getSdkHash(), { &g_rlibFlags, &g_rustFlags });
        return rust ? s_rust : s_cpp;
}

static bool isBuildStampCurrent(const std::string& wasmPath, bool rust)
{
        FILE* f = fopen((wasmPath + ".sdk").c_str(), "rb");
        if (!f)
                return false;
        char buf[17] = { };
        size_t n = fread(buf, 1, 16, f);
        fclose(f);
        return n == 16 && getBuildStamp(rust) == buf;
}

static void writeBuildStamp(const std::string& wasmPath, bool rust)
{
        FILE* f = fopen((wasmPath + ".sdk").c_str(), "wb");
        if (!f)
                return;
        const std::string& stamp = getBuildStamp(rust);
        fwrite(stamp.data(), 1, stamp.size(), f);
        fclose(f);
}

static bool readTextFile(const std::string& path, std::string& data)
{
        FILE* f = fopen(path.c_str(), "rb");
        if (!f)
                return false;
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
                data.append(buf, n);
        fclose(f);
        return true;
}

static bool parseDepRule(const std::string& data, std::vector<std::string>& deps)
{
        size_t pos = data.find(": ");
        if (pos == std::string::npos)
                return false;
        std::string dep;
        for (size_t i = pos + 2; i < data.size(); i++)
        {
                char c = data[i];
                char next = i + 1 < data.size() ? data[i + 1] : '\0';
                if (c == '\\' && (next == ' ' || next == '#'))
                {
                        dep += next;
                        i++;
                        continue;
                }
                if (c == '\\' && next == '\n')
                {
                        i++;
                        c = ' ';
                }
                else if (c == '$' && next == '$')
                {
                        dep += '$';
                        i++;
                        continue;
                }
                if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
                {
                        dep += c;
                        continue;
                }
                if (!dep.empty())
                        deps.push_back(std::move(dep));
                dep.clear();
                if (c == '\n')
                        break;
        }
        if (!dep.empty())
                deps.push_back(std::move(dep));
        return true;
}

static bool depsChanged(const std::string& wasmPath)
{
        std::string data;
        std::vector<std::string> deps;
        if (!readTextFile(wasmPath + ".d", data) || !parseDepRule(data, deps))
                return true;
        for (const auto& dep : deps)
        {
                if (!fileExists(dep) || isNewer(dep, wasmPath))
                        return true;
        }
        return false;
}

static bool processFailed(const fx::ProcessResult& result, std::string& reason)
{
        if (result.status == -3)
                reason = "timed out";
        else if (result.status < 0)
                reason = "could not be started";
        else if (result.exitCode != 0)
                reason = "exit " + std::to_string(result.exitCode);
        else
                return false;
        return true;
}

static const std::string& getRustcHash()
{
        static std::string s_hash;
        static std::chrono::steady_clock::time_point s_nextAttempt{ };
        auto now = std::chrono::steady_clock::now();
        if (s_hash.empty() && now >= s_nextAttempt)
        {
                s_nextAttempt = now + std::chrono::seconds(30);
                auto result = fx::spawnProgram({ "rustc", "-vV" }, 65536, 10000);
                std::string reason;
                if (!processFailed(result, reason))
                        s_hash = fnv1aHex(reinterpret_cast<const unsigned char*>(result.output.data()), reinterpret_cast<const unsigned char*>(result.output.data() + result.output.size()));
        }
        return s_hash;
}

static std::string ensureRustRlib(const std::string& sdkPath)
{
        static std::mutex s_mutex;
        std::lock_guard<std::mutex> lk(s_mutex);
        const std::string& rustcHash = getRustcHash();
        if (rustcHash.empty())
        {
                LogError("Failed to run 'rustc -vV', is Rust installed and on PATH?");
                return { };
        }
        std::string dir = sdkPath + "/rust-" + hashWithFlags(rustcHash, { &g_rlibFlags });
        std::string rlib = dir + "/libcfx.rlib";
        std::string src = sdkPath + "/include/RustSdk.rs";
        if (fileExists(rlib) && !isNewer(src, rlib))
                return rlib;
        mkdir(dir.c_str(), 0700);
        std::string tmp = rlib + ".tmp-" + std::to_string(getpid());
        std::vector<std::string> args{ "rustc" };
        args.insert(args.end(), g_rlibFlags.begin(), g_rlibFlags.end());
        args.insert(args.end(), { src, "-o", tmp });
        auto result = fx::spawnProgram(args, 1048576, 120000);
        std::string reason;
        if (processFailed(result, reason))
        {
                unlink(tmp.c_str());
                LogError("Failed to compile Rust SDK rlib (%s):\n%s", reason.c_str(), result.output.c_str());
                return { };
        }
        if (!result.output.empty())
                LogInfo("[rust-sdk] %s", result.output.c_str());
        if (rename(tmp.c_str(), rlib.c_str()) != 0)
        {
                unlink(tmp.c_str());
                LogError("Failed to move Rust SDK rlib into place: %s", strerror(errno));
                return { };
        }
        return rlib;
}

static std::string RealPath(const std::string& path)
{
        char* real = realpath(path.c_str(), nullptr);
        if (!real)
                return { };
        std::string out = real;
        free(real);
        return out;
}

static bool DepsInside(const std::vector<std::string>& deps, const std::string& root, const std::string& sdkPath, const std::string& source, const std::string& resourceName)
{
        std::string sdkRoot = RealPath(sdkPath);
        for (const auto& dep : deps)
        {
                std::string real = RealPath(dep);
                if (!real.empty() && (real.starts_with(root + "/") || (!sdkRoot.empty() && real.starts_with(sdkRoot + "/"))))
                        continue;
                LogError("'%s' in resource '%s' reads '%s' at compile time, only files inside the resource can be used", source.c_str(), resourceName.c_str(), dep.c_str());
                return false;
        }
        return true;
}

static bool compileRust(const std::string& rsPath, const std::string& wasmPath, const std::string& sdkPath, const std::string& resourceName, const std::string& root)
{
        std::string rlib = ensureRustRlib(sdkPath);
        if (rlib.empty())
                return false;
        size_t slash = wasmPath.rfind('/');
        std::string outDir = slash == std::string::npos ? std::string(".") : wasmPath.substr(0, slash);
        std::vector<std::string> args{ "rustc" };
        args.insert(args.end(), g_rustFlags.begin(), g_rustFlags.end());
        args.insert(args.end(), { "--extern", "cfx=" + rlib, rsPath, "--out-dir", outDir, "--emit=link=" + wasmPath + ",dep-info=" + wasmPath + ".d" });
        auto result = fx::spawnProgram(args, 1048576, 120000);
        std::string reason;
        if (processFailed(result, reason))
        {
                LogError("Failed to compile '%s' in resource '%s' (%s):\n%s", rsPath.c_str(), resourceName.c_str(), reason.c_str(), result.output.c_str());
                return false;
        }
        std::string depInfo;
        std::vector<std::string> deps;
        if (!readTextFile(wasmPath + ".d", depInfo) || !parseDepRule(depInfo, deps) || !DepsInside(deps, root, sdkPath, rsPath, resourceName))
        {
                unlink(wasmPath.c_str());
                unlink((wasmPath + ".d").c_str());
                return false;
        }
        if (!result.output.empty())
                LogInfo("[%s] %s", resourceName.c_str(), result.output.c_str());
        return true;
}

static bool ReadFileBytes(const std::string& path, const std::string& resolvedRoot, std::vector<uint8_t>& out)
{
        int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0)
                return false;
        char fdLink[64];
        snprintf(fdLink, sizeof(fdLink), "/proc/self/fd/%d", fd);
        char* fdResolved = realpath(fdLink, nullptr);
        if (!fdResolved)
        {
                close(fd);
                return false;
        }
        bool pathOk = (strncmp(fdResolved, resolvedRoot.c_str(), resolvedRoot.size()) == 0 && (fdResolved[resolvedRoot.size()] == '/' || fdResolved[resolvedRoot.size()] == '\0'));
        free(fdResolved);
        if (!pathOk)
        {
                close(fd);
                return false;
        }
        FILE* f = fdopen(fd, "rb");
        if (!f)
        {
                close(fd);
                return false;
        }
        struct stat st;
        if (fstat(fd, &st) != 0 || st.st_size <= 0)
        {
                fclose(f);
                return false;
        }
        auto sz = st.st_size;
        out.resize(static_cast<size_t>(sz));
        bool ok = fread(out.data(), 1, out.size(), f) == out.size();
        fclose(f);
        return ok;
}

static wasmtime_module_t* CompileWasmFile(const std::string& path, const std::string& root)
{
        std::vector<uint8_t> bytes;
        if (!ReadFileBytes(path, root, bytes))
        {
                LogError("Failed to read '%s'", path.c_str());
                return nullptr;
        }
        wasmtime_module_t* module = nullptr;
        if (auto* err = wasmtime_module_new(WasmScriptRuntime::engine(), bytes.data(), bytes.size(), &module))
        {
                LogError("Compile error in '%s': %s", path.c_str(), ExtractWasmError(err, nullptr).c_str());
                return nullptr;
        }
        return module;
}

static std::mutex g_compileJobsMutex;
static std::unordered_map<std::string, std::shared_ptr<WasmScriptRuntime::CompileJob>> g_compileJobs;

static bool HasScriptEntry(const std::vector<uint8_t>& src, std::string_view entry, std::string_view openers, bool atLineStart)
{
        auto isIdent = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
        std::string_view text(reinterpret_cast<const char*>(src.data()), src.size());
        bool lineStart = true;
        size_t i = 0;
        while (i < text.size())
        {
                if (text.compare(i, 2, "//") == 0)
                {
                        size_t end = text.find('\n', i);
                        i = end == std::string_view::npos ? text.size() : end;
                        continue;
                }
                if (text.compare(i, 2, "/*") == 0)
                {
                        size_t end = text.find("*/", i + 2);
                        i = end == std::string_view::npos ? text.size() : end + 2;
                        continue;
                }
                char c = text[i];
                if (c == '\n')
                {
                        lineStart = true;
                        ++i;
                        continue;
                }
                if (std::isspace(static_cast<unsigned char>(c)))
                {
                        ++i;
                        continue;
                }
                if ((!atLineStart || lineStart) && text.compare(i, entry.size(), entry) == 0 && (i == 0 || !isIdent(text[i - 1])))
                {
                        size_t j = i + entry.size();
                        while (j < text.size() && std::isspace(static_cast<unsigned char>(text[j])))
                                ++j;
                        if (j < text.size() && openers.find(text[j]) != std::string_view::npos)
                                return true;
                }
                lineStart = false;
                ++i;
        }
        return false;
}

static bool HasCppEntry(const std::vector<uint8_t>& src)
{
        return HasScriptEntry(src, "Server", "{", true);
}

static bool HasRustEntry(const std::vector<uint8_t>& src)
{
        return HasScriptEntry(src, "Server!", "{([", false);
}

static std::string escapeDep(const std::string& path)
{
        std::string out;
        for (char c : path)
        {
                if (c == ' ' || c == '#')
                        out += '\\';
                else if (c == '$')
                        out += '$';
                out += c;
        }
        return out;
}

static bool scanCppDeps(const std::string& cppPath, const std::string& depFile, const std::string& sdkPath, const std::string& resourceName, std::vector<std::string>& deps)
{
        std::vector<std::string> args{ "zig", "c++" };
        args.insert(args.end(), g_cppFlags.begin(), g_cppFlags.end());
        args.insert(args.end(), { "-I" + sdkPath, "-MM", "-MF", depFile, cppPath });
        auto result = fx::spawnProgram(args, 1048576, 60000);
        std::string reason;
        if (processFailed(result, reason))
        {
                unlink(depFile.c_str());
                LogError("Failed to compile '%s' in resource '%s' (%s):\n%s", cppPath.c_str(), resourceName.c_str(), reason.c_str(), result.output.c_str());
                return false;
        }
        std::string data;
        bool ok = readTextFile(depFile, data) && parseDepRule(data, deps);
        unlink(depFile.c_str());
        if (!ok)
                LogError("Failed to read dependencies of '%s' in resource '%s'", cppPath.c_str(), resourceName.c_str());
        return ok;
}

static bool ReadLeb32(const std::string& data, size_t& pos, uint32_t& out)
{
        out = 0;
        for (int shift = 0; shift < 35; shift += 7)
        {
                if (pos >= data.size())
                        return false;
                uint8_t byte = static_cast<uint8_t>(data[pos++]);
                out |= static_cast<uint32_t>(byte & 0x7F) << shift;
                if (!(byte & 0x80))
                        return true;
        }
        return false;
}

static bool StripDebugSections(const std::string& wasmPath)
{
        std::string data;
        if (!readTextFile(wasmPath, data) || data.size() < 8 || data.compare(0, 4, std::string_view("\0asm", 4)) != 0)
                return false;
        std::string out = data.substr(0, 8);
        size_t pos = 8;
        while (pos < data.size())
        {
                size_t start = pos++;
                uint32_t size;
                if (!ReadLeb32(data, pos, size) || size > data.size() - pos)
                        return false;
                size_t payload = pos;
                pos += size;
                if (data[start] == 0)
                {
                        uint32_t nameLen;
                        if (!ReadLeb32(data, payload, nameLen) || nameLen > pos - payload)
                                return false;
                        if (std::string_view(data).substr(payload, nameLen).starts_with(".debug_"))
                                continue;
                }
                out.append(data, start, pos - start);
        }
        if (out.size() == data.size())
                return true;
        std::string tmp = wasmPath + ".strip";
        FILE* f = fopen(tmp.c_str(), "wb");
        if (!f)
                return false;
        bool ok = fwrite(out.data(), 1, out.size(), f) == out.size();
        ok = fclose(f) == 0 && ok;
        if (!ok || rename(tmp.c_str(), wasmPath.c_str()) != 0)
        {
                unlink(tmp.c_str());
                return false;
        }
        return true;
}

static bool compileCpp(const std::string& cppPath, const std::string& wasmPath, const std::string& sdkPath, const std::string& resourceName, const std::string& root)
{
        std::vector<std::string> sources{ cppPath };
        std::unordered_set<std::string> seenSources{ cppPath };
        std::vector<std::string> allDeps;
        std::unordered_set<std::string> seenDeps;
        for (size_t i = 0; i < sources.size(); ++i)
        {
                std::vector<std::string> deps;
                if (!scanCppDeps(sources[i], wasmPath + ".d.tmp", sdkPath, resourceName, deps) || !DepsInside(deps, root, sdkPath, sources[i], resourceName))
                        return false;
                for (const auto& dep : deps)
                {
                        if (seenDeps.insert(dep).second)
                                allDeps.push_back(dep);
                        if (!dep.ends_with(".h") && !dep.ends_with(".hpp") && !dep.ends_with(".hh") && !dep.ends_with(".hxx"))
                                continue;
                        std::string helper = RealPath(dep.substr(0, dep.rfind('.')) + ".cpp");
                        if (helper.empty() || !helper.starts_with(root + "/") || seenSources.count(helper))
                                continue;
                        std::vector<uint8_t> source;
                        if (!ReadFileBytes(helper, root, source) || HasCppEntry(source))
                                continue;
                        seenSources.insert(helper);
                        sources.push_back(helper);
                }
        }
        std::vector<std::string> args{ "zig", "c++" };
        args.insert(args.end(), g_cppFlags.begin(), g_cppFlags.end());
        args.push_back("-I" + sdkPath);
        args.insert(args.end(), sources.begin(), sources.end());
        args.push_back("-o");
        args.push_back(wasmPath);
        auto result = fx::spawnProgram(args, 1048576, 300000);
        std::string reason;
        if (processFailed(result, reason))
        {
                LogError("Failed to compile '%s' in resource '%s' (%s):\n%s", cppPath.c_str(), resourceName.c_str(), reason.c_str(), result.output.c_str());
                return false;
        }
        if (!result.output.empty())
                LogInfo("[%s] %s", resourceName.c_str(), result.output.c_str());
        if (!StripDebugSections(wasmPath))
                LogWarning("Could not strip debug info from '%s', loading it as is", wasmPath.c_str());
        std::string depInfo = escapeDep(wasmPath) + ":";
        for (const auto& dep : allDeps)
                depInfo += " " + escapeDep(dep);
        depInfo += "\n";
        FILE* f = fopen((wasmPath + ".d").c_str(), "wb");
        if (!f || fwrite(depInfo.data(), 1, depInfo.size(), f) != depInfo.size())
        {
                if (f)
                        fclose(f);
                unlink((wasmPath + ".d").c_str());
                LogError("Failed to write dependency info for '%s' in resource '%s'", cppPath.c_str(), resourceName.c_str());
                return false;
        }
        fclose(f);
        return true;
}

static bool compileResource(const std::string& sourcePath, const std::string& wasmFile, const std::string& sdkPath, const std::string& resourceName, const std::string& root, bool rust)
{
        bool ok = rust ? compileRust(sourcePath, wasmFile, sdkPath, resourceName, root) : compileCpp(sourcePath, wasmFile, sdkPath, resourceName, root);
        if (ok)
                writeBuildStamp(wasmFile, rust);
        return ok;
}

static void RunCompileJob(std::shared_ptr<WasmScriptRuntime::CompileJob> job, std::shared_ptr<WasmScriptRuntime::CompileJob> prev, std::string sourcePath, std::string sdkPath, std::string resourceName, std::string root, bool rust)
{
        if (prev)
        {
                while (!prev->done.load(std::memory_order_acquire))
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                prev.reset();
        }
        bool ok = !job->cancelled.load(std::memory_order_relaxed) && compileResource(sourcePath, job->wasmFile, sdkPath, resourceName, root, rust);
        if (ok && !job->cancelled.load(std::memory_order_relaxed))
                job->module = CompileWasmFile(job->wasmFile, root);
        {
                std::lock_guard<std::mutex> lk(g_compileJobsMutex);
                auto it = g_compileJobs.find(job->wasmFile);
                if (it != g_compileJobs.end() && it->second == job)
                        g_compileJobs.erase(it);
        }
        job->ok = ok && job->module && !job->cancelled.load(std::memory_order_relaxed);
        job->done.store(true, std::memory_order_release);
}

void WasmScriptRuntime::drainPendingLoads()
{
        {
                std::lock_guard<std::mutex> lk(g_compileJobsMutex);
                for (auto& load : m_pendingLoads)
                {
                        while (load.job && load.job->cancelled.load(std::memory_order_relaxed) && load.job->successor)
                                load.job = load.job->successor;
                }
        }
        size_t ready = 0;
        while (ready < m_pendingLoads.size() && (!m_pendingLoads[ready].job || m_pendingLoads[ready].job->done.load(std::memory_order_acquire)))
                ++ready;
        if (ready == 0)
                return;
        std::vector<PendingLoad> loads(std::make_move_iterator(m_pendingLoads.begin()), std::make_move_iterator(m_pendingLoads.begin() + ready));
        m_pendingLoads.erase(m_pendingLoads.begin(), m_pendingLoads.begin() + ready);
        for (auto& load : loads)
        {
                if (load.job && !load.job->ok)
                        continue;
                wasmtime_module_t* module = load.job ? wasmtime_module_clone(load.job->module) : CompileWasmFile(load.wasmFile, load.root);
                if (module)
                        loadWasm(module, load.scriptFile, load.wasmFile);
        }
}

result_t OM_DECL WasmScriptRuntime::LoadFile(char* scriptFile)
{
        if (!m_host.GetRef() || !scriptFile)
                return FX_E_INVALIDARG;
        std::string_view file(scriptFile);
        std::string rootResource = m_resourceName;
        const char* relativeFile = scriptFile;
        if (file.starts_with('@'))
        {
                size_t slash = file.find('/');
                if (slash == std::string_view::npos || slash == 1 || slash + 1 == file.size())
                {
                        LogError("Invalid script path '%s' in resource '%s', expected '@resource/file'", scriptFile, m_resourceName.c_str());
                        return FX_E_INVALIDARG;
                }
                rootResource = std::string(file.substr(1, slash - 1));
                relativeFile = scriptFile + slash + 1;
        }
        std::string root = GetResourcePath(m_host.GetRef(), rootResource);
        if (!root.empty() && root.back() == '/')
                root.pop_back();
        if (root.empty())
        {
                LogError("Could not get resource path for '%s'", rootResource.c_str());
                return FX_E_INVALIDARG;
        }
        {
                fx::OMPtr<fxIStream> stream;
                if (FX_FAILED(m_host->OpenHostFile(scriptFile, stream.GetAddressOf())) || !stream.GetRef())
                {
                        LogError("Host denied access to '%s' in resource '%s'", scriptFile, m_resourceName.c_str());
                        return FX_E_INVALIDARG;
                }
        }
        std::string resolvedPath, resolvedRoot;
        if (!ValidateScriptPath(relativeFile, root, resolvedPath, resolvedRoot))
                return FX_E_INVALIDARG;
        std::string wasmFile = resolvedPath;
        if (file.ends_with(".wasm"))
        {
                std::string stem = resolvedPath.substr(0, resolvedPath.size() - 5);
                for (const char* ext : { ".cpp", ".rs" })
                {
                        if (fileExists(stem + ext))
                        {
                                LogInfo("Skipping '%s' in resource '%s', it is built from '%.*s%s'", scriptFile, m_resourceName.c_str(), static_cast<int>(file.size() - 5), file.data(), ext);
                                return FX_S_OK;
                        }
                }
        }
        if (file.ends_with(".cpp") || file.ends_with(".rs"))
        {
                std::vector<uint8_t> source;
                if (!ReadFileBytes(resolvedPath, resolvedRoot, source))
                {
                        LogError("Failed to read '%s'", resolvedPath.c_str());
                        return FX_E_INVALIDARG;
                }
                if (file.ends_with(".rs") && !HasRustEntry(source))
                {
                        LogInfo("Skipping '%s' in resource '%s', it has no Server! block and is treated as a module", scriptFile, m_resourceName.c_str());
                        return FX_S_OK;
                }
                if (file.ends_with(".cpp") && !HasCppEntry(source))
                {
                        LogInfo("Skipping '%s' in resource '%s', it has no Server block and is treated as a helper", scriptFile, m_resourceName.c_str());
                        return FX_S_OK;
                }
        }
        if (file.ends_with(".cpp") || file.ends_with(".rs"))
        {
                std::string sdkPath = getSdkPath();
                if (sdkPath.empty())
                {
                        LogError("Failed to extract embedded SDK for compilation");
                        return FX_E_INVALIDARG;
                }
                size_t extLen = file.ends_with(".cpp") ? 4 : 3;
                wasmFile = resolvedPath.substr(0, resolvedPath.size() - extLen) + ".wasm";
                auto [owner, claimed] = m_outputOwners.try_emplace(wasmFile, resolvedPath, scriptFile);
                if (!claimed && owner->second.first != resolvedPath)
                {
                        LogError("'%s' and '%s' in resource '%s' both compile to the same .wasm, rename one of them", owner->second.second.c_str(), scriptFile, m_resourceName.c_str());
                        return FX_E_INVALIDARG;
                }
                bool compiling = false;
                {
                        std::lock_guard<std::mutex> lk(g_compileJobsMutex);
                        compiling = g_compileJobs.count(wasmFile) != 0;
                }
                bool stale = compiling || !fileExists(wasmFile) || isNewer(resolvedPath, wasmFile) || depsChanged(wasmFile) || !isBuildStampCurrent(wasmFile, file.ends_with(".rs"));
                if (stale && !g_serverTicking.load(std::memory_order_relaxed))
                {
                        LogInfo("Compiling '%s' in resource '%s'...", scriptFile, m_resourceName.c_str());
                        if (!compileResource(resolvedPath, wasmFile, sdkPath, m_resourceName, resolvedRoot, file.ends_with(".rs")))
                                return FX_E_INVALIDARG;
                }
                else if (stale)
                {
                        LogInfo("Compiling '%s' in resource '%s' in the background...", scriptFile, m_resourceName.c_str());
                        std::shared_ptr<CompileJob> job;
                        {
                                std::lock_guard<std::mutex> lk(g_compileJobsMutex);
                                auto& slot = g_compileJobs[wasmFile];
                                std::shared_ptr<CompileJob> prev = slot;
                                job = std::make_shared<CompileJob>();
                                job->wasmFile = wasmFile;
                                try
                                {
                                        std::thread(RunCompileJob, job, prev, resolvedPath, sdkPath, m_resourceName, resolvedRoot, file.ends_with(".rs")).detach();
                                }
                                catch (...)
                                {
                                        if (!prev)
                                                g_compileJobs.erase(wasmFile);
                                        LogError("Failed to start compile thread for '%s' in resource '%s'", scriptFile, m_resourceName.c_str());
                                        return FX_E_INVALIDARG;
                                }
                                if (prev)
                                {
                                        prev->successor = job;
                                        prev->cancelled.store(true, std::memory_order_relaxed);
                                }
                                slot = job;
                        }
                        m_pendingLoads.push_back({ std::move(job), scriptFile, wasmFile, resolvedRoot });
                        return FX_S_OK;
                }
        }
        else if (!file.ends_with(".wasm"))
        {
                LogError("Unsupported file type for '%s' in resource '%s'", scriptFile, m_resourceName.c_str());
                return FX_E_INVALIDARG;
        }
        if (!m_pendingLoads.empty())
        {
                m_pendingLoads.push_back({ nullptr, scriptFile, wasmFile, resolvedRoot });
                return FX_S_OK;
        }
        wasmtime_module_t* module = CompileWasmFile(wasmFile, resolvedRoot);
        if (!module)
                return FX_E_INVALIDARG;
        return loadWasm(module, scriptFile, wasmFile);
}

result_t OM_DECL WasmScriptRuntime::WalkStack(char*, uint32_t, char*, uint32_t, IScriptStackWalkVisitor* visitor)
{
        if (!visitor || !m_activeModule)
                return FX_S_OK;
        const std::string& scriptFile = m_activeModule->scriptFile;
        fx::MsgpackWriter w;
        w.mapHeader(4);
        w.str("name");
        w.str(scriptFile.empty() ? "[wasm]" : scriptFile);
        w.str("file");
        w.str(m_resourceName);
        w.str("sourcefile");
        w.str(scriptFile);
        w.str("line");
        w.encInt(0);
        visitor->SubmitStackFrame(reinterpret_cast<char*>(w.buf.data()), static_cast<uint32_t>(w.buf.size()));
        return FX_S_OK;
}

result_t OM_DECL WasmScriptRuntime::RequestMemoryUsage()
{
        return FX_S_OK;
}

result_t OM_DECL WasmScriptRuntime::GetMemoryUsage(int64_t* memUsage)
{
        if (!memUsage)
                return FX_E_INVALIDARG;
        size_t total = 0;
        for (auto& mod : m_modules)
                total += mod->wasmMemSize();
        *memUsage = static_cast<int64_t>(total);
        return FX_S_OK;
}

result_t OM_DECL WasmScriptRuntime::EmitWarning(char* channel, char* message)
{
        if (message)
        {
                const char* ch = channel ? channel : "script";
                ScriptPrint(m_host.GetRef(), m_resourceName, std::string("^3[warning:") + ch + "] " + message + "^7");
        }
        return FX_S_OK;
}

void OM_DECL WasmScriptRuntime::SetupFxProfiler(void*, int32_t)
{
}

void OM_DECL WasmScriptRuntime::ShutdownFxProfiler()
{
}

FX_NEW_FACTORY(WasmScriptRuntime);
FX_IMPLEMENTS(CLSID_WasmScriptRuntime, IScriptRuntime);
FX_IMPLEMENTS(CLSID_WasmScriptRuntime, IScriptFileHandlingRuntime);
FX_IMPLEMENTS(CLSID_WasmScriptRuntime, IScriptTickRuntime);
FX_IMPLEMENTS(CLSID_WasmScriptRuntime, IScriptEventRuntime);
FX_IMPLEMENTS(CLSID_WasmScriptRuntime, IScriptRefRuntime);
FX_IMPLEMENTS(CLSID_WasmScriptRuntime, IScriptTickRuntimeWithBookmarks);
FX_IMPLEMENTS(CLSID_WasmScriptRuntime, IScriptStackWalkingRuntime);
FX_IMPLEMENTS(CLSID_WasmScriptRuntime, IScriptMemInfoRuntime);
FX_IMPLEMENTS(CLSID_WasmScriptRuntime, IScriptWarningRuntime);
FX_IMPLEMENTS(CLSID_WasmScriptRuntime, IScriptProfiler);
