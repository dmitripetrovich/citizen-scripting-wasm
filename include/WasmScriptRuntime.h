#pragma once

#include "WasmComponentHost.h"

namespace fx::cpp
{

struct CppBoundary
{
        int64_t hint;
};

class BoundaryGuard
{
        IScriptHost* m_host;
        CppBoundary m_boundary;

    public:
        BoundaryGuard(IScriptHost* host, int64_t hint) : m_host(host), m_boundary{ hint }
        {
                if (m_host)
                        m_host->SubmitBoundaryStart(reinterpret_cast<char*>(&m_boundary), sizeof(m_boundary));
        }
        ~BoundaryGuard()
        {
                if (m_host)
                        m_host->SubmitBoundaryEnd(reinterpret_cast<char*>(&m_boundary), sizeof(m_boundary));
        }
        BoundaryGuard(const BoundaryGuard&) = delete;
        BoundaryGuard& operator=(const BoundaryGuard&) = delete;
};

using WasmNativeCtx = fx::NativeCtx;

FX_DEFINE_GUID(CLSID_WasmScriptRuntime, 0xF3A7B9, 0x241D, 0x5E4C, 0x8A, 0x93, 0x2F, 0xA1, 0xB2, 0xC3, 0xD4, 0xE5);

struct WasmModule;

class WasmScriptRuntime final : public fx::OMClass<WasmScriptRuntime, IScriptRuntime, IScriptTickRuntime, IScriptEventRuntime, IScriptRefRuntime, IScriptFileHandlingRuntime, IScriptTickRuntimeWithBookmarks, IScriptStackWalkingRuntime, IScriptMemInfoRuntime, IScriptWarningRuntime, IScriptProfiler>
{
        friend struct WasmModule;

    public:
        WasmScriptRuntime();
        ~WasmScriptRuntime();
        result_t OM_DECL Create(IScriptHost* host) override;
        result_t OM_DECL Destroy() override;

        void* OM_DECL GetParentObject() override
        {
                return m_parentObject;
        }

        void OM_DECL SetParentObject(void*) override;

        int32_t OM_DECL GetInstanceId() override
        {
                return m_instanceId;
        }

        result_t OM_DECL Tick() override;
        result_t OM_DECL TickBookmarks(uint64_t* bookmarks, int32_t numBookmarks) override;
        result_t OM_DECL TriggerEvent(char* eventName, char* argsSerialized, uint32_t serializedSize, char* sourceId) override;
        result_t OM_DECL CallRef(int32_t refIdx, char* argsSerialized, uint32_t argsSize, IScriptBuffer** retval) override;
        result_t OM_DECL DuplicateRef(int32_t refIdx, int32_t* newRefIdx) override;
        result_t OM_DECL RemoveRef(int32_t refIdx) override;
        int32_t OM_DECL HandlesFile(char* scriptFile, IScriptHostWithResourceData* metadata) override;
        result_t OM_DECL LoadFile(char* scriptFile) override;
        result_t OM_DECL WalkStack(char* boundaryStart, uint32_t boundaryStartLength, char* boundaryEnd, uint32_t boundaryEndLength, IScriptStackWalkVisitor* visitor) override;
        result_t OM_DECL RequestMemoryUsage() override;
        result_t OM_DECL GetMemoryUsage(int64_t* memUsage) override;
        result_t OM_DECL EmitWarning(char* channel, char* message) override;
        void OM_DECL SetupFxProfiler(void* obj, int32_t resourceId) override;
        void OM_DECL ShutdownFxProfiler() override;
        int32_t AddFuncRef(fx::RefCallback cb);

        IScriptHost* host() const
        {
                return m_host.GetRef();
        }

        IScriptHostWithResourceData* metadataHost() const
        {
                return m_metadataHost.GetRef();
        }

        IScriptHostWithManifest* manifestHost() const
        {
                return m_manifestHost.GetRef();
        }

        const std::string& resourceName() const
        {
                return m_resourceName;
        }

        bool callInvokeRef(WasmModule& mod, uint32_t callbackId, const char* argsSerialized, uint32_t argsSize, std::vector<char>& result);

        struct WorkerState
        {
                enum Status
                {
                        Running,
                        Done,
                        Error
                };
                std::thread thread;
                std::mutex mutex;
                Status status = Running;
                std::vector<char> result;
                std::string resourceName;
                const WasmModule* owner = nullptr;
                std::atomic<bool> cancelled{ false };
                std::string stdioBuffer;
        };

        static constexpr int32_t MAX_WORKERS_PER_RESOURCE = 8;

        struct ProcessState
        {
                std::mutex mutex;
                bool done = false;
                fx::ProcessResult result;
                const WasmModule* owner = nullptr;
                std::atomic<bool> cancelled{ false };
        };

        static constexpr int32_t MAX_PROCESSES_PER_RESOURCE = 8;

        void scheduleWasmBookmark(WasmModule& mod, int32_t wasmId, int32_t deadlineMs);
        static wasm_engine_t* engine();

        std::unordered_map<int32_t, std::shared_ptr<WorkerState>> m_workers;
        int32_t m_nextWorkerId = 1;
        std::unordered_map<int32_t, std::shared_ptr<ProcessState>> m_processes;
        int32_t m_nextProcessId = 1;
        struct WasmRef
        {
                WasmModule* mod = nullptr;
                int32_t callbackId = 0;
        };
        std::unordered_map<int32_t, WasmRef> m_refToCallbackId;
        struct RefGuard
        {
                std::recursive_mutex mu;
                bool alive = true;
                WasmScriptRuntime* rt = nullptr;
        };
        std::shared_ptr<RefGuard> m_refGuard = std::make_shared<RefGuard>();

    private:
        fx::OMPtr<IScriptHost> m_host;
        fx::OMPtr<IScriptHostWithBookmarks> m_bookmarkHost;
        fx::OMPtr<IScriptHostWithResourceData> m_metadataHost;
        fx::OMPtr<IScriptHostWithManifest> m_manifestHost;
        void* m_parentObject = nullptr;
        int32_t m_instanceId = 0;
        bool m_destroyed = false;
        std::string m_resourceName;
        WasmModule* m_activeModule = nullptr;
        std::unordered_map<int32_t, fx::RefCallback> m_refs;
        int32_t m_nextRefIdx = 1;
        int32_t allocRefIdx();
        uint64_t m_nextBoundaryId = 1;
        uint64_t nextBoundaryId();
        std::vector<std::shared_ptr<WasmModule>> m_modules;
        uint32_t m_nextBookmarkSlot = 1;

    public:
        struct CompileJob
        {
                std::string wasmFile;
                bool ok = false;
                std::atomic<bool> done{ false };
                std::atomic<bool> cancelled{ false };
                std::shared_ptr<CompileJob> successor;
                wasmtime_module_t* module = nullptr;

                ~CompileJob()
                {
                        if (module)
                                wasmtime_module_delete(module);
                }
        };

    private:
        struct PendingLoad
        {
                std::shared_ptr<CompileJob> job;
                std::string scriptFile;
                std::string wasmFile;
                std::string root;
        };
        std::vector<PendingLoad> m_pendingLoads;
        struct MissedEvent
        {
                std::string name;
                std::vector<char> args;
                std::string source;
        };
        std::vector<MissedEvent> m_missedStartEvents;
        std::unordered_map<std::string, std::pair<std::string, std::string>> m_outputOwners;
        void drainPendingLoads();
        void dispatchEvent(const std::vector<std::shared_ptr<WasmModule>>& targets, std::string_view name, const char* argsSerialized, uint32_t serializedSize, std::string_view src);
        void defineImports(WasmModule& mod);
        bool resolveExports(WasmModule& mod);
        void destroyModule(WasmModule& mod);
        void destroyWorkers();
        result_t loadWasm(wasmtime_module_t* module, const std::string& scriptFile, const std::string& sourcePath);
};

struct WasmModule : std::enable_shared_from_this<WasmModule>
{
        WasmScriptRuntime* rt = nullptr;
        std::string scriptFile;
        wasmtime_store_t* store = nullptr;
        wasmtime_module_t* module = nullptr;
        wasmtime_linker_t* linker = nullptr;
        wasmtime_instance_t instance{ };
        wasmtime_memory_t memory{ };
        bool hasMemory = false;
        wasmtime_func_t fnTick{ };
        wasmtime_func_t fnEvent{ };
        wasmtime_func_t fnStop{ };
        wasmtime_func_t fnAlloc{ };
        wasmtime_func_t fnFree{ };
        wasmtime_func_t fnInvokeRef{ };
        wasmtime_func_t fnDuplicateRef{ };
        wasmtime_func_t fnRemoveRef{ };
        wasmtime_func_t fnHasPendingWork{ };
        wasmtime_func_t fnTickBookmarks{ };
        bool hasTickFn = false;
        bool hasEventFn = false;
        bool hasStopFn = false;
        bool hasAllocFn = false;
        bool hasFreeFn = false;
        bool hasInvokeRefFn = false;
        bool hasDuplicateRefFn = false;
        bool hasRemoveRefFn = false;
        bool hasHasPendingWorkFn = false;
        bool hasTickBookmarksFn = false;
        bool hasValidNativeResult = false;
        fxNativeContext lastNativeCtx{ };
        std::string_view lastResultKinds;
        int32_t callDepth = 0;
        bool limitExceeded = false;
        std::string lastNativeError;
        std::string stdioBuffer;
        uint32_t bookmarkSlot = 0;

        uint8_t* wasmBase();
        size_t wasmMemSize();
        uint32_t wasmAlloc(uint32_t size);
        void wasmFree(uint32_t ptr, uint32_t size);
        void resetLimits();
        bool call(const wasmtime_func_t& fn, const wasmtime_val_t* args, size_t nargs, wasmtime_val_t* results, size_t nresults, const char* label);
        bool callVoid(const wasmtime_func_t& fn);
        bool callEvent(uint32_t namePtr, uint32_t nameLen, uint32_t argsPtr, uint32_t argsLen, uint32_t srcPtr, uint32_t srcLen);
        void duplicateRef(int32_t callbackId);
        void removeRef(int32_t callbackId);
};

}
