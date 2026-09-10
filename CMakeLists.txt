cmake_minimum_required(VERSION 3.21)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE AND DEFINED ENV{VITASDK})
    set(PS2X_VITA_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake")
    if(EXISTS "${PS2X_VITA_TOOLCHAIN_FILE}")
        set(CMAKE_TOOLCHAIN_FILE "${PS2X_VITA_TOOLCHAIN_FILE}" CACHE PATH
            "Toolchain file used for cross-compiling" FORCE)
        message(STATUS "Using VitaSDK toolchain from VITASDK: ${CMAKE_TOOLCHAIN_FILE}")
    endif()
endif()

project(PS2RetroX)

# Visual Studio generator: compile a project's files in parallel (MSBuild MultiProcessorCompilation). The
# generator turns /MP into the project property; it is not passed to clang-cl. Without it the runner's
# ~1000 unity units build one at a time (45-minute scratch builds, 2026-09-08).
if(CMAKE_GENERATOR MATCHES "Visual Studio")
    add_compile_options(/MP)
endif()

# A bare configure (setup.py, or a plain `cmake -S . -B build`) used to build with NO
# optimization at all (empty CMAKE_BUILD_TYPE = -O0): the from-scratch runner played
# fights at ~21 fps where the same sources at Release hold the engine's locked 30.
# Default to Release; -DCMAKE_BUILD_TYPE=... still overrides.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# The runtime reads its knobs with getenv/fopen/sscanf; the MSVC CRT flags every call as "unsafe" and the
# Windows build log becomes unreadable (dozens of notes per file). Same code, same semantics -- silence it.
if(WIN32)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS)
endif()

option(PS2X_BUILD_RECOMP "Build ps2xRecomp" ON)
option(PS2X_BUILD_RUNTIME "Build ps2xRuntime" ON)
option(PS2X_BUILD_ANALYZER "Build ps2xAnalyzer" ON)
option(PS2X_BUILD_TEST "Build ps2xTest" ON)
option(PS2X_BUILD_STUDIO "Build ps2xStudio" ON)

set(PS2X_IS_ARM_TARGET OFF)
set(PS2X_IS_AARCH64_TARGET OFF)
# CMAKE_SYSTEM_PROCESSOR describes the host when selecting a macOS cross-build.
# SIMD compile flags must instead follow the requested output architecture.
set(PS2X_TARGET_PROCESSOR "${CMAKE_SYSTEM_PROCESSOR}")
if(APPLE AND CMAKE_OSX_ARCHITECTURES)
    list(LENGTH CMAKE_OSX_ARCHITECTURES _ps2x_arch_count)
    if(NOT _ps2x_arch_count EQUAL 1)
        message(FATAL_ERROR "Build macOS arm64 and x86_64 separately; mixed SIMD universal builds are not supported")
    endif()
    set(PS2X_TARGET_PROCESSOR "${CMAKE_OSX_ARCHITECTURES}")
endif()
if(PS2X_TARGET_PROCESSOR MATCHES "arm64|aarch64|ARM64")
    set(PS2X_IS_ARM_TARGET ON)
    set(PS2X_IS_AARCH64_TARGET ON)
elseif(PS2X_TARGET_PROCESSOR MATCHES "^arm|^ARM")
    set(PS2X_IS_ARM_TARGET ON)
endif()

if((CMAKE_C_COMPILER MATCHES "arm-vita-eabi") OR
   (CMAKE_CXX_COMPILER MATCHES "arm-vita-eabi"))
    set(PS2X_IS_ARM_TARGET ON)
endif()

if(PS2X_IS_ARM_TARGET)
    message(STATUS "ARM target detected, fetching sse2neon")

    include(FetchContent)
    FetchContent_Declare(
        sse2neon
        GIT_REPOSITORY https://github.com/DLTcollab/sse2neon.git
        GIT_TAG v1.9.1
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(sse2neon)

    include_directories(${sse2neon_SOURCE_DIR})
    add_compile_definitions(USE_SSE2NEON)

    if(PS2X_IS_AARCH64_TARGET AND APPLE)
        # macOS ARM64 already uses optimal defaults
        message(STATUS "macOS ARM64 - using default compiler flags")
    elseif(PS2X_IS_AARCH64_TARGET AND MSVC)
        # Windows ARM64 - MSVC already uses optimal defaults
        message(STATUS "Windows ARM64 (MSVC) - using default compiler flags")
    elseif(PS2X_IS_AARCH64_TARGET)
        # Linux ARM64 - add NEON flags
        message(STATUS "Non-Apple ARM64 - adding NEON compiler flags")
        add_compile_options(-march=armv8-a+fp+simd)

        # Try to enable crypto and CRC extensions if supported
        include(CheckCXXCompilerFlag)
        check_cxx_compiler_flag("-march=armv8-a+fp+simd+crypto+crc" COMPILER_SUPPORTS_CRYPTO_CRC)
        if(COMPILER_SUPPORTS_CRYPTO_CRC)
            set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=armv8-a+fp+simd+crypto+crc")
            message(STATUS "Crypto and CRC extensions enabled")
        else()
            add_compile_options(-march=armv8-a+fp+simd)
        endif()
    endif()
endif()

if(PS2X_BUILD_RECOMP)
    add_subdirectory("ps2xRecomp")
endif()

if(PS2X_BUILD_RUNTIME)
    add_subdirectory("ps2xRuntime")
endif()

if(PS2X_BUILD_ANALYZER)
    add_subdirectory("ps2xAnalyzer")
endif()

if(PS2X_BUILD_TEST)
    add_subdirectory("ps2xTest")
endif()

if(PS2X_BUILD_STUDIO)
    add_subdirectory("ps2xStudio")
endif()
