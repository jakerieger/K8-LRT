cmake_minimum_required(VERSION 3.14)
project(K8Tool LANGUAGES C CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(SRC_DIR ${CMAKE_SOURCE_DIR}/src)
set(RES_DIR ${CMAKE_SOURCE_DIR}/res)

set(VER_MAJOR 3)
set(VER_MINOR 0)
set(VER_PATCH 0)

if (NOT EXISTS "${CMAKE_SOURCE_DIR}/.build")
    file(WRITE "${CMAKE_SOURCE_DIR}/.build" "1")
endif ()

file(READ "${CMAKE_SOURCE_DIR}/.build" VER_BUILD)
string(STRIP "${VER_BUILD}" VER_BUILD)

math(EXPR VER_BUILD "${VER_BUILD} + 1")
file(WRITE "${CMAKE_SOURCE_DIR}/.build" "${VER_BUILD}")

configure_file(
        "${SRC_DIR}/version.h.in"
        "${SRC_DIR}/version.h"
)

add_executable(K8Tool WIN32
        ${RES_DIR}/app.rc
        ${SRC_DIR}/main.cpp
)

target_include_directories(K8Tool PRIVATE
        ${SRC_DIR}
        ${CMAKE_SOURCE_DIR}/vendor
)

target_link_options(K8Tool PRIVATE "/MANIFEST:NO")

target_compile_definitions(K8Tool PRIVATE
        $<$<CONFIG:Debug>:_DEBUG>
        $<$<CONFIG:Release>:NDEBUG>
)

set_target_properties(K8Tool PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

target_link_libraries(K8Tool PRIVATE
        user32
        gdi32 advapi32
        shell32
        ole32
        shlwapi
        comctl32
        winhttp
        dwmapi
)