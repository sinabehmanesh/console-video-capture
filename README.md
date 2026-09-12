# console-video-capture

A small native Windows C++ capture-card playground for getting a console signal onto the PC without dragging OBS into the room.

The basic pipe is:

```text
console -> HDMI -> capture card -> USB -> Media Foundation -> D3D11 -> pixels
                           |
                           +-> USB audio -> WASAPI -> Windows output
```

Or, when the capture card's USB video path is being a goblin:

```text
console -> capture card -> HDMI passthrough -> monitor
                     \
                      +-> USB audio -> WASAPI -> Windows output
```

No Electron. No browser. No account. No cloud. Just Win32, Media Foundation, Direct3D 11, WASAPI, and an unreasonable amount of staring at YUV conversion code.

## Hardware I've actually thrown at it

Tested end-to-end with:

- **PlayStation 2** through an HDMI adapter
- **Xbox 360** over HDMI

It is not doing anything magical or console-specific at the protocol level, so other consoles should work too if they give the capture card a signal it can expose through the same capture path. HDMI-era boxes are the obvious candidates; older analog consoles will need whatever converter/upscaler nonsense gets them to HDMI first.

The capture-card matcher is currently tuned to the device used while hacking on this (`VID_345F:PID_2131`). Other capture cards may expose compatible UVC/Media Foundation streams, but the device-selection code will probably need a tiny poke before they are picked automatically.

## What it does

At startup there are two top-level modes:

```text
[1] Video + audio capture
[2] Audio passthrough only
```

Video mode currently exposes these capture paths:

```text
[1] 1280x720  @ 60 fps NV12   - smoothest motion, less detail
[2] 1920x1080 @ 50 fps NV12   - default / best 1080p motion
[3] 1920x1080 @ 30 fps NV12   - full 1080p, lower frame rate
[4] 1920x1080 @ 50 fps MJPEG  - alternate USB path, decoded to NV12
[5] 1920x1080 @ 30 fps MJPEG  - alternate USB path, decoded to NV12
```

Frames end up in a D3D11 shader where NV12 is converted to RGB. There are controls for aspect ratio, output size, scaling, BT.601/BT.709, Limited/Full range, brightness, contrast, gamma, saturation, and chroma reconstruction.

The audio-only mode intentionally does not start video capture or D3D at all. It just takes the capture card's USB audio endpoint and pushes it to the current Windows default output. This is useful when HDMI passthrough looks cleaner than the USB video stream, which is exactly what happened with the card used during development.

## Known weirdness

The tested card's HDMI passthrough looks cleaner than its USB capture output. The USB stream shows some color/edge artifacts that survive both native NV12 and MJPEG capture paths, so this appears to be upstream of the renderer rather than something we can bully out of the shader.

So: this tool can be a nice low-latency preview, but if your card's HDMI passthrough is better, use it. Physics and cheap capture silicon win sometimes.

## Hotkeys

Press `H` in the video window for the built-in cheat sheet.

| Key | Action |
| --- | --- |
| `F11` | Toggle borderless fullscreen |
| `Esc` | Leave fullscreen |
| `R` | Cycle output resolution |
| `A` | Toggle 4:3 / 16:9 |
| `Q` | Cycle Nearest / Bilinear / Sharp Bilinear |
| `U` | Toggle chroma reconstruction mode |
| `C` | Toggle BT.601 / BT.709 |
| `L` | Toggle Limited / Full range |
| `B` / `Shift+B` | Brightness +/- |
| `K` / `Shift+K` | Contrast +/- |
| `G` / `Shift+G` | Gamma +/- |
| `S` / `Shift+S` | Saturation +/- |
| `0` | Reset image settings |
| `M` | Cycle display mode |
| `1` | Fit aspect ratio |
| `2` | Fixed resolution |
| `3` | Stretch |
| `H` | Show/hide help |

## Build the thing

Use a Visual Studio Developer Command Prompt or Developer PowerShell:

```powershell
cmake -S . -B build -G "NMake Makefiles"
cmake --build build
.\build\console-video-capture.exe
```

Release builds are produced by GitHub Actions from version tags. See [RELEASING.md](RELEASING.md).

## Stack

- C++20
- Win32
- Media Foundation
- Direct3D 11 / HLSL
- WASAPI
- CMake
- an HDMI cable spaghetti monster

Windows-only for now. There is no telemetry, updater, server component, login screen, or mysterious daemon phoning home at 03:00.
