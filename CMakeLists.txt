cmake_minimum_required(VERSION 3.14)
project(K8-LRT VERSION 2.0.0 LANGUAGES C RC)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(SRC_DIR ${CMAKE_SOURCE_DIR}/src)
set(RES_DIR ${CMAKE_SOURCE_DIR}/res)

add_executable(K8-LRT WIN32
    ${RES_DIR}/app.rc
    ${SRC_DIR}/main.c
)

target_link_options(K8-LRT PRIVATE "/MANIFEST:NO")

target_compile_definitions(K8-LRT PRIVATE
    $<$<CONFIG:Debug>:_DEBUG>
    $<$<CONFIG:Release>:NDEBUG>
)

set_target_properties(K8-LRT PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

target_link_libraries(K8-LRT PRIVATE
    user32
    gdi32
    advapi32
    shell32
    ole32
    shlwapi
    comctl32
    winhttp
    pathcch
)