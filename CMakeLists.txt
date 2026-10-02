cmake_minimum_required(VERSION 3.20)
project(LumaBridge LANGUAGES CXX VERSION 0.36.0)

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
    src/core src/integrations/razer src/integrations/corsair src/integrations/alienware src/integrations/steelseries src/app src/app/games src/app/sensors src/app/peripherals src/hardware/aura-usb)
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
    src/core/config.cpp
    src/core/log.cpp
    src/core/module_bootstrap.cpp)
target_include_directories(luma_common PUBLIC src/core)

# Direct USB access to the motherboard's Aura controller (the default way to reach the
# hardware; the Aura SDK reports no devices on current Armoury Crate versions).
add_library(aura_usb STATIC src/hardware/aura-usb/aura_usb.cpp src/hardware/aura-usb/usb_aura.cpp)
target_include_directories(aura_usb PUBLIC src/hardware/aura-usb)
target_link_libraries(aura_usb PUBLIC luma_common hid setupapi)

add_library(aura_bridge STATIC
    src/hardware/aura-sdk/aura_bridge.cpp
    src/hardware/aura_mirror.cpp)
target_include_directories(aura_bridge PUBLIC src/hardware src/hardware/aura-sdk)
target_link_libraries(aura_bridge PUBLIC luma_common aura_usb ole32 oleaut32 uuid)

# ---- proxy DLL -----------------------------------------------------------------------
add_library(lumabridge_proxy SHARED
    src/integrations/logitech/dllmain.cpp
    src/integrations/logitech/exports.cpp
    src/integrations/logitech/real_logiled.cpp
    src/integrations/logitech/exports.def)
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

luma_emulator(chroma_emu integrations/razer RzChromaSDK64 RzChromaSDK
    src/integrations/razer/dllmain.cpp src/integrations/razer/chroma_exports.cpp src/integrations/razer/chroma_exports.def)
# CUE SDK 3.x (VS2019 toolset) names; the install script copies it over whichever CUESDK*.dll
# a game ships.
luma_emulator(corsair_emu integrations/corsair CUESDK.x64_2019 CUESDK_2019
    src/integrations/corsair/dllmain.cpp src/integrations/corsair/corsair_exports.cpp src/integrations/corsair/corsair_exports.def)
luma_emulator(lightfx_emu integrations/alienware LightFX LightFX
    src/integrations/alienware/dllmain.cpp src/integrations/alienware/lightfx_exports.cpp src/integrations/alienware/lightfx_exports.def)

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

    add_library(gamesense STATIC src/integrations/steelseries/gamesense_server.cpp src/integrations/steelseries/gg_forwarder.cpp)
    target_include_directories(gamesense PUBLIC src/integrations/steelseries)
    target_link_libraries(gamesense PUBLIC luma_common ws2_32 winhttp)

    add_executable(lumabridge_app WIN32
        src/app/main.cpp
        src/app/ui.cpp
        src/app/scene_gpu.cpp
        src/app/controller.cpp
        src/app/app_settings.cpp
        src/app/integrations.cpp
        src/app/armoury_crate.cpp
        src/app/game_detector.cpp
        src/app/game_library.cpp
        src/app/game_feeds.cpp
        src/app/displays.cpp
        src/app/peripherals/nzxt_kraken.cpp
        src/app/screen_capture.cpp
        src/app/vendor_detect.cpp
        src/app/device_inventory.cpp
        src/app/sensors/system_monitor.cpp
        src/app/peripherals/logitech_output.cpp
        src/app/peripherals/azoth_output.cpp src/app/peripherals/dualsense_output.cpp src/app/peripherals/pad_input.cpp src/app/peripherals/hardware_helper.cpp
        src/app/peripherals/azoth_oled_assets.cpp
        src/app/peripherals/openrgb_output.cpp src/app/peripherals/lamparray_output.cpp
        src/app/peripherals/ghub_settings.cpp
        src/integrations/logitech/real_logiled.cpp
        src/app/res/app.rc)
    target_include_directories(lumabridge_app PRIVATE src/app src/app/games src/app/sensors src/app/peripherals
        src/integrations/logitech)
    target_compile_definitions(lumabridge_app PRIVATE LUMA_VERSION="${PROJECT_VERSION}")
    target_link_libraries(lumabridge_app PRIVATE imgui aura_bridge gamesense luma_common shell32 ole32 advapi32 dbghelp psapi version
                          winhttp ws2_32 d3d11 d3dcompiler dxgi hid setupapi cfgmgr32 pdh gdi32 windowscodecs)
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


add_executable(aura-usb-test tools/aura-usb-test/main.cpp)
target_link_libraries(aura-usb-test PRIVATE aura_usb)

# Read-only SMBus probe for RAM lighting controllers (needs the PawnIO driver at runtime;
# PawnIOLib.dll is loaded dynamically, nothing to link).
add_executable(ram-probe tools/ram-probe/main.cpp)

# The elevated hardware helper: RAM lighting and sensors for the app, through the bundled
# PawnIO driver (runs as a scheduled task; see src/helper/main.cpp and
# scripts/Install-Helper.ps1).
add_executable(hw_helper src/helper/main.cpp)
target_include_directories(hw_helper PRIVATE src/app/peripherals src/app/sensors src/core)
target_link_libraries(hw_helper PRIVATE advapi32)
set_target_properties(hw_helper PROPERTIES OUTPUT_NAME "LumaBridge-Helper")

# Read-only survey of keyboard / mouse HID interfaces (LampArray, vendor pages).
add_executable(device-probe tools/device-probe/main.cpp)
target_link_libraries(device-probe PRIVATE hid setupapi)
add_executable(azoth-probe tools/azoth-probe/main.cpp)
target_link_libraries(azoth-probe PRIVATE hid setupapi)
target_include_directories(azoth-probe PRIVATE src/core)
add_executable(hidpp-probe tools/hidpp-probe/main.cpp)
target_link_libraries(hidpp-probe PRIVATE hid setupapi)

# Read-only probe of G HUB's local WebSocket (does it report game lighting?).
add_executable(ghub-probe tools/ghub-probe/main.cpp)
target_link_libraries(ghub-probe PRIVATE winhttp iphlpapi ws2_32)
# Chroma Broadcast probe: can Razer Synapse pass games' Chroma lighting to LumaBridge?
add_executable(chroma-probe tools/chroma-probe/main.cpp)
target_include_directories(chroma-probe PRIVATE src/integrations/razer)
target_link_libraries(chroma-probe PRIVATE winhttp version)

if(MINGW)
    # wmain entry points
    target_link_options(aura-test PRIVATE -municode)
    target_link_options(logiled-harness PRIVATE -municode)
    target_link_options(aura-usb-test PRIVATE -municode)
    target_link_options(ram-probe PRIVATE -municode)
    target_link_options(hw_helper PRIVATE -municode)
    target_link_options(ghub-probe PRIVATE -municode)
    target_link_options(chroma-probe PRIVATE -municode)
    target_link_options(device-probe PRIVATE -municode)
    target_link_options(hidpp-probe PRIVATE -municode)
    target_link_options(azoth-probe PRIVATE -municode)
endif()

# Release layout:
#   LumaBridge.exe, LumaBridge.ini.example     the app (x64 build only)
#   integrations\                              game SDK stand-ins (x86\ added by the release job)
#   tools\                                     hardware tests
#   scripts\                                   install helpers the app runs
install(TARGETS lumabridge_proxy chroma_emu corsair_emu lightfx_emu RUNTIME DESTINATION integrations)
install(TARGETS hw_helper RUNTIME DESTINATION .)
# PawnIO, bundled unmodified (third_party/pawnio/README.md): installed by Install-Helper.ps1.
install(FILES third_party/pawnio/PawnIO_setup.exe third_party/pawnio/COPYING third_party/pawnio/README.md DESTINATION pawnio)
install(FILES third_party/pawnio/modules/SmbusPIIX4.bin third_party/pawnio/modules/SmbusI801.bin third_party/pawnio/modules/LpcIO.bin
        third_party/pawnio/modules/AMDFamily17.bin third_party/pawnio/modules/IntelMSR.bin
        third_party/pawnio/modules/COPYING DESTINATION pawnio/modules)
install(TARGETS aura-test aura-usb-test ram-probe ghub-probe chroma-probe device-probe hidpp-probe azoth-probe logiled-harness RUNTIME DESTINATION tools)
install(FILES scripts/Run-HardwareTest.ps1 DESTINATION tools)
install(FILES scripts/Install-LogiLedProxy.ps1 scripts/Install-SdkEmulators.ps1 scripts/Install-HandbackTask.ps1 scripts/Install-Helper.ps1 scripts/Write-GameFile.ps1
        DESTINATION scripts)
install(FILES config/LumaBridge.ini.example DESTINATION .)
