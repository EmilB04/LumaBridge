cmake_minimum_required(VERSION 3.20)
project(LumaBridge LANGUAGES CXX VERSION 0.1.0)

if(NOT WIN32)
    message(STATUS "Non-Windows host: only the portable core tests will be built.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# x64 or x86 decides which DLL name the proxy stands in for (BF1 is x64-only).
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(LUMA_ARCH x64)
else()
    set(LUMA_ARCH x86)
endif()

if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
    # Static CRT: the proxy gets loaded into arbitrary game processes; don't depend on
    # whichever VC++ redistributable happens to be installed.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
    add_compile_options(-Wall -Wextra)
endif()

# ---- Portable core tests (any OS) ----------------------------------------------------
enable_testing()
add_executable(test_core tests/test_core.cpp)
target_include_directories(test_core PRIVATE src/common src/chroma-emu)
add_test(NAME test_core COMMAND test_core)

if(NOT WIN32)
    return()
endif()

add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)

if(MINGW)
    # Self-contained binaries for cross-compiled test builds.
    add_link_options(-static -static-libgcc -static-libstdc++)
endif()

# ---- common + aura-bridge ------------------------------------------------------------
add_library(luma_common STATIC
    src/common/config.cpp
    src/common/log.cpp)
target_include_directories(luma_common PUBLIC src/common)

add_library(aura_bridge STATIC
    src/aura-bridge/aura_bridge.cpp
    src/aura-bridge/aura_mirror.cpp)
target_include_directories(aura_bridge PUBLIC src/aura-bridge)
target_link_libraries(aura_bridge PUBLIC luma_common ole32 oleaut32 uuid)

# ---- proxy DLL -----------------------------------------------------------------------
add_library(lumabridge_proxy SHARED
    src/proxy-dll/dllmain.cpp
    src/proxy-dll/exports.cpp
    src/proxy-dll/real_logiled.cpp
    src/proxy-dll/exports.def)
target_link_libraries(lumabridge_proxy PRIVATE aura_bridge luma_common advapi32)
set_target_properties(lumabridge_proxy PROPERTIES
    OUTPUT_NAME "LumaBridge_${LUMA_ARCH}"
    PREFIX "")
if(MINGW)
    # Undecorated export names on x86 (MSVC gets this from exports.def alone).
    target_link_options(lumabridge_proxy PRIVATE -Wl,--kill-at)
endif()

# ---- Razer Chroma emulator DLL -------------------------------------------------------
add_library(chroma_emu SHARED
    src/chroma-emu/dllmain.cpp
    src/chroma-emu/chroma_exports.cpp
    src/chroma-emu/chroma_exports.def)
target_link_libraries(chroma_emu PRIVATE aura_bridge luma_common)
# Chroma games load RzChromaSDK64.dll (x64) / RzChromaSDK.dll (x86) by name.
if(LUMA_ARCH STREQUAL "x64")
    set_target_properties(chroma_emu PROPERTIES OUTPUT_NAME "RzChromaSDK64" PREFIX "")
else()
    set_target_properties(chroma_emu PROPERTIES OUTPUT_NAME "RzChromaSDK" PREFIX "")
endif()
if(MINGW)
    target_link_options(chroma_emu PRIVATE -Wl,--kill-at)
endif()

# ---- tools ---------------------------------------------------------------------------
add_executable(aura-test tools/aura-test/main.cpp)
target_link_libraries(aura-test PRIVATE aura_bridge)

add_executable(logiled-harness tools/logiled-harness/main.cpp)

if(MINGW)
    # wmain entry points
    target_link_options(aura-test PRIVATE -municode)
    target_link_options(logiled-harness PRIVATE -municode)
endif()

install(TARGETS lumabridge_proxy chroma_emu aura-test logiled-harness RUNTIME DESTINATION .)
install(FILES config/LumaBridge.ini.example DESTINATION .)
