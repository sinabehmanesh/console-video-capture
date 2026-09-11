# ps2-capture-stream

Low-latency Windows capture-card preview for retro consoles.

## Goal

Use a USB HDMI capture card as a low-latency monitor for a PlayStation 2 while keeping control over aspect ratio and scaling on a modern display.

Target pipeline:

```text
PS2 -> HDMI adapter -> USB capture card -> Windows app -> monitor
```

The final application is intended to provide:

- low-latency capture preview
- 4:3 preservation
- fixed-size centered rendering with black borders
- fit / fill / 1:1-style scaling modes
- borderless fullscreen
- audio passthrough
- basic FPS / timing statistics

No network service, telemetry, or automatic updater is planned.

## Stage 1

Stage 1 establishes the rendering foundation only:

- native Win32 window
- Direct3D 11 device and swap chain
- resize-safe render target recreation
- immediate presentation loop
- GCC/MinGW-compatible CMake build

The window currently renders a dark background. Capture-card access is added in Stage 2.

## Build on Windows with GCC / MinGW-w64

Requirements:

- GCC/MinGW-w64 with C++20 support
- CMake 3.20+
- Windows SDK headers/libraries supplied by MinGW-w64

From a MinGW shell:

```bash
cmake -S . -B build -G "MinGW Makefiles"
cmake --build build -j
./build/ps2-capture-stream.exe
```

Or compile Stage 1 directly with `g++`:

```bash
g++ -std=c++20 -Wall -Wextra -Wpedantic src/main.cpp -ld3d11 -ldxgi -o ps2-capture-stream.exe
```

## Development plan

1. Win32 + Direct3D 11 foundation
2. Media Foundation capture-device discovery
3. Live video frame capture and rendering
4. Low-latency tuning and latest-frame-wins buffering
5. 4:3, fit/fill, fixed-size centered rendering
6. Audio passthrough
7. Fullscreen, hotkeys, settings, and timing stats
8. Release packaging
