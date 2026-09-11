cmake_minimum_required(VERSION 3.20)

project(ps2_capture_stream LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(ps2-capture-stream
    src/main.cpp
    src/audio_passthrough.cpp
    src/capture_devices.cpp
    src/capture_formats.cpp
    src/capture_session.cpp
    src/hud_renderer.cpp
    resources/app.rc
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
        $<$<COMPILE_LANGUAGE:CXX>:/FI"${CMAKE_CURRENT_SOURCE_DIR}/src/app_icon.h">
    )
elseif (MINGW)
    target_compile_options(ps2-capture-stream PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
endif()

target_link_libraries(ps2-capture-stream PRIVATE
    avrt
    d3d11
    d3dcompiler
    dxgi
    dxguid
    mf
    mfplat
    mfreadwrite
    mfuuid
    ole32
)
