cmake_minimum_required(VERSION 3.20)

project(console_video_capture LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(console-video-capture
    src/entry.cpp
    src/main.cpp
    src/audio_passthrough.cpp
    src/capture_devices.cpp
    src/capture_formats.cpp
    src/capture_session.cpp
    src/hud_renderer.cpp
    resources/app.rc
)

# Keep the video application intact, but rename its entry point so entry.cpp
# can choose between full video+audio capture and audio-only mode.
set_source_files_properties(src/main.cpp PROPERTIES
    COMPILE_DEFINITIONS "main=video_main"
)

target_compile_definitions(console-video-capture PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
)

if (MSVC)
    target_compile_options(console-video-capture PRIVATE
        /W4
        /permissive-
        $<$<COMPILE_LANGUAGE:CXX>:/FI${CMAKE_CURRENT_SOURCE_DIR}/src/app_icon.h>
    )
elseif (MINGW)
    target_compile_options(console-video-capture PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
endif()

target_link_libraries(console-video-capture PRIVATE
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
