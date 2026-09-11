# PlayStation2 Capture Stream

A small native Windows C++ app for using a USB capture card as a low-latency PS2 display.

```text
PS2 -> HDMI adapter -> USB capture card -> Media Foundation -> Direct3D 11
                                  \-> WASAPI -> Windows audio output
```

## Features

- `720x480 @ 60 fps` YUY2 capture
- D3D11 rendering with 4:3, fixed-size and stretch modes
- Fixed output sizes: `640x480`, `960x720`, `1280x960`, `1920x1440`
- Borderless fullscreen
- HUD with output resolution, FPS and built-in help
- WASAPI audio passthrough
- Nearest, Bilinear and Sharp Bilinear scaling
- BT.601 / BT.709 and Limited / Full range controls
- Brightness, contrast, gamma and saturation controls
- Windows application icon and release builds through GitHub Actions

The current setup is tuned for the capture chain used during development, where `BT.709 + Full range` gives the best result.

## Controls

| Key | Action |
| --- | --- |
| `F11` | Toggle fullscreen |
| `Esc` | Leave fullscreen |
| `R` | Cycle output resolution |
| `Q` | Cycle scaling filter |
| `M` | Cycle display mode |
| `1` / `2` / `3` | 4:3 / Fixed / Stretch |
| `C` | BT.601 / BT.709 |
| `L` | Limited / Full range |
| `B` / `Shift+B` | Brightness +/- |
| `K` / `Shift+K` | Contrast +/- |
| `G` / `Shift+G` | Gamma +/- |
| `S` / `Shift+S` | Saturation +/- |
| `0` | Reset image settings |
| `H` | Show/hide help |

## Build

Use a Visual Studio Developer Command Prompt or Developer PowerShell:

```powershell
cmake -S . -B build -G "NMake Makefiles"
cmake --build build
.\build\ps2-capture-stream.exe
```

## Notes

This project is intentionally Windows-only and uses MSVC x64, Media Foundation, Direct3D 11 and WASAPI. It has no telemetry, updater or network dependency.

For tagged releases, see [RELEASING.md](RELEASING.md).
