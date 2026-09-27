cmake_minimum_required(VERSION 3.20)
project(LumaBridge LANGUAGES CXX VERSION 0.2.4)

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
target_include_directories(test_core PRIVATE
    src/common src/chroma-emu src/corsair-emu src/lightfx-emu src/gamesense src/app src/aura-usb)
add_test(NAME test_core COMMAND test_core)

if(NOT WIN32)
    return()
endif()

enable_language(RC)
add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)

if(MINGW)
    # Self-contained binaries for cross-compiled test builds.
    add_link_options(-static -static-libgcc -static-libstdc++)
endif()

# ---- common + aura-bridge ------------------------------------------------------------
add_library(luma_common STATIC
    src/common/config.cpp
    src/common/log.cpp
    src/common/module_bootstrap.cpp)
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

# ---- SDK emulator DLLs ---------------------------------------------------------------
# Each is named exactly like the vendor runtime games load.
function(luma_emulator target dir output_x64 output_x86)
    add_library(${target} SHARED ${ARGN})
    target_include_directories(${target} PRIVATE src/${dir})
    target_link_libraries(${target} PRIVATE aura_bridge luma_common)
    if(LUMA_ARCH STREQUAL "x64")
        set_target_properties(${target} PROPERTIES OUTPUT_NAME "${output_x64}" PREFIX "")
    else()
        set_target_properties(${target} PROPERTIES OUTPUT_NAME "${output_x86}" PREFIX "")
    endif()
    if(MINGW)
        target_link_options(${target} PRIVATE -Wl,--kill-at)
    endif()
endfunction()

luma_emulator(chroma_emu chroma-emu RzChromaSDK64 RzChromaSDK
    src/chroma-emu/dllmain.cpp src/chroma-emu/chroma_exports.cpp src/chroma-emu/chroma_exports.def)
# CUE SDK 3.x (VS2019 toolset) names; the install script copies it over whichever CUESDK*.dll
# a game ships.
luma_emulator(corsair_emu corsair-emu CUESDK.x64_2019 CUESDK_2019
    src/corsair-emu/dllmain.cpp src/corsair-emu/corsair_exports.cpp src/corsair-emu/corsair_exports.def)
luma_emulator(lightfx_emu lightfx-emu LightFX LightFX
    src/lightfx-emu/dllmain.cpp src/lightfx-emu/lightfx_exports.cpp src/lightfx-emu/lightfx_exports.def)

# ---- LumaBridge app (x64 only: it's a normal desktop app) ----------------------------
if(LUMA_ARCH STREQUAL "x64")
    include(FetchContent)
    # Dear ImGui (MIT), pinned. Set FETCHCONTENT_SOURCE_DIR_IMGUI to use a local copy offline.
    FetchContent_Declare(imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG v1.91.9
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(imgui)

    add_library(imgui STATIC
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp)
    target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
    target_link_libraries(imgui PUBLIC d3d11 d3dcompiler dxgi dwmapi)
    if(MSVC)
        target_compile_options(imgui PRIVATE /W0)
    else()
        target_compile_options(imgui PRIVATE -w)
    endif()

    add_library(gamesense STATIC src/gamesense/gamesense_server.cpp src/gamesense/gg_forwarder.cpp)
    target_include_directories(gamesense PUBLIC src/gamesense)
    target_link_libraries(gamesense PUBLIC luma_common ws2_32 winhttp)

    add_executable(lumabridge_app WIN32
        src/app/main.cpp
        src/app/ui.cpp
        src/app/controller.cpp
        src/app/app_settings.cpp
        src/app/integrations.cpp
        src/app/res/app.rc)
    target_include_directories(lumabridge_app PRIVATE src/app)
    target_link_libraries(lumabridge_app PRIVATE imgui aura_bridge gamesense luma_common shell32 ole32 advapi32 dbghelp)
    set_target_properties(lumabridge_app PROPERTIES OUTPUT_NAME "LumaBridge")
    if(MSVC)
        target_link_options(lumabridge_app PRIVATE /MANIFEST:NO)  # app.rc embeds our manifest
    else()
        target_link_options(lumabridge_app PRIVATE -municode)
    endif()
    install(TARGETS lumabridge_app RUNTIME DESTINATION .)
endif()

# ---- tools ---------------------------------------------------------------------------
add_executable(aura-test tools/aura-test/main.cpp)
target_link_libraries(aura-test PRIVATE aura_bridge)

add_executable(logiled-harness tools/logiled-harness/main.cpp)

# Direct USB access to the motherboard's Aura controller (replacement for the Aura SDK,
# which reports no devices on current Armoury Crate versions).
add_library(aura_usb STATIC src/aura-usb/aura_usb.cpp)
target_include_directories(aura_usb PUBLIC src/aura-usb)
target_link_libraries(aura_usb PUBLIC luma_common hid setupapi)

add_executable(aura-usb-test tools/aura-usb-test/main.cpp)
target_link_libraries(aura-usb-test PRIVATE aura_usb)

if(MINGW)
    # wmain entry points
    target_link_options(aura-test PRIVATE -municode)
    target_link_options(logiled-harness PRIVATE -municode)
    target_link_options(aura-usb-test PRIVATE -municode)
endif()

install(TARGETS lumabridge_proxy chroma_emu corsair_emu lightfx_emu aura-test aura-usb-test logiled-harness
        RUNTIME DESTINATION .)
install(FILES config/LumaBridge.ini.example DESTINATION .)
install(FILES scripts/Install-LogiLedProxy.ps1 scripts/Install-SdkEmulators.ps1 scripts/Run-HardwareTest.ps1
        DESTINATION .)
