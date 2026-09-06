cmake_minimum_required(VERSION 3.20)

project(PS2Runtime VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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

    FetchContent_Declare(
        raylib
        GIT_REPOSITORY "https://github.com/raysan5/raylib.git"
        GIT_TAG "5.5" # we will migrate to 4.2.0 later, trust me it will be better
        GIT_PROGRESS TRUE
        # Without this, a fresh clone pulls raylib's whole history: 60794 objects, ~350 MiB, for
        # a pinned tag we only ever build one revision of. Depth 1 fetches a fraction of that.
        GIT_SHALLOW TRUE
        # [ps2x raylib patch] batch multi-buffering (4 buffers), no per-flush texture/VAO/program unbinds,
        # draw-call/flush counters + flush timer read by the fps line. Applied once at populate time;
        # the reverse --check makes a re-populate of an already-patched source a no-op.
        PATCH_COMMAND git apply --reverse --check --quiet "${CMAKE_CURRENT_SOURCE_DIR}/patches/raylib-5.5-ps2x.patch" || git apply --ignore-whitespace "${CMAKE_CURRENT_SOURCE_DIR}/patches/raylib-5.5-ps2x.patch"
    )
    FetchContent_MakeAvailable(raylib)

    # raylib defaults to MAX_GAMEPADS=4, and on Linux those four slots are not necessarily
    # controllers: udev sets ID_INPUT_JOYSTICK=1 on plenty of things that are not, notably
    # keyboards exposing System Control / Consumer Control HID pages. On a machine with such a
    # keyboard the four slots fill with junk and a REAL pad plugged in afterwards lands at
    # index >= 4, where IsGamepadAvailable() can never see it -- the pad simply does not exist
    # as far as the game is concerned. Raise the ceiling so every connected device is at least
    # addressable; PadConfig then filters the list down to things that look like gamepads.
    # NOTE: a -DMAX_GAMEPADS=16 compile definition does NOT work. rcore.c includes raylib's
    # own config.h (whenever EXTERNAL_CONFIG_FLAGS is undefined, which is our case) and that
    # header redefines MAX_GAMEPADS unconditionally, so it silently wins -- and -w hides the
    # macro-redefinition warning that would have said so. Patch the fetched header instead;
    # this re-applies on every configure, so a fresh clone gets it too.
    if(EXISTS "${raylib_SOURCE_DIR}/src/config.h")
        file(READ "${raylib_SOURCE_DIR}/src/config.h" _rl_cfg)
        string(REGEX REPLACE "#define[ \t]+MAX_GAMEPADS[ \t]+4"
                             "#define MAX_GAMEPADS                   16" _rl_cfg "${_rl_cfg}")
        file(WRITE "${raylib_SOURCE_DIR}/src/config.h" "${_rl_cfg}")
    # [ps2x raylib patch] the PATCH_COMMAND above only runs when FetchContent populates the source; a build
    # directory populated before the patch existed still needs it. Idempotent: apply only if not applied.
    execute_process(COMMAND git -C "${raylib_SOURCE_DIR}" apply --reverse --check --quiet "${CMAKE_CURRENT_SOURCE_DIR}/patches/raylib-5.5-ps2x.patch"
                    RESULT_VARIABLE _rl_patched OUTPUT_QUIET ERROR_QUIET)
    if(NOT _rl_patched EQUAL 0)
        execute_process(COMMAND git -C "${raylib_SOURCE_DIR}" apply --ignore-whitespace "${CMAKE_CURRENT_SOURCE_DIR}/patches/raylib-5.5-ps2x.patch"
                        RESULT_VARIABLE _rl_apply)
        if(NOT _rl_apply EQUAL 0)
            message(FATAL_ERROR "raylib ps2x patch failed to apply to ${raylib_SOURCE_DIR}")
        endif()
        message(STATUS "raylib ps2x patch applied to ${raylib_SOURCE_DIR}")
    endif()
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

    add_library(imgui STATIC
        "${PS2X_IMGUI_SOURCE_DIR}/imgui.cpp"
        "${PS2X_IMGUI_SOURCE_DIR}/imgui_draw.cpp"
        "${PS2X_IMGUI_SOURCE_DIR}/imgui_tables.cpp"
        "${PS2X_IMGUI_SOURCE_DIR}/imgui_widgets.cpp"
        "${PS2X_IMGUI_SOURCE_DIR}/imgui_demo.cpp"
    )
    target_include_directories(imgui PUBLIC "${PS2X_IMGUI_SOURCE_DIR}")

    FetchContent_Declare(
        rlImGui
        GIT_REPOSITORY https://github.com/raylib-extras/rlImGui.git
        GIT_TAG "Raylib_5_5"
        GIT_SHALLOW TRUE
    )
    FetchContent_GetProperties(rlImGui)

    if(NOT rlimgui_POPULATED)
        FetchContent_Populate(rlImGui)
    endif()

    set(PS2X_RLIMGUI_SOURCE_DIR "${rlimgui_SOURCE_DIR}")

    add_library(rlImGui STATIC
        "${PS2X_RLIMGUI_SOURCE_DIR}/rlImGui.cpp"
    )
    target_include_directories(rlImGui PUBLIC "${PS2X_RLIMGUI_SOURCE_DIR}")
    target_link_libraries(rlImGui PUBLIC raylib imgui)
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
    target_link_libraries(ps2_host_backend INTERFACE raylib)
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
    src/lib/ps2_gif_arbiter.cpp
    src/lib/ps2_audio.cpp
    src/lib/ps2_audio_vag.cpp
    src/lib/ps2_gs_gpu.cpp
    src/lib/ps2_gs_gpu_renderer.cpp
    src/lib/ps2_win_timer.cpp
    src/lib/ps2_gs_memory.cpp
    src/lib/ps2_gs_rasterizer.cpp
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
    src/lib/games_database.cpp
)

# Native Linux evdev reader for unmapped gamepads (xone driver).
if(UNIX AND NOT APPLE)
    target_sources(ps2_runtime PRIVATE src/lib/pad_evdev_linux.cpp)
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

add_executable(ps2EntryRunner
    ${RUNNER_SRC_FILES}
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
if(NOT MSVC AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
    target_compile_options(ps2_runtime PRIVATE -msse4.1)
    target_compile_options(ps2EntryRunner PRIVATE -msse4.1)
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
)

target_link_libraries(ps2_runtime PUBLIC ps2_host_backend ffmpeg)
target_link_libraries(ps2EntryRunner
    PRIVATE
    ps2_runtime
)

if(NOT PS2X_IS_VITA)
    target_sources(ps2EntryRunner PRIVATE
        src/lib/ps2_settings_overlay.cpp
    )
    target_link_libraries(ps2EntryRunner PRIVATE rlImGui imgui)
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
