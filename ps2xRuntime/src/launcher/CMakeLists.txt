cmake_minimum_required(VERSION 3.16)
project(bt3_launcher LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

include(FetchContent)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network)
find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core Gui Widgets Network)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core Gui Widgets Network)

# Cross-platform joystick backend for the controller test + bind capture.
# GLFW's own dependency guarantees the same mapping DB on Linux/Windows/macOS.
# X11-only on Linux (minimal build environments lack wayland-scanner); Windows and
# macOS pick their native backend automatically regardless of these flags.
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
if(NOT WIN32 AND NOT APPLE)
    set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_X11 ON CACHE BOOL "" FORCE)
endif()
FetchContent_Declare(glfw
    URL https://github.com/glfw/glfw/releases/download/3.4/glfw-3.4.zip)
FetchContent_MakeAvailable(glfw)

file(GLOB LAUNCHER_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

# --- libarchive: in-process 7z/zip/tar/rar extraction ---------------------
# The launcher extracts archives itself via libarchive, so the portable tree no
# longer needs a system 7z/unrar/tar and behaves identically on every platform.
# Prefer the system library; fall back to a pinned, statically-built copy so the
# result is reproducible on a bare distro / CI. ENABLE_LZMA must stay on: the
# 7z/LZMA2 texture pack and disc dumps need it.
set(LA_LINK "")
set(LA_INCLUDES "")
set(LA_EXTRA "")
find_package(LibArchive QUIET)
if(LibArchive_FOUND)
    set(LA_LINK ${LibArchive_LIBRARIES})
    set(LA_INCLUDES ${LibArchive_INCLUDE_DIRS})
else()
    message(STATUS "libarchive not found: fetching pinned static build (v3.8.1)")
    include(FetchContent)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(ENABLE_TEST OFF CACHE BOOL "" FORCE)
    set(ENABLE_TAR OFF CACHE BOOL "" FORCE)
    set(ENABLE_CPIO OFF CACHE BOOL "" FORCE)
    set(ENABLE_CAT OFF CACHE BOOL "" FORCE)
    # [lzma] libarchive needs liblzma to read 7z/LZMA2 (the texture pack). Windows has no
    # system liblzma, so fetch xz; then satisfy libarchive's find_package(LibLZMA) (CMake's
    # standard module) with xz's source tree + target. Pre-setting the module's REQUIRED_VARS
    # as cache entries makes its check_c_source_compiles honour them (the .lib does not exist
    # until build time). Without this the fetched libarchive builds LZMA-less and the texture
    # install fails with "lzma codec unsupported".
    FetchContent_Declare(liblzma
        URL https://github.com/tukaani-project/xz/releases/download/v5.6.2/xz-5.6.2.tar.gz
        # [xzpin] Pinned like libarchive below. Hash of the tarball whose .sig verifies against Lasse
        # Collin's release key (3690C240CE51B4670D30AD1C38EE757D69184620); 5.6.2 postdates the
        # 5.6.0/5.6.1 backdoor.
        URL_HASH SHA256=8bfd20c0e1d86f0402f2497cfa71c6ab62d4cd35fd704276e3140bfb71414519
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        EXCLUDE_FROM_ALL)   # only the liblzma target is needed; its tools/tests do not build under clang-cl
    set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(CREATE_XZ_SYMLINKS OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(liblzma)
    set(LIBLZMA_INCLUDE_DIR "${liblzma_SOURCE_DIR}/src/liblzma/api" CACHE PATH "" FORCE)
    set(LIBLZMA_LIBRARY liblzma CACHE STRING "" FORCE)   # a target name: resolved at link time
    set(LIBLZMA_HAS_AUTO_DECODER 1 CACHE INTERNAL "" FORCE)
    set(LIBLZMA_HAS_EASY_ENCODER 1 CACHE INTERNAL "" FORCE)
    set(LIBLZMA_HAS_LZMA_PRESET 1 CACHE INTERNAL "" FORCE)
    FetchContent_Declare(libarchive
        URL https://github.com/libarchive/libarchive/releases/download/v3.8.1/libarchive-3.8.1.tar.gz
        URL_HASH SHA256=bde832a5e3344dc723cfe9cc37f8e54bde04565bfe6f136bc1bd31ab352e9fab)
    FetchContent_MakeAvailable(libarchive)
    set(LA_LINK archive_static)
    # Make the xz target build before libarchive and make sure the launcher links it too:
    # archive_static is a static lib, so its LIBLZMA dependency does not propagate.
    if(TARGET archive_static)
        add_dependencies(archive_static liblzma)
    endif()
    set(LA_EXTRA liblzma)
endif()

set(LAUNCHER_RES "")
if(WIN32)
    set(LAUNCHER_RES "${CMAKE_CURRENT_SOURCE_DIR}/bt3.rc")
endif()
add_executable(Launcher ${LAUNCHER_SOURCES} ${LAUNCHER_RES})
# [windows] Qt GUI app under a WIN32 subsystem: without this the plain
# add_executable produces a console-subsystem exe and a terminal window pops up
# on every launch (the runtime suppresses its own via PS2X_SHOW_WINDOWS_CONSOLE).
if(WIN32)
    set_target_properties(Launcher PROPERTIES WIN32_EXECUTABLE TRUE)
endif()
if(APPLE)
    if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
        execute_process(COMMAND sw_vers -productVersion
            OUTPUT_VARIABLE CMAKE_OSX_DEPLOYMENT_TARGET OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE _macos_version_result)
        if(NOT _macos_version_result EQUAL 0)
            message(FATAL_ERROR "Could not determine macOS version; set CMAKE_OSX_DEPLOYMENT_TARGET")
        endif()
    endif()
    set_target_properties(Launcher PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "BT3-Recomp"
        MACOSX_BUNDLE_GUI_IDENTIFIER "org.bt3recomp.launcher"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "0.1.0"
        MACOSX_BUNDLE_BUNDLE_VERSION "1"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in")
    set(LAUNCHER_ASSET_DIR "$<TARGET_BUNDLE_CONTENT_DIR:Launcher>/Resources/assets")
else()
    set(LAUNCHER_ASSET_DIR "$<TARGET_FILE_DIR:Launcher>/assets")
endif()

target_include_directories(Launcher PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
    ${LA_INCLUDES})

target_link_libraries(Launcher PRIVATE
    Qt${QT_VERSION_MAJOR}::Core
    Qt${QT_VERSION_MAJOR}::Gui
    Qt${QT_VERSION_MAJOR}::Widgets
    Qt${QT_VERSION_MAJOR}::Network
    ${LA_LINK}
    ${LA_EXTRA}
    glfw)

# The pinned libarchive is a static library (archive_static builds it itself with
# LIBARCHIVE_STATIC, which is PRIVATE); archive_extract.cpp includes archive.h and
# would otherwise import every archive_* symbol with __declspec(dllimport), which
# lld-link cannot resolve against a .lib. Keep the same macro on the launcher side.
target_compile_definitions(Launcher PRIVATE LIBARCHIVE_STATIC)

if(NOT MSVC)
    target_compile_options(Launcher PRIVATE -Wall -Wextra)
endif()

# Deploy layout: the launcher binary sits next to the ELF game binary. The
# background/ font assets are copied alongside at a fixed relative path.
add_custom_command(TARGET Launcher POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "${LAUNCHER_ASSET_DIR}/fonts"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/RussoOne-Regular.ttf"
        "${LAUNCHER_ASSET_DIR}/fonts/RussoOne-Regular.ttf"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/PZS3US1.afl"
        "${LAUNCHER_ASSET_DIR}/PZS3US1.afl"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/PZS3US2.afl"
        "${LAUNCHER_ASSET_DIR}/PZS3US2.afl"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/background.png"
        "${LAUNCHER_ASSET_DIR}/background.png"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon.png"
        "${LAUNCHER_ASSET_DIR}/icon.png"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/NOTICE"
        "${LAUNCHER_ASSET_DIR}/NOTICE")
