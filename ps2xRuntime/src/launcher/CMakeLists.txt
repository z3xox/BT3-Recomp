cmake_minimum_required(VERSION 3.16)
project(bt3_launcher LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

include(FetchContent)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)
find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core Gui Widgets)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core Gui Widgets)

# Cross-platform joystick backend for the controller test + bind capture.
# GLFW's own dependency guarantees the same mapping DB on Linux/Windows/macOS.
# X11-only on Linux (the baseline container lacks wayland-scanner); Windows and
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

add_executable(Launcher ${LAUNCHER_SOURCES})
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
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include)

target_link_libraries(Launcher PRIVATE
    Qt${QT_VERSION_MAJOR}::Core
    Qt${QT_VERSION_MAJOR}::Gui
    Qt${QT_VERSION_MAJOR}::Widgets
    glfw)

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
