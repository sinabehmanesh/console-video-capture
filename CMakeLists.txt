cmake_minimum_required(VERSION 3.20)

project(ps2_capture_stream LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(ps2-capture-stream
    src/main.cpp
)

target_compile_definitions(ps2-capture-stream PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
)

if (MSVC)
    target_compile_options(ps2-capture-stream PRIVATE
        /W4
        /permissive-
    )
elseif (MINGW)
    target_compile_options(ps2-capture-stream PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
endif()

target_link_libraries(ps2-capture-stream PRIVATE
    d3d11
    dxgi
    dxguid
)
