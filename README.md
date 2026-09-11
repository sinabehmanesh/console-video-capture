# PS2 Capture Stream

A native Windows C++ utility for low-latency preview of a PlayStation 2 through a USB capture card.

Pipeline:

```text
PS2 -> HDMI adapter -> USB capture card -> Media Foundation -> Direct3D 11 -> display
```

## Current status

Stage 5 is implemented on `feature/low-latency-capture-preview`:

- Win32 window
- Direct3D 11 swap chain and rendering
- Media Foundation device discovery
- Native capture format enumeration
- Live `1920x1080 @ 60 fps YUY2` capture from `USB3 Video`
- Latest-frame handoff from the capture thread
- GPU upload of packed YUY2 frames
- D3D11 pixel-shader YUV -> RGB conversion
- Live video preview in the application window

The current renderer intentionally shows the raw capture frame stretched to the window. Aspect-ratio modes, fixed-size centered output, fullscreen UX, audio passthrough, and deeper latency tuning are later stages.

## Build with MSVC

From a Visual Studio Developer Command Prompt or Developer PowerShell:

```powershell
cmake -S . -B build -G "NMake Makefiles"
cmake --build build
```

Run:

```powershell
.\build\ps2-capture-stream.exe
```

## Roadmap

1. Win32 + D3D11 foundation
2. Media Foundation capture-device discovery
3. Capture format enumeration
4. Live low-latency frame capture
5. GPU YUY2 rendering
6. Scaling/aspect-ratio modes and borderless fullscreen
7. Audio passthrough and latency/statistics tuning
8. Packaging/release

## Design constraints

- Native Windows application
- MSVC x64 toolchain
- No network dependency
- No telemetry
- No auto-updater
- No mandatory installer
