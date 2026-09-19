cmake_minimum_required(VERSION 3.20)

project(PS2Runtime VERSION 0.1.0 LANGUAGES CXX)

# [R1] DORMANT experiment: route the GS replay through the renamed vendored rlgl copy (gsrl) instead of
# the vendored raylib rlgl. OFF by default -- with raylib vendored 1:1 the replay already submits through
# rlgl that lives in our tree, with a single GL state owner. Kept for A/B comparison.
option(PS2X_USE_GSRL "Route the GS replay through the renamed gsrl copy (experiment)" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT DEFINED PS2X_TARGET_PROCESSOR)
    set(PS2X_TARGET_PROCESSOR "${CMAKE_SYSTEM_PROCESSOR}")
endif()

option(PS2X_ENABLE_RUNNER_UNITY_BUILD "Build ps2EntryRunner with CMake unity build" ON)
set(PS2X_RUNNER_UNITY_BUILD_BATCH_SIZE 8 CACHE STRING "Unity build batch size for ps2EntryRunner")
option(PS2X_ENABLE_SCCACHE "Use sccache as compiler launcher when available" ON)

option(PS2X_ENABLE_RUNTIME_LOGS "Enable PS2 runtime logs" OFF)
option(PS2X_ENABLE_AGRESSIVE_LOGS "Enable very verbose/agressive PS2 runtime logs" OFF)
option(PS2X_STRICT_RETURN_DIAGNOSTICS "Route generated JR $ra returns through runtime branch diagnostics" OFF)
option(PS2X_SHOW_WINDOWS_CONSOLE "Show a console window for ps2EntryRunner on Windows release builds" ON)

if(PS2X_ENABLE_SCCACHE)
    find_program(PS2X_SCCACHE_PROGRAM sccache)

    if(PS2X_SCCACHE_PROGRAM)
        if(CMAKE_C_COMPILER)
            set(CMAKE_C_COMPILER_LAUNCHER "${PS2X_SCCACHE_PROGRAM}")
        endif()

        set(CMAKE_CXX_COMPILER_LAUNCHER "${PS2X_SCCACHE_PROGRAM}")
        message(STATUS "Using sccache: ${PS2X_SCCACHE_PROGRAM}")
    else()
        message(STATUS "sccache not found; continuing without compiler launcher")
    endif()
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/ReleaseMode.cmake")

set(PS2X_IS_VITA OFF)

if((CMAKE_C_COMPILER MATCHES "arm-vita-eabi") OR
    (CMAKE_CXX_COMPILER MATCHES "arm-vita-eabi"))
    set(PS2X_IS_VITA ON)
endif()

option(PS2X_VITA_CREATE_PACKAGE "Create Vita eboot/vpk targets" ON)
set(PS2X_VITA_APP_NAME "PS2 Retro X" CACHE STRING "Display name for the Vita bubble")
set(PS2X_VITA_TITLEID "RANJ00001" CACHE STRING "9-character Vita title id")
set(PS2X_VITA_VERSION "01.00" CACHE STRING "Vita app version")
set(PS2X_DEFAULT_BOOT_ELF "" CACHE STRING "Guest ELF path passed directly to main() when argv is unavailable")

if(PS2X_IS_VITA)
    if(NOT DEFINED ENV{VITASDK})
        message(FATAL_ERROR "VITASDK is not defined. Configure inside the VitaSDK environment or pass the Vita toolchain file.")
    endif()

    set(PS2X_VITASDK "$ENV{VITASDK}")

    if(NOT EXISTS "${PS2X_VITASDK}/share/vita.cmake")
        message(FATAL_ERROR "Could not find vita.cmake under ${PS2X_VITASDK}/share.")
    endif()

    include("${PS2X_VITASDK}/share/vita.cmake" REQUIRED)
else()
    include(FetchContent)
    set(FETCHCONTENT_QUIET FALSE)
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BUILD_GAMES OFF CACHE BOOL "" FORCE)

    # [vendor] raylib is VENDORED 1:1 at third_party/bt3gl (see VENDORED.md): no FetchContent, no
    # patch step (the copy already carries patches/raylib-5.5-ps2x.patch, and its config.h already has
    # MAX_GAMEPADS=16) and no network at configure time. The target is named `bt3gl` (our vendored raylib),
    # so every link and ${bt3gl_SOURCE_DIR} use below is local and patch-free.
    set(bt3gl_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/bt3gl")

    # Only what the runtime uses: rcore.c also carries rlgl's implementation (upstream 5.5 layout).
    # rtext.c (fonts: DrawText/TextFormat/GetFontDefault, used by the overlay/UI) and raudio.c (the
    # PS2X_HOSTAUDIO=raylib fallback) are needed as well; rmodels.c is the only module left out.
    add_library(bt3gl STATIC
        "${bt3gl_SOURCE_DIR}/src/rcore.c"
        "${bt3gl_SOURCE_DIR}/src/rshapes.c"
        "${bt3gl_SOURCE_DIR}/src/rtextures.c"
        "${bt3gl_SOURCE_DIR}/src/rtext.c"
        "${bt3gl_SOURCE_DIR}/src/raudio.c"
        "${bt3gl_SOURCE_DIR}/src/utils.c")
    # ONLY src: the vendor's own includes are relative ("external/glad.h", "external/stb_image.h", ...)
    # so they resolve on their own. Putting src/external on the include path makes the Windows-only
    # external/dirent.h SHADOW the real <dirent.h> (angle includes search -I/-isystem dirs first) and the
    # Linux build dies on its '#include <io.h>'. ps2_runtime adds that dir separately, for stb.
    target_include_directories(bt3gl PUBLIC "${bt3gl_SOURCE_DIR}/src")
    # [B] The window/input/GL-context come from SDL2 (platforms/rcore_desktop_sdl.c), not GLFW: GLFW is
    # gone from the build entirely. SDL2 is fetched and built further down for hostio; the window uses
    # the same static build (linked after that block, see PS2X_SDL2_TARGET).
    target_compile_definitions(bt3gl PUBLIC PLATFORM_DESKTOP_SDL GRAPHICS_API_OPENGL_33)

    if(WIN32)
        target_link_libraries(bt3gl PUBLIC opengl32 winmm)
    else()
        find_package(OpenGL REQUIRED)
        target_link_libraries(bt3gl PUBLIC OpenGL::GL ${CMAKE_DL_LIBS} pthread m)
    endif()


    FetchContent_Declare(
        imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG "docking"
        GIT_SHALLOW TRUE
    )
    FetchContent_GetProperties(imgui)

    if(NOT imgui_POPULATED)
        FetchContent_Populate(imgui)
    endif()

    set(PS2X_IMGUI_SOURCE_DIR "${imgui_SOURCE_DIR}")

    # Declared BEFORE its first use below: with the option only defined further down (next to the SDL2
    # fetch) the variable was empty on a FRESH configure, so imgui_impl_sdl2.cpp was silently left out
    # and the link failed with undefined ImGui_ImplSDL2_* symbols. Cached builds hid it.
    option(PS2X_HOST_SDL2 "Use SDL2 for gamepad input and audio output" ON)

    add_library(imgui STATIC
        "${PS2X_IMGUI_SOURCE_DIR}/imgui.cpp"
        "${PS2X_IMGUI_SOURCE_DIR}/imgui_draw.cpp"
        "${PS2X_IMGUI_SOURCE_DIR}/imgui_tables.cpp"
        "${PS2X_IMGUI_SOURCE_DIR}/imgui_widgets.cpp"
        "${PS2X_IMGUI_SOURCE_DIR}/imgui_demo.cpp"
    )
    target_include_directories(imgui PUBLIC "${PS2X_IMGUI_SOURCE_DIR}")
    # [altGL] ImGui's OpenGL3 backend, so the overlay can render through gfx::gl / raw GL instead
    # of rlImGui/rlgl (PS2X_UIGL=1). It brings its own GL loader, so it does not need rlgl.
    target_sources(imgui PRIVATE "${PS2X_IMGUI_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp")
    target_include_directories(imgui PUBLIC "${PS2X_IMGUI_SOURCE_DIR}/backends")

    # [d3d11] ImGui's Direct3D 11 backend, so the overlay can render through the native
    # video path instead of rlImGui/rlgl when PS2X_D3D11=1 is active.
    if(WIN32)
        target_sources(imgui PRIVATE "${PS2X_IMGUI_SOURCE_DIR}/backends/imgui_impl_dx11.cpp")
        target_link_libraries(imgui PUBLIC d3d11 dxgi)
    endif()

    # [C] ImGui's SDL2 PLATFORM backend: the overlay's window/input glue. It replaces rlImGui (raylib's
    # ecosystem) while the renderers stay ours -- imgui_impl_opengl3 for gfx::gl and imgui_impl_dx11 for
    # the native path. SDL event WATCHING (see ui_sdl.cpp) lets ImGui see every event without consuming
    # it, so raylib keeps its own input. The SDL2 link is added after the SDL2 target exists, below.
    # [linuxfix] This block used to sit INSIDE if(WIN32), so Linux never got the platform backend.
    # (The PS2X_HOST_SDL2 option() is declared above, before its first use.)
    if(PS2X_HOST_SDL2)
        target_sources(imgui PRIVATE "${PS2X_IMGUI_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp")
        # [sdlprefetch] clang-cl: SDL_endian.h defines _m_prefetch, a builtin on current clang ("definition
        # of builtin function"). Our files include SDL via ps2_host_sdl.h, which sets clang's guard first;
        # imgui's SDL2 backend and bt3gl's SDL platform (rcore.c) include <SDL.h> directly, so force the
        # same guard into those two. Out of the unity build so the force-include stays on that one file.
        if(MSVC AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            set(_ps2x_sdl_prefetch_fix "${CMAKE_CURRENT_SOURCE_DIR}/src/lib/ps2_sdl_prefetch_fix.h")
            set_source_files_properties(
                "${PS2X_IMGUI_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp"
                "${bt3gl_SOURCE_DIR}/src/rcore.c"
                PROPERTIES COMPILE_OPTIONS "/FI${_ps2x_sdl_prefetch_fix}" SKIP_UNITY_BUILD_INCLUSION ON)
        endif()
    endif()

    # [C] rlImGui is GONE: the overlay is ImGui over our own backends (imgui_impl_sdl2 for the platform
    # and imgui_impl_opengl3 / imgui_impl_dx11 to draw), so raylib's ecosystem is no longer a build
    # dependency. See src/lib/ui_sdl.cpp for the event-watch trick that keeps raylib's input working.

    # [hostio] SDL2 for gamepads and audio output (the window and keyboard stay raylib's). Built
    # static from source so every platform gets the same version; PS2X_HOSTPAD=raylib /
    # PS2X_HOSTAUDIO=raylib switch back at runtime, -DPS2X_HOST_SDL2=OFF leaves it out entirely.
    # (The option itself is declared earlier, before the imgui target uses it.)
    if(PS2X_HOST_SDL2)
        set(SDL_TEST OFF CACHE BOOL "" FORCE)
        set(SDL_TESTS OFF CACHE BOOL "" FORCE)
        set(SDL_SHARED OFF CACHE BOOL "" FORCE)
        set(SDL_STATIC ON CACHE BOOL "" FORCE)
        set(SDL_RENDER OFF CACHE BOOL "" FORCE)   # not used; no GL/renderer surface goes through SDL
        FetchContent_Declare(
            SDL2
            GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
            GIT_TAG        release-2.32.10
            GIT_SHALLOW    TRUE
        )
        FetchContent_MakeAvailable(SDL2)
        if(TARGET SDL2::SDL2-static)
            set(PS2X_SDL2_TARGET SDL2::SDL2-static)
        elseif(TARGET SDL2-static)
            set(PS2X_SDL2_TARGET SDL2-static)
        else()
            set(PS2X_SDL2_TARGET SDL2::SDL2)
        endif()
        message(STATUS "[hostio] SDL2 gamepad/audio backend: ON (${PS2X_SDL2_TARGET})")
    else()
        message(STATUS "[hostio] SDL2 gamepad/audio backend: OFF (raylib only)")
    endif()
endif()

# [B] The vendored raylib takes its window/input/GL context from SDL2 (PLATFORM_DESKTOP_SDL), so the
# platform needs SDL2 linked here -- after the fetch above, whose target name depends on how SDL2 was
# built. raylib is linked against the same SDL2 the hostio backend uses.
if(TARGET bt3gl AND DEFINED PS2X_SDL2_TARGET)
    target_link_libraries(bt3gl PUBLIC ${PS2X_SDL2_TARGET})
    message(STATUS "[B] bt3gl (vendored raylib) window backend: SDL2 (${PS2X_SDL2_TARGET})")
    # [C] imgui_impl_sdl2.cpp (the overlay's platform backend) needs SDL2's headers and symbols.
    if(TARGET imgui)
        target_link_libraries(imgui PUBLIC ${PS2X_SDL2_TARGET})
    endif()
else()
    # The window/input/GL context come from SDL2 now (PLATFORM_DESKTOP_SDL), so there is no fallback:
    # fail here with a clear message instead of a wall of undefined SDL_* symbols at link time.
    message(FATAL_ERROR
        "The runtime window backend is SDL2 (PLATFORM_DESKTOP_SDL). Configure with PS2X_HOST_SDL2=ON.")
endif()

# [d3d11 probe] Native Direct3D 11 video on raylib's HWND, buildable on its own so the
# P1 milestone (device + swap chain + present, raylib keeping window/input/audio) can be
# validated without compiling the full runner. See tools/d3d11_probe/main.cpp.
if(WIN32 AND NOT PS2X_IS_VITA)
    add_executable(ps2xD3D11Probe
        tools/d3d11_probe/main.cpp
        src/gfx/d3d11/D3D11Device.cpp
        src/gfx/d3d11/D3D11Gfx.cpp
    )
    target_include_directories(ps2xD3D11Probe PRIVATE src)
    target_link_libraries(ps2xD3D11Probe PRIVATE bt3gl d3d11 dxgi d3dcompiler dxguid)
    if(MSVC)
        target_compile_options(ps2xD3D11Probe PRIVATE /W4)
    else()
        target_compile_options(ps2xD3D11Probe PRIVATE -Wall -Wextra)
    endif()
endif()

# [altGL probe] The standalone OpenGL render layer (loader + gfx/gl + present). Built on every
# platform so the port can be validated in parallel with the raylib GL path and the D3D11 layer.
# raylib is used ONLY for the window/context here (transition); see tools/gl_probe/main.cpp.
if(NOT PS2X_IS_VITA)
    add_executable(ps2xGlProbe
        tools/gl_probe/main.cpp
        src/gfx/gl/GlApi.cpp
        src/gfx/gl/GlDevice.cpp
        src/gfx/gl/GlGfx.cpp
    )
    target_include_directories(ps2xGlProbe PRIVATE src)
    if(WIN32)
        target_link_libraries(ps2xGlProbe PRIVATE bt3gl opengl32)
    else()
        find_package(OpenGL REQUIRED)
        target_link_libraries(ps2xGlProbe PRIVATE bt3gl OpenGL::GL ${CMAKE_DL_LIBS})
    endif()
    if(MSVC)
        target_compile_options(ps2xGlProbe PRIVATE /W4)
    else()
        target_compile_options(ps2xGlProbe PRIVATE -Wall -Wextra)
    endif()
endif()

add_library(ps2_host_backend INTERFACE)

set(PS2X_DEFAULT_BOOT_ELF_DEFINE "")

if(PS2X_DEFAULT_BOOT_ELF)
    set(PS2X_DEFAULT_BOOT_ELF_DEFINE "${PS2X_DEFAULT_BOOT_ELF}")
    string(REPLACE "\\" "\\\\" PS2X_DEFAULT_BOOT_ELF_DEFINE "${PS2X_DEFAULT_BOOT_ELF_DEFINE}")
    string(REPLACE "\"" "\\\"" PS2X_DEFAULT_BOOT_ELF_DEFINE "${PS2X_DEFAULT_BOOT_ELF_DEFINE}")
endif()

if(PS2X_IS_VITA)
    set(PS2X_VITA_PREFIX "${PS2X_VITASDK}/arm-vita-eabi")
    set(PS2X_VITA_EXTRA_INCLUDE_DIRS "" CACHE STRING "Extra include directories for Vita host dependencies")
    set(PS2X_VITA_LIBRARY_DIR "${PS2X_VITA_PREFIX}/lib")
    set(PS2X_VITA_REQUIRED_LIBRARY_NAMES
        raylib
        SDL2
        OpenSLES
        taihen_stub
        SceAppMgr_stub
        SceCtrl_stub
        SceGxm_stub
        SceCommonDialog_stub
        SceLibKernel_stub
        SceAudio_stub
        SceTouch_stub
        SceHid_stub
        SceMotion_stub
        SceSysmodule_stub
        SceIofilemgr_stub
        SceNetCtl_stub
        SceNet_stub
        SceDisplay_stub
        SceAppUtil_stub
        SceAudioIn_stub
        ScePower_stub
        SceProcessmgr_stub
        SceIme_stub
        libIMGEGL_stub_weak
        libgpu_es4_ext_stub_weak
        libGLESv2_stub_weak
    )

    set(PS2X_VITA_INCLUDE_DIRS
        "${PS2X_VITA_PREFIX}/include"
    )

    foreach(PS2X_VITA_OPTIONAL_INCLUDE_DIR IN ITEMS
        "${PS2X_VITA_PREFIX}/include/raylib"
        "${PS2X_VITA_PREFIX}/include/SDL2")
        if(EXISTS "${PS2X_VITA_OPTIONAL_INCLUDE_DIR}")
            list(APPEND PS2X_VITA_INCLUDE_DIRS "${PS2X_VITA_OPTIONAL_INCLUDE_DIR}")
        endif()
    endforeach()

    if(PS2X_VITA_EXTRA_INCLUDE_DIRS)
        list(APPEND PS2X_VITA_INCLUDE_DIRS ${PS2X_VITA_EXTRA_INCLUDE_DIRS})
    endif()

    if(NOT EXISTS "${PS2X_VITA_PREFIX}/include/SDL2/SDL.h")
        message(FATAL_ERROR
            "Missing SDL2 headers under ${PS2X_VITA_PREFIX}/include/SDL2. "
            "Quenom/raylib-5.5-vita requires SDL2 with PVR support installed into VitaSDK.")
    endif()

    if(NOT EXISTS "${PS2X_VITA_LIBRARY_DIR}")
        message(FATAL_ERROR "Missing Vita library directory: ${PS2X_VITA_LIBRARY_DIR}")
    endif()

    function(ps2x_find_vita_library out_var library_name)
        string(MAKE_C_IDENTIFIER "${library_name}" library_id)
        find_library(${out_var}
            NAMES "${library_name}" "${library_name}.a" "lib${library_name}.a"
            PATHS "${PS2X_VITA_LIBRARY_DIR}"
            NO_DEFAULT_PATH
        )

        if(NOT ${out_var})
            message(FATAL_ERROR
                "Could not find Vita library '${library_name}' under ${PS2X_VITA_LIBRARY_DIR}. "
                "Install Quenom/raylib-5.5-vita and its SDL2/PVR dependencies into VitaSDK first.")
        endif()

        set(${out_var} "${${out_var}}" PARENT_SCOPE)
    endfunction()

    set(PS2X_VITA_RESOLVED_LIBRARIES "")

    foreach(PS2X_VITA_LIBRARY_NAME IN LISTS PS2X_VITA_REQUIRED_LIBRARY_NAMES)
        ps2x_find_vita_library(PS2X_VITA_LIBRARY_PATH "${PS2X_VITA_LIBRARY_NAME}")
        list(APPEND PS2X_VITA_RESOLVED_LIBRARIES "${PS2X_VITA_LIBRARY_PATH}")
    endforeach()

    target_compile_definitions(ps2_host_backend INTERFACE PLATFORM_VITA)
    target_include_directories(ps2_host_backend INTERFACE ${PS2X_VITA_INCLUDE_DIRS})
    target_link_libraries(ps2_host_backend INTERFACE
        ${PS2X_VITA_RESOLVED_LIBRARIES}
        m
        c
        pthread
    )
else()
    target_link_libraries(ps2_host_backend INTERFACE bt3gl)
endif()

if(WIN32)
    include(ExternalProject)

    set(FFMPEG_PREBUILT_TAG "n7.1-241205")
    set(FFMPEG_PREBUILT_URL
        "https://github.com/System233/ffmpeg-msvc-prebuilt/releases/download/${FFMPEG_PREBUILT_TAG}/ffmpeg-${FFMPEG_PREBUILT_TAG}-lgpl-amd64-shared.zip")

    set(FFMPEG_PREFIX_DIR "${CMAKE_BINARY_DIR}/ThirdParty/ffmpeg-prefix")
    set(FFMPEG_SOURCE_DIR "${FFMPEG_PREFIX_DIR}/src/ffmpeg_external")
    set(FFMPEG_INCLUDE_DIR "${FFMPEG_SOURCE_DIR}/include")
    set(FFMPEG_LIB_DIR "${FFMPEG_SOURCE_DIR}/bin")
    set(FFMPEG_BIN_DIR "${FFMPEG_SOURCE_DIR}/bin")

    ExternalProject_Add(ffmpeg_external
        URL "${FFMPEG_PREBUILT_URL}"
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        PREFIX "${FFMPEG_PREFIX_DIR}"
        CONFIGURE_COMMAND ""
        BUILD_COMMAND ""
        INSTALL_COMMAND ""
        BUILD_BYPRODUCTS
        "${FFMPEG_LIB_DIR}/avcodec.lib"
        "${FFMPEG_LIB_DIR}/avformat.lib"
        "${FFMPEG_LIB_DIR}/avutil.lib"
        "${FFMPEG_LIB_DIR}/swresample.lib"
        "${FFMPEG_LIB_DIR}/swscale.lib"
    )

    # Link against the import libraries shipped with the Windows DLLs.
    add_library(ffmpeg_avcodec STATIC IMPORTED GLOBAL)
    set_target_properties(ffmpeg_avcodec PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_LIB_DIR}/avcodec.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_INCLUDE_DIR}"
    )
    add_dependencies(ffmpeg_avcodec ffmpeg_external)

    add_library(ffmpeg_avformat STATIC IMPORTED GLOBAL)
    set_target_properties(ffmpeg_avformat PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_LIB_DIR}/avformat.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_INCLUDE_DIR}"
    )
    add_dependencies(ffmpeg_avformat ffmpeg_external)

    add_library(ffmpeg_avutil STATIC IMPORTED GLOBAL)
    set_target_properties(ffmpeg_avutil PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_LIB_DIR}/avutil.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_INCLUDE_DIR}"
    )
    add_dependencies(ffmpeg_avutil ffmpeg_external)

    add_library(ffmpeg_swresample STATIC IMPORTED GLOBAL)
    set_target_properties(ffmpeg_swresample PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_LIB_DIR}/swresample.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_INCLUDE_DIR}"
    )
    add_dependencies(ffmpeg_swresample ffmpeg_external)

    add_library(ffmpeg_swscale STATIC IMPORTED GLOBAL)
    set_target_properties(ffmpeg_swscale PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_LIB_DIR}/swscale.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_INCLUDE_DIR}"
    )
    add_dependencies(ffmpeg_swscale ffmpeg_external)

    add_library(ffmpeg INTERFACE)
    target_link_libraries(ffmpeg INTERFACE
        ffmpeg_avcodec
        ffmpeg_avformat
        ffmpeg_avutil
        ffmpeg_swresample
        ffmpeg_swscale
        bcrypt
        secur32
        ws2_32
        user32
        advapi32
        ole32
        shell32
    )

    set(PS2X_FFMPEG_BIN_DIR "${FFMPEG_BIN_DIR}" CACHE INTERNAL "Directory containing FFmpeg runtime DLLs")
    set(PS2X_COPY_FFMPEG_DLLS_SCRIPT
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CopyFfmpegDlls.cmake"
        CACHE INTERNAL
        "Script used to stage FFmpeg runtime DLLs beside executables")
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET GLOBAL
        libavcodec
        libavformat
        libavutil
        libswresample
        libswscale
    )

    add_library(ffmpeg INTERFACE)
    target_link_libraries(ffmpeg INTERFACE PkgConfig::FFMPEG)
endif()

function(ps2x_stage_ffmpeg_runtime_dlls target_name)
    if(NOT WIN32)
        return()
    endif()

    if(NOT PS2X_FFMPEG_BIN_DIR OR NOT PS2X_COPY_FFMPEG_DLLS_SCRIPT)
        return()
    endif()

    if(TARGET ffmpeg_external)
        add_dependencies(${target_name} ffmpeg_external)
    endif()

    add_custom_command(
        TARGET ${target_name} POST_BUILD
        COMMAND "${CMAKE_COMMAND}"
        "-Dsource_dir=${PS2X_FFMPEG_BIN_DIR}"
        "-Ddest_dir=$<TARGET_FILE_DIR:${target_name}>"
        -P "${PS2X_COPY_FFMPEG_DLLS_SCRIPT}"
        VERBATIM
    )
endfunction()

add_library(ps2_runtime STATIC
    src/lib/game_overrides.cpp
    src/lib/ps2_eeprof.cpp
    src/lib/ps2_stepcensus.cpp
    src/lib/ps2_fiber.cpp
    src/lib/ps2_gif_arbiter.cpp
    src/lib/ps2_audio.cpp
    src/lib/ps2_audio_vag.cpp
    src/lib/ps2_host_audio.cpp   # [hostio] audio device: SDL2 mixer or raylib/miniaudio
src/lib/ps2_host_window.cpp  # [B] native HWND from raylib's window handle (SDL returns the SDL_Window*)
src/lib/ui_sdl.cpp          # [C] ImGui platform glue on SDL2 (replaces rlImGui)
    src/lib/ps2_host_pad.cpp     # [hostio] gamepads: SDL2 gamecontroller (default) or raylib's own API
    src/lib/ps2_gs_gpu.cpp
    src/lib/ps2_gs_gpu_renderer.cpp
    src/lib/ps2_win_timer.cpp
    src/lib/ps2_gs_memory.cpp
    src/lib/ps2_gs_rasterizer.cpp
    src/lib/ps2_texreplace.cpp   # [texreplace] PCSX2-compatible texture identity
    src/lib/ps2_texcache.cpp     # [texcache] persistent write-back texture cache
    src/lib/ps2_fmv_override.cpp # [fmvoverride] inject a user 4K video as the opening FMV
    src/lib/ps2_netplay.cpp      # [netplay] deterministic lockstep transport
    src/lib/ps2_iop.cpp
    src/lib/ps2_iop_audio.cpp
    src/lib/ps2_iop_dbcman.cpp
    src/lib/ps2_iop_sdrdrv.cpp
    src/lib/ps2_iop_cl.cpp
    src/lib/ps2_memory.cpp
    src/lib/ps2_pad.cpp
    src/lib/pad_config.cpp
    src/lib/ps2_runtime.cpp
    src/lib/ps2_interp.cpp
    src/lib/ps2_vif1_interpreter.cpp
    src/lib/ps2_vu1.cpp
    src/lib/ps2_vu1_native.cpp   # [vunative] hand-written kernels for the hot VU1 microprograms
    src/lib/games_database.cpp
)

# Native Linux evdev reader for unmapped gamepads (xone driver).
if(UNIX AND NOT APPLE)
    target_sources(ps2_runtime PRIVATE src/lib/pad_evdev_linux.cpp)
endif()

# [hostio] SDL2 is only included by the two host files; PRIVATE keeps <SDL.h> (and its `main`
# macro) out of every other translation unit, while the static lib still reaches the runner.
if(NOT PS2X_IS_VITA AND PS2X_HOST_SDL2)
    target_compile_definitions(ps2_runtime PRIVATE PS2X_HAVE_SDL2=1)
    target_link_libraries(ps2_runtime PRIVATE ${PS2X_SDL2_TARGET})
endif()

# The emulator runtime is small (~25 files) and holds every hot loop (rasterizer,
# GS, VIF, VU1, memory). Build it optimized even when the global flags stay at
# -O0 for the 7800 generated runner files. -fno-strict-aliasing/-fwrapv: the
# runtime type-puns guest memory and relies on integer wraparound.
if(NOT MSVC)
    target_compile_options(ps2_runtime PRIVATE -O2 -fno-strict-aliasing -fwrapv)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(ps2_runtime PRIVATE /clang:-fno-strict-aliasing /clang:-fwrapv /clang:-ffp-contract=off)   # clang-cl: same contract
endif()

if(PS2X_ENABLE_RUNTIME_LOGS)
    target_compile_definitions(ps2_runtime PUBLIC
        PS2_RUNTIME_LOGS=1
    )
endif()

if(PS2X_ENABLE_AGRESSIVE_LOGS)
    target_compile_definitions(ps2_runtime PUBLIC
        AGRESSIVE_LOGS=1
    )
endif()

if(PS2X_STRICT_RETURN_DIAGNOSTICS)
    target_compile_definitions(ps2_runtime PUBLIC
        PS2X_STRICT_RETURN_DIAGNOSTICS=1
    )
endif()

file(GLOB_RECURSE KERNEL_SRC_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/lib/Kernel/*.cpp"
)

target_sources(ps2_runtime PRIVATE
    ${KERNEL_SRC_FILES}
)

file(GLOB RUNNER_SRC_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/runner/*.cpp"
)

# Recompiled DBZP.BIN overlay (the game code). Kept in a separate directory so the
# boot ELF's runner sources are never touched. See src/runner_overlay/.
file(GLOB RUNNER_OVERLAY_SRC_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/runner_overlay/*.cpp"
)
list(APPEND RUNNER_SRC_FILES ${RUNNER_OVERLAY_SRC_FILES})

set(RUNNER_MAIN_CPP "${CMAKE_CURRENT_SOURCE_DIR}/src/runner/main.cpp")
set(ROOT_MAIN_CPP "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp")

if(EXISTS "${RUNNER_MAIN_CPP}")
    list(APPEND RUNNER_SRC_FILES "${RUNNER_MAIN_CPP}")
elseif(EXISTS "${ROOT_MAIN_CPP}")
    list(APPEND RUNNER_SRC_FILES "${ROOT_MAIN_CPP}")
endif()

set(RUNNER_RESOURCES "")
if(WIN32)
    set(RUNNER_RESOURCES "${CMAKE_CURRENT_SOURCE_DIR}/bt3.rc")
endif()
add_executable(ps2EntryRunner
    ${RUNNER_SRC_FILES}
    ${RUNNER_RESOURCES}
)

# The overlay's single 20MB TU and its register file must not be unity-batched
# with the boot ELF's sources (anon-namespace initializer name clash + huge TU).
if(RUNNER_OVERLAY_SRC_FILES)
    set_source_files_properties(${RUNNER_OVERLAY_SRC_FILES} PROPERTIES
        SKIP_UNITY_BUILD_INCLUSION ON
    )
endif()

if(PS2X_ENABLE_RUNNER_UNITY_BUILD)
    set_target_properties(ps2EntryRunner PROPERTIES
        UNITY_BUILD ON
        UNITY_BUILD_BATCH_SIZE "${PS2X_RUNNER_UNITY_BUILD_BATCH_SIZE}"
    )
endif()

if(PS2X_DEFAULT_BOOT_ELF_DEFINE)
    target_compile_definitions(ps2EntryRunner PRIVATE
        PS2X_DEFAULT_BOOT_ELF="${PS2X_DEFAULT_BOOT_ELF_DEFINE}"
    )
endif()

if(MSVC)
    target_compile_options(ps2EntryRunner PRIVATE /FS)
endif()

# The runtime and the generated runner sources use SSE4.1 intrinsics
# (_mm_blendv_ps, _mm_extract_epi32, ...). Guarantee the ISA is enabled even
# when the user configures without -march=native.
if(NOT MSVC AND PS2X_TARGET_PROCESSOR MATCHES "x86_64|AMD64|amd64")
    target_compile_options(ps2_runtime PRIVATE -msse4.1)
    target_compile_options(ps2EntryRunner PRIVATE -msse4.1)
endif()

# ARM can fuse multiply/add by default; guest VU math requires two roundings.
if(APPLE)
    target_compile_options(ps2_runtime PRIVATE -ffp-contract=off)
    target_compile_options(ps2EntryRunner PRIVATE -ffp-contract=off -fno-strict-aliasing -fwrapv)
endif()

# Mirror the Linux build on MSVC: the generated runner sources compile WITHOUT
# optimization (the hot loops live in ps2_runtime, which keeps the config's /O2).
# Optimizing the giant machine-generated TUs pushed a single cl.exe past 34 GB of
# commit (observed via Resource-Exhaustion event). /bigobj: generated TUs exceed
# the default COFF section limit.
if(MSVC AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(ps2EntryRunner PRIVATE /Od /bigobj)
elseif(MSVC)
    target_compile_options(ps2EntryRunner PRIVATE /bigobj /clang:-ffp-contract=off /clang:-fno-strict-aliasing /clang:-fwrapv)   # clang-cl: optimized like Linux (-O3 there)
endif()

target_include_directories(ps2_runtime PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/lib/Kernel
    ${CMAKE_CURRENT_SOURCE_DIR}/src          # gfx/... (D3D11 device etc.)
)

target_link_libraries(ps2_runtime PUBLIC ps2_host_backend ffmpeg)

# [d3d11] Native Direct3D 11 video device (device + DXGI swap chain), Windows-only.
# d3dcompiler is linked now so the P3 shader port can compile HLSL at runtime.
if(WIN32)
    target_sources(ps2_runtime PRIVATE
        src/gfx/d3d11/D3D11Device.cpp
        src/gfx/d3d11/D3D11Gfx.cpp)
    target_link_libraries(ps2_runtime PUBLIC d3d11 dxgi d3dcompiler dxguid)
endif()

# [altGL] Standalone OpenGL layer (own loader + gfx/gl). Opt-in present (PS2X_ALTGL=1) while the
# renderer is migrated off raylib; coexists with the D3D11 device in the same TU.
target_sources(ps2_runtime PRIVATE
    src/gfx/gl/GlApi.cpp
    src/gfx/gl/GlDevice.cpp
    src/gfx/gl/GlGfx.cpp
    src/gfx/gl/gsrl_vendor.cpp   # [R1] vendored rlgl subset (own state cache/batcher for the replay)
    src/gfx/gl/gsrl.cpp          # [R1] mirroring wrappers (vendored + raylib stacks)
    src/gfx/gl_context.cpp
    src/gfx/gs_resources.cpp
    src/gfx/gs_rt.cpp
    src/gfx/gs_gl.cpp
    src/gfx/image_io.cpp
    src/gfx/video_overlay.cpp)
if(PS2X_USE_GSRL)
    target_compile_definitions(ps2_runtime PRIVATE PS2X_USE_GSRL=1)
    message(STATUS "[gsrl] experiment ON: the replay submits through the renamed vendored rlgl copy")
endif()
# [A4] stb_image(_write) / glad come from bt3gl as "external/<name>.h" (bt3gl/src is on the path through
# the bt3gl link). [linuxfix] Do NOT add bt3gl/src/external itself: its Windows dirent.h shim (<io.h>)
# would shadow the system <dirent.h> in every runtime file (pad_evdev_linux.cpp).
# [C] ui_sdl.cpp includes imgui_impl_sdl2.h: the backend headers come from the imgui target (PUBLIC).
target_link_libraries(ps2_runtime PRIVATE imgui)
if(WIN32)
    target_link_libraries(ps2_runtime PUBLIC opengl32)
else()
    find_package(OpenGL REQUIRED)
    target_link_libraries(ps2_runtime PUBLIC OpenGL::GL ${CMAKE_DL_LIBS})
endif()

# [statesync] libunwind: the netplay state sync unwinds each parked guest fiber's host call chain into a
# structural signature (the peer only adopts a state at a matching park). Optional: without it the
# signature falls back to the wait point + guest pc/ra, which is weaker.
if(UNIX AND NOT APPLE)
    find_library(PS2X_UNWIND_LIB unwind)
    find_path(PS2X_UNWIND_INC libunwind.h)
    if(PS2X_UNWIND_LIB AND PS2X_UNWIND_INC)
        target_link_libraries(ps2_runtime PUBLIC ${PS2X_UNWIND_LIB} ${CMAKE_DL_LIBS})
        target_include_directories(ps2_runtime PRIVATE ${PS2X_UNWIND_INC})
        set_source_files_properties(src/lib/ps2_runtime.cpp PROPERTIES COMPILE_DEFINITIONS PS2X_HAVE_LIBUNWIND=1)
        message(STATUS "[statesync] libunwind: ${PS2X_UNWIND_LIB}")
    else()
        message(STATUS "[statesync] libunwind not found: park signatures use the wait point only")
    endif()
endif()

# [pgs] optional paraLLEl-GS backend (Vulkan compute GS, LGPLv3+): built in when the checkout is present.
# ps2xRuntime/third_party/parallel-gs is a git-ignored symlink/checkout of https://github.com/Arntzen-Software/parallel-gs
set(PS2X_PGS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/parallel-gs" CACHE PATH "paraLLEl-GS checkout (with its Granite submodule)")
option(PS2X_DISABLE_PGS "Do not build the paraLLEl-GS backend even if the checkout is present" OFF)
# paraLLEl-GS does not compile on macOS: Granite's sleep_until_nsecs() uses
# clock_nanosleep/TIMER_ABSTIME for everything that is not _WIN32, and macOS has
# neither. Gate it on the platform rather than on a cache variable alone, so a
# regenerated cache cannot silently turn it back on. PS2X_FORCE_PGS=ON opts in
# (needs MoltenVK and a patched Granite).
option(PS2X_FORCE_PGS "Build paraLLEl-GS even on platforms it is not known to support" OFF)
if (APPLE AND NOT PS2X_FORCE_PGS)
    set(PS2X_DISABLE_PGS ON)
    set(PS2X_PGS_SKIP_REASON "not supported on macOS (Granite needs clock_nanosleep)")
endif()
# [pgs] paraLLEl-GS is available on Windows again: D3D11 is retired for now, so the Vulkan compute
# backend is a first-class renderer option here once more (macOS is still excluded above).
# [pgs submodule] Fetch it ourselves rather than requiring every clone to remember
# --recursive. Without this a plain `git clone` + build produces a runner with NO
# paraLLEl-GS renderer and only a STATUS line to say so, which is easy to miss and
# very expensive to discover later. git is already a hard build requirement here (the
# raylib 5.5 patch below is applied with `git apply`), so leaning on it costs nothing.
# safe.directory='*' is passed with -c so we never write to the user's real gitconfig.
if (NOT EXISTS "${PS2X_PGS_DIR}/CMakeLists.txt" AND NOT PS2X_DISABLE_PGS
    AND EXISTS "${CMAKE_SOURCE_DIR}/.gitmodules" AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
    message(STATUS "[pgs] paraLLEl-GS checkout missing -- initialising submodules")
    execute_process(COMMAND git -c safe.directory=* -C "${CMAKE_SOURCE_DIR}"
                            submodule update --init --recursive
                            ps2xRuntime/third_party/parallel-gs
                    RESULT_VARIABLE _pgs_sub OUTPUT_VARIABLE _pgs_subout ERROR_VARIABLE _pgs_subout)
    if (NOT _pgs_sub EQUAL 0)
        message(WARNING "[pgs] could not initialise the paraLLEl-GS submodule:\n${_pgs_subout}")
    endif()
endif()

if (EXISTS "${PS2X_PGS_DIR}/CMakeLists.txt" AND NOT PS2X_DISABLE_PGS)
    # [pgs patches] Two upstream fixes that live in the paraLLEl-GS / Granite submodules. Both
    # point at repos we cannot commit to (Granite is upstream Themaister/Granite), so they are
    # carried as patch files and applied here rather than making every clone run git by hand.
    #
    #   granite-framectx     frame-context ring 2 -> 16. The ring is FLUSH-depth, not frame-depth,
    #                        and paraLLEl-GS flushes ~7x per frame under BT3's packet rate.
    #   parallel-gs-gcframe  advance that ring only at the REAL frame boundary. flush_submit is
    #                        not one: 6 of those 7 calls are FlushReason::HostAccess sync points,
    #                        and each advance blocked in Device::PerFrame::wait. Worth 3x 1P-vs-COM
    #                        52.3 -> 58.8 fps, splitscreen 45.6 -> 49.2 with a much tighter spread.
    #
    # Each entry detects the EFFECT, not the patch: if the marker is already in the probe file --
    # from this patch or from the fuller pgswait diagnostic patch used on dev branches -- it is
    # skipped, and nothing is ever reset. Failure is a WARNING: both are pure performance, so a
    # checkout they cannot patch should still build and run, just slower.
    foreach(_e IN ITEMS "granite-framectx:${PS2X_PGS_DIR}/Granite:granite-framectx.patch:PS2X_PGS_FRAMECTX:vulkan/device.cpp"
                        "gcframe:${PS2X_PGS_DIR}:parallel-gs-gcframe.patch:frame_boundary_flush:gs/gs_renderer.hpp")
        string(REPLACE ":" ";" _el "${_e}")
        list(GET _el 0 _ename)
        list(GET _el 1 _edir)
        list(GET _el 2 _efile)
        list(GET _el 3 _emark)
        list(GET _el 4 _eprobe)
        set(_epatch "${CMAKE_CURRENT_SOURCE_DIR}/patches/${_efile}")
        if (EXISTS "${_edir}/${_eprobe}" AND EXISTS "${_epatch}")
            file(READ "${_edir}/${_eprobe}" _esrc)
            string(FIND "${_esrc}" "${_emark}" _ehas)
            unset(_esrc)
            if (_ehas EQUAL -1)
                execute_process(COMMAND git -c safe.directory=* -C "${_edir}" apply --ignore-whitespace "${_epatch}"
                                RESULT_VARIABLE _eres ERROR_VARIABLE _eerr)
                if (_eres EQUAL 0)
                    message(STATUS "[pgs] ${_ename} patch applied")
                else()
                    message(WARNING "[pgs] ${_ename} patch did not apply to ${_edir} -- the backend still builds, just slower. Apply by hand:\n"
                                    "    git -C \"${_edir}\" apply \"${_epatch}\"\n${_eerr}")
                endif()
            else()
                message(STATUS "[pgs] ${_ename}: already present, nothing to do")
            endif()
        endif()
    endforeach()

    set(PARALLEL_GS_STANDALONE ON CACHE BOOL "" FORCE)
    add_subdirectory("${PS2X_PGS_DIR}" "${CMAKE_BINARY_DIR}/parallel-gs" EXCLUDE_FROM_ALL)
    target_sources(ps2_runtime PRIVATE src/lib/ps2_gs_pgs.cpp)
    target_link_libraries(ps2_runtime PUBLIC parallel-gs)
    target_compile_definitions(ps2_runtime PUBLIC PS2X_HAVE_PGS=1)
    message(STATUS "[pgs] paraLLEl-GS backend enabled from ${PS2X_PGS_DIR}")
else()
    # Say WHICH of the three reasons applies: the old message always blamed a
    # missing checkout, including when the backend was deliberately disabled.
    if (PS2X_PGS_SKIP_REASON)
        set(_pgs_why "${PS2X_PGS_SKIP_REASON}")
    elseif (PS2X_DISABLE_PGS)
        set(_pgs_why "PS2X_DISABLE_PGS=ON")
    else()
        set(_pgs_why "no checkout at ${PS2X_PGS_DIR}")
    endif()
    if (PS2X_DISABLE_PGS OR PS2X_PGS_SKIP_REASON)
        message(STATUS "[pgs] paraLLEl-GS backend not built (${_pgs_why})")
    else()
        # Not fatal -- the software/GL path still builds -- but this is the renderer, and
        # the old STATUS line scrolled past unnoticed in a wall of configure output.
        message(WARNING "[pgs] paraLLEl-GS backend NOT BUILT (${_pgs_why}).\n"
                        "The runner will have no paraLLEl-GS renderer. Fix the checkout with:\n"
                        "    git -C \"${CMAKE_SOURCE_DIR}\" submodule update --init --recursive\n"
                        "or configure with -DPS2X_DISABLE_PGS=ON to build without it deliberately.")
    endif()
endif()
target_link_libraries(ps2EntryRunner
    PRIVATE
    ps2_runtime
)

if(NOT PS2X_IS_VITA)
    target_sources(ps2EntryRunner PRIVATE
        src/lib/ps2_settings_overlay.cpp
        src/gfx/ps2x_ui.cpp          # overlay backend bridge (rlImGui or imgui_impl_dx11)
    )
    if(WIN32)
        target_sources(ps2EntryRunner PRIVATE
            src/gfx/d3d11/ui_d3d11.cpp   # imgui_impl_dx11 backend (needs windows.h)
        )
    endif()
    target_link_libraries(ps2EntryRunner PRIVATE imgui)
    # gamemode: request CPU governor / IO priority optimisations at startup
    # (auto-release on exit; harmless no-op if the daemon isn't running).
    find_library(GAMEMODE_LIB NAMES libgamemodeauto.so gamemodeauto NAMES_PER_DIR)
    if(GAMEMODE_LIB)
        target_link_libraries(ps2EntryRunner PRIVATE ${GAMEMODE_LIB})
        message(STATUS "gamemode: linking libgamemodeauto (${GAMEMODE_LIB})")
    else()
        message(STATUS "gamemode: libgamemodeauto not found, running without it")
    endif()
    # Stage the settings-overlay assets (font) next to the executable.
    add_custom_command(TARGET ps2EntryRunner POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/assets"
            "$<TARGET_FILE_DIR:ps2EntryRunner>/assets"
        COMMENT "Staging overlay assets (fonts) next to ps2EntryRunner"
    )
    target_compile_definitions(ps2EntryRunner PRIVATE PS2X_ENABLE_DEBUG_UI=1)
endif()

# [texreplace] Stage the repo's textures/ (as data/Textures, where the runtime looks) and mods/
# next to the executable. The game runs from the build output, so without this the folders would
# exist in the repo but not where a user would actually look. copy_directory MERGES, so rebuilding
# never disturbs a pack someone has dropped in.
add_custom_command(TARGET ps2EntryRunner POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/../textures" "$<TARGET_FILE_DIR:ps2EntryRunner>/data/Textures"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/../mods"     "$<TARGET_FILE_DIR:ps2EntryRunner>/mods"
    COMMENT "Staging data/Textures and mods/ next to ps2EntryRunner"
)

ps2x_stage_ffmpeg_runtime_dlls(ps2EntryRunner)

if(PS2X_IS_VITA)
    set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d PARENTAL_LEVEL=1")

    if(PS2X_VITA_CREATE_PACKAGE)
        vita_create_self(eboot.bin ps2EntryRunner UNSAFE)
        vita_create_vpk(ps2EntryRunner.vpk ${PS2X_VITA_TITLEID} eboot.bin
            VERSION ${PS2X_VITA_VERSION}
            NAME ${PS2X_VITA_APP_NAME}
        )
    endif()
endif()

if(WIN32)
    # Guest threads (std::thread, default stack) run recompiled game code with deep call chains and
    # large frames; Linux gives them 8 MB, Windows 1 MB unless the image says otherwise. The PE
    # default stack reserve applies to every thread created without an explicit size.
    target_link_options(ps2EntryRunner PRIVATE "/STACK:134217728")
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    EnableFastReleaseMode(ps2_runtime)
    EnableFastReleaseMode(ps2EntryRunner)

    if(WIN32 AND NOT PS2X_SHOW_WINDOWS_CONSOLE)
        if(MSVC)
            target_link_options(ps2EntryRunner PRIVATE
                $<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>:/SUBSYSTEM:WINDOWS>
                $<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>:/ENTRY:mainCRTStartup>
            )
        elseif(MINGW)
            target_link_options(ps2EntryRunner PRIVATE
                $<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>:-mwindows>
            )
        endif()
    endif()
endif()

# Work around WinAPI vs raylib symbol clash for CloseWindow on x64
if(MSVC)
    target_link_options(ps2EntryRunner PRIVATE "/FORCE:MULTIPLE")
    # [prof] emit a linker map so sampled RIPs can be resolved to function names.
    target_link_options(ps2EntryRunner PRIVATE "/MAP:${CMAKE_BINARY_DIR}/ps2EntryRunner.map")
endif()

install(TARGETS ps2_runtime ps2EntryRunner
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

if(WIN32)
    install(DIRECTORY "${FFMPEG_BIN_DIR}/"
        DESTINATION bin
        FILES_MATCHING
        PATTERN "*.dll"
    )
endif()

install(DIRECTORY include/
    DESTINATION include
)
# [lab] line-level perf annotate of the VU1 interpreter (-g only, no codegen change)
set_source_files_properties(src/lib/ps2_vu1.cpp PROPERTIES COMPILE_OPTIONS "-g")
set_source_files_properties(src/lib/ps2_gs_rasterizer.cpp PROPERTIES COMPILE_OPTIONS "-g")   # [lab] annotate the sprite recorder

# [fps60] the 60 fps pacing rules must sit next to the runner in EVERY build and bundle: without them the overlay
# toggle is inert (a clean release build shipped an inert button on 2026-09-11).
add_custom_command(TARGET ps2EntryRunner POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_SOURCE_DIR}/games/bt3/fps60_sites.txt"
            "$<TARGET_FILE_DIR:ps2EntryRunner>/fps60_sites.txt"
    COMMENT "Staging fps60_sites.txt next to ps2EntryRunner")
