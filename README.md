# PlayStation2 Capture Stream

A native Windows C++ utility for low-latency preview of a PlayStation 2 through a USB capture card.

Pipeline:

```text
PS2 -> HDMI adapter -> USB capture card -> Media Foundation -> Direct3D 11 -> display
                                  \-> WASAPI -> default Windows audio output
```

## Current status

Stage 10 is implemented and validated on the current feature branch:

- Win32 window
- Direct3D 11 swap chain and rendering
- Media Foundation device discovery
- Native capture format enumeration
- Live `720x480 @ 60 fps YUY2` capture from the target capture card
- Capture-card selection by stable USB hardware ID (`VID_345F:PID_2131`)
- Latest-frame handoff from the capture thread
- GPU upload of packed YUY2 frames
- D3D11 pixel-shader YUV -> RGB conversion
- Live video preview in the application window
- Centered 4:3 fit mode with black borders
- Selectable fixed output sizes: `640x480`, `960x720`, `1280x960`, `1920x1440`
- Minimum window size follows the selected fixed output size
- Stretch mode for comparison/debugging
- Borderless fullscreen on the current monitor
- Window size and position restored when leaving fullscreen
- D3D-rendered HUD with selected output resolution and capture FPS
- WASAPI capture-card audio passthrough to the current default Windows output device
- Audio thread promoted to the Windows `Pro Audio` MMCSS task when available
- Selectable scaling filters: Nearest, Bilinear, and Sharp bilinear
- Live color-matrix selection: BT.601 / BT.709
- Live input-range selection: Limited / Full
- GPU-side brightness, contrast, gamma, and saturation controls
- Validated default color path for the current PS2/HDMI/capture-card chain: `BT.709 + Full range`

### Display hotkeys

- `F11` - toggle borderless fullscreen
- `Esc` - leave fullscreen
- `R` - cycle fixed output resolution
- `Q` - cycle scaling filters: Nearest / Bilinear / Sharp bilinear
- `M` - cycle display modes
- `1` - Fit 4:3
- `2` - Fixed resolution
- `3` - Stretch

### Image hotkeys

- `C` - toggle BT.601 / BT.709 color matrix
- `L` - toggle Limited / Full input range
- `B` - increase brightness
- `Shift+B` - decrease brightness
- `K` - increase contrast
- `Shift+K` - decrease contrast
- `G` - increase gamma
- `Shift+G` - decrease gamma
- `S` - increase saturation
- `Shift+S` - decrease saturation
- `0` - reset image controls to the validated defaults

Current defaults:

```text
Matrix:     BT.709
Range:      Full
Brightness: 0.0
Contrast:   1.0
Gamma:      1.0
Saturation: 1.0
Scaling:    Bilinear
```

The scaling-filter differences can be subtle because the upstream PS2-to-HDMI adapter and capture chain already perform their own processing. The application cannot reconstruct detail that is lost before capture.

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
6. Scaling/aspect-ratio modes
7. Borderless fullscreen and selectable fixed output sizes
8. WASAPI audio passthrough
9. Image-quality controls and scaling filters
10. Latency/statistics tuning
11. Packaging/release

## Design constraints

- Native Windows application
- MSVC x64 toolchain
- No network dependency
- No telemetry
- No auto-updater
- No mandatory installer
