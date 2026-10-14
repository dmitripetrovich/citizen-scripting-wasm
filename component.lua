local cwd = os.getcwd()
local wasmtime_dir = ("%s/vendor/wasmtime"):format(cwd)

if not os.isfile(("%s/lib/libwasmtime.a"):format(wasmtime_dir)) then
        os.execute("tools/ext/wasmtime")
end

if not os.isdir(("%s/src/natives"):format(cwd)) then
        os.execute("python3 tools/native_db.py")
end

return function()
        filter {}
        includedirs { ("%s/include"):format(wasmtime_dir) }
        linkoptions { ("%s/lib/libwasmtime.a"):format(wasmtime_dir), "-lpthread", "-ldl", "-lm" }
        makesettings [[
                CC = zig cc -target x86_64-linux-musl
                CXX = zig c++ -target x86_64-linux-musl
        ]]
end
