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
                "src/CppComponentHost.cpp",
                "src/CppScriptRuntime.cpp",
                "src/Component.cpp",
        }
        includedirs { ".", "include" }
        component()
        filter "configurations:Debug"
                symbols "On"
                optimize "Off"
        filter "configurations:Release"
                optimize "On"
