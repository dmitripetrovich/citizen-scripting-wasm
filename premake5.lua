local component = dofile("component.lua")

workspace "citizen-scripting-wasm"
        configurations { "Debug", "Release" }
        architecture "x86_64"
        language "C++"
        cppdialect "C++23"
        location "build"

project "citizen-scripting-wasm"
        kind "SharedLib"
        targetname "citizen-scripting-wasm"
        targetprefix "lib"
        files {
                "src/WasmComponentHost.cpp",
                "src/WasmScriptRuntime.cpp",
                "src/Component.cpp",
        }
        includedirs { ".", "include" }
        component()
        filter "configurations:Debug"
                symbols "On"
                optimize "Off"
        filter "configurations:Release"
                optimize "On"
        filter {}
        makesettings [[
                .DEFAULT_GOAL := all
                SDK_SOURCES = ../include/cfx.h ../include/cfx.rs ../src/natives/all.h ../src/natives/all.rs
                ../src/sdk.blob: $(SDK_SOURCES) ; @echo Embedding SDK && python3 ../tools/native_db.py --embed-only --embed $@
                obj/Debug/WasmScriptRuntime.o obj/Release/WasmScriptRuntime.o: ../src/sdk.blob
        ]]
        postbuildcommands {
                '{COPYFILE} "%{wks.location}/../component.json" "%{cfg.buildtarget.directory}/libcitizen-scripting-wasm.json"'
        }
