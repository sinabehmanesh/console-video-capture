#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <mfapi.h>
#include <objbase.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "audio_passthrough.h"
#include "capture_devices.h"
#include "capture_session.h"
#include "hud_renderer.h"

namespace {

constexpr wchar_t kWindowClassName[] = L"ConsoleVideoCaptureWindowClass";
constexpr wchar_t kWindowTitle[] = L"Console Video Capture - Native NV12";
constexpr wchar_t kCaptureHardwareId[] = L"vid_345f&pid_2131";

struct OutputResolution {
    UINT width;
    UINT height;
};

constexpr std::array<OutputResolution, 5> kOutputResolutions{{
    {640, 480},
    {960, 720},
    {1280, 720},
    {1920, 1080},
    {2560, 1440},
}};

enum class DisplayMode {
    FitAspectRatio,
    FixedResolution,
    Stretch,
};

enum class AspectRatioMode {
    Ratio4x3,
    Ratio16x9,
};

enum class ScalingFilter : std::uint32_t {
    Nearest = 0,
    Bilinear = 1,
    SharpBilinear = 2,
};

enum class ColorMatrix : std::uint32_t {
    BT601 = 0,
    BT709 = 1,
};

enum class InputRange : std::uint32_t {
    Limited = 0,
    Full = 1,
};

enum class ChromaFilter : std::uint32_t {
    Nearest = 0,
    BilinearCentered = 1,
};

struct alignas(16) ImageSettingsGpu {
    float brightness;
    float contrast;
    float gamma;
    float saturation;
    std::uint32_t color_matrix;
    std::uint32_t input_range;
    std::uint32_t filter_mode;
    std::uint32_t chroma_filter;
};
static_assert(sizeof(ImageSettingsGpu) % 16 == 0);

struct WindowState {
    bool fullscreen = false;
    LONG_PTR style = 0;
    LONG_PTR ex_style = 0;
    WINDOWPLACEMENT placement{sizeof(WINDOWPLACEMENT)};
};

DisplayMode g_display_mode = DisplayMode::FixedResolution;
AspectRatioMode g_aspect_ratio_mode = AspectRatioMode::Ratio4x3;
ScalingFilter g_scaling_filter = ScalingFilter::Bilinear;
ChromaFilter g_chroma_filter = ChromaFilter::BilinearCentered;
ColorMatrix g_color_matrix = ColorMatrix::BT709;
InputRange g_input_range = InputRange::Limited;
float g_brightness = 0.0f;
float g_contrast = 1.0f;
float g_gamma = 1.0f;
float g_saturation = 1.0f;
std::size_t g_output_resolution_index = 2;
WindowState g_window_state;
UINT g_hud_fps = 0;
UINT g_capture_width = 0;
UINT g_capture_height = 0;
HudRenderer g_hud_renderer;

constexpr char kVertexShaderSource[] = R"(
struct VSOut {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

VSOut main(uint vertex_id : SV_VertexID) {
    VSOut output;
    if (vertex_id == 0) {
        output.position = float4(-1.0, -1.0, 0.0, 1.0);
        output.uv = float2(0.0, 1.0);
    } else if (vertex_id == 1) {
        output.position = float4(-1.0, 3.0, 0.0, 1.0);
        output.uv = float2(0.0, -1.0);
    } else {
        output.position = float4(3.0, -1.0, 0.0, 1.0);
        output.uv = float2(2.0, 1.0);
    }
    return output;
}
)";

constexpr char kPixelShaderSource[] = R"(
Texture2D<float> nv12_texture : register(t0);

cbuffer ImageSettings : register(b0) {
    float brightness;
    float contrast;
    float gamma_value;
    float saturation;
    uint color_matrix;
    uint input_range;
    uint filter_mode;
    uint chroma_filter;
};

struct PSIn {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

void source_dimensions(out uint source_width, out uint source_height) {
    uint packed_height = 0;
    nv12_texture.GetDimensions(source_width, packed_height);
    source_height = (packed_height * 2) / 3;
}

float3 yuv_to_rgb(float y, float u, float v) {
    float3 rgb;

    if (input_range == 0) {
        float luma = 1.164383 * (y - (16.0 / 255.0));
        if (color_matrix == 0) {
            rgb.r = luma + 1.596027 * v;
            rgb.g = luma - 0.391762 * u - 0.812968 * v;
            rgb.b = luma + 2.017232 * u;
        } else {
            rgb.r = luma + 1.792741 * v;
            rgb.g = luma - 0.213249 * u - 0.532909 * v;
            rgb.b = luma + 2.112402 * u;
        }
    } else {
        if (color_matrix == 0) {
            rgb.r = y + 1.402000 * v;
            rgb.g = y - 0.344136 * u - 0.714136 * v;
            rgb.b = y + 1.772000 * u;
        } else {
            rgb.r = y + 1.574800 * v;
            rgb.g = y - 0.187324 * u - 0.468124 * v;
            rgb.b = y + 1.855600 * u;
        }
    }

    return rgb;
}

float2 load_chroma_nearest(uint source_x, uint source_y, uint source_width, uint source_height) {
    uint uv_x = min((source_x / 2) * 2, source_width - 2);
    uint uv_y = source_height + source_y / 2;
    return float2(
        nv12_texture.Load(int3(uv_x, uv_y, 0)),
        nv12_texture.Load(int3(uv_x + 1, uv_y, 0))
    );
}

float2 load_chroma_sample(int cx, int cy, uint source_width, uint source_height) {
    const int chroma_width = int(source_width / 2);
    const int chroma_height = int(source_height / 2);
    cx = clamp(cx, 0, chroma_width - 1);
    cy = clamp(cy, 0, chroma_height - 1);
    const uint uv_x = uint(cx * 2);
    const uint uv_y = source_height + uint(cy);
    return float2(
        nv12_texture.Load(int3(uv_x, uv_y, 0)),
        nv12_texture.Load(int3(uv_x + 1, uv_y, 0))
    );
}

float2 load_chroma_bilinear_centered(uint source_x, uint source_y, uint source_width, uint source_height) {
    float2 chroma_pos = (float2(source_x, source_y) - 0.5) * 0.5;
    int2 p0 = int2(floor(chroma_pos));
    float2 f = frac(chroma_pos);

    float2 c00 = load_chroma_sample(p0.x,     p0.y,     source_width, source_height);
    float2 c10 = load_chroma_sample(p0.x + 1, p0.y,     source_width, source_height);
    float2 c01 = load_chroma_sample(p0.x,     p0.y + 1, source_width, source_height);
    float2 c11 = load_chroma_sample(p0.x + 1, p0.y + 1, source_width, source_height);

    return lerp(lerp(c00, c10, f.x), lerp(c01, c11, f.x), f.y);
}

float3 decode_video(uint source_x, uint source_y) {
    uint source_width = 0;
    uint source_height = 0;
    source_dimensions(source_width, source_height);

    source_x = min(source_x, source_width - 1);
    source_y = min(source_y, source_height - 1);

    float y = nv12_texture.Load(int3(source_x, source_y, 0));
    float2 uv = chroma_filter == 0
        ? load_chroma_nearest(source_x, source_y, source_width, source_height)
        : load_chroma_bilinear_centered(source_x, source_y, source_width, source_height);

    float u = uv.x - (128.0 / 255.0);
    float v = uv.y - (128.0 / 255.0);
    return yuv_to_rgb(y, u, v);
}

float3 sample_bilinear(float2 source_pos, uint source_width, uint source_height) {
    float2 max_coord = float2(source_width - 1, source_height - 1);
    source_pos = clamp(source_pos, float2(0.0, 0.0), max_coord);

    uint x0 = (uint)floor(source_pos.x);
    uint y0 = (uint)floor(source_pos.y);
    uint x1 = min(x0 + 1, source_width - 1);
    uint y1 = min(y0 + 1, source_height - 1);
    float2 fraction = frac(source_pos);

    float3 top = lerp(decode_video(x0, y0), decode_video(x1, y0), fraction.x);
    float3 bottom = lerp(decode_video(x0, y1), decode_video(x1, y1), fraction.x);
    return lerp(top, bottom, fraction.y);
}

float3 apply_image_controls(float3 rgb) {
    rgb = (rgb - 0.5) * contrast + 0.5 + brightness;
    float luminance = dot(rgb, float3(0.299, 0.587, 0.114));
    rgb = lerp(luminance.xxx, rgb, saturation);
    rgb = pow(saturate(rgb), 1.0 / max(gamma_value, 0.1));
    return saturate(rgb);
}

float4 main(PSIn input) : SV_TARGET {
    uint source_width = 0;
    uint source_height = 0;
    source_dimensions(source_width, source_height);

    float2 source_pos = saturate(input.uv) * float2(source_width, source_height) - 0.5;
    float3 rgb;

    if (filter_mode == 0) {
        uint x = min((uint)max(round(source_pos.x), 0.0), source_width - 1);
        uint y = min((uint)max(round(source_pos.y), 0.0), source_height - 1);
        rgb = decode_video(x, y);
    } else {
        rgb = sample_bilinear(source_pos, source_width, source_height);

        if (filter_mode == 2) {
            uint x = min((uint)max(round(source_pos.x), 0.0), source_width - 1);
            uint y = min((uint)max(round(source_pos.y), 0.0), source_height - 1);
            uint left = x > 0 ? x - 1 : 0;
            uint right = min(x + 1, source_width - 1);
            uint up = y > 0 ? y - 1 : 0;
            uint down = min(y + 1, source_height - 1);

            float3 neighbours = (
                decode_video(left, y) +
                decode_video(right, y) +
                decode_video(x, up) +
                decode_video(x, down)
            ) * 0.25;

            rgb += 0.65 * (rgb - neighbours);
        }
    }

    return float4(apply_image_controls(rgb), 1.0);
}
)";

struct D3DState {
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    IDXGISwapChain* swap_chain = nullptr;
    ID3D11RenderTargetView* render_target = nullptr;
    ID3D11VertexShader* video_vertex_shader = nullptr;
    ID3D11PixelShader* video_pixel_shader = nullptr;
    ID3D11Buffer* image_settings_buffer = nullptr;
    ID3D11Texture2D* nv12_texture = nullptr;
    ID3D11ShaderResourceView* nv12_srv = nullptr;
    UINT viewport_width = 0;
    UINT viewport_height = 0;

    ~D3DState() {
        if (context != nullptr) context->ClearState();
        if (nv12_srv != nullptr) nv12_srv->Release();
        if (nv12_texture != nullptr) nv12_texture->Release();
        if (image_settings_buffer != nullptr) image_settings_buffer->Release();
        if (video_pixel_shader != nullptr) video_pixel_shader->Release();
        if (video_vertex_shader != nullptr) video_vertex_shader->Release();
        if (render_target != nullptr) render_target->Release();
        if (swap_chain != nullptr) swap_chain->Release();
        if (context != nullptr) context->Release();
        if (device != nullptr) device->Release();
    }
};

class MediaFoundationRuntime {
public:
    MediaFoundationRuntime() {
        const HRESULT com_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(com_result)) throw std::runtime_error("Failed to initialize COM");
        com_initialized_ = true;

        const HRESULT mf_result = MFStartup(MF_VERSION, MFSTARTUP_FULL);
        if (FAILED(mf_result)) {
            CoUninitialize();
            com_initialized_ = false;
            throw std::runtime_error("Failed to initialize Media Foundation");
        }
        mf_initialized_ = true;
    }

    ~MediaFoundationRuntime() {
        if (mf_initialized_) MFShutdown();
        if (com_initialized_) CoUninitialize();
    }

private:
    bool com_initialized_ = false;
    bool mf_initialized_ = false;
};

D3DState g_d3d;

void throw_if_failed(HRESULT result, const char* message) {
    if (FAILED(result)) throw std::runtime_error(std::string(message));
}

const OutputResolution& selected_output_resolution() {
    return kOutputResolutions[g_output_resolution_index];
}

float selected_display_aspect() {
    return g_aspect_ratio_mode == AspectRatioMode::Ratio16x9 ? 16.0f / 9.0f : 4.0f / 3.0f;
}

const char* aspect_ratio_name() {
    return g_aspect_ratio_mode == AspectRatioMode::Ratio16x9 ? "16:9" : "4:3";
}

const char* scaling_filter_name() {
    switch (g_scaling_filter) {
    case ScalingFilter::Nearest: return "Nearest";
    case ScalingFilter::Bilinear: return "Bilinear";
    case ScalingFilter::SharpBilinear: return "Sharp bilinear";
    }
    return "Unknown";
}

const char* chroma_filter_name() {
    return g_chroma_filter == ChromaFilter::Nearest ? "Nearest" : "Bilinear centered";
}

const char* color_matrix_name() {
    return g_color_matrix == ColorMatrix::BT601 ? "BT.601" : "BT.709";
}

const char* input_range_name() {
    return g_input_range == InputRange::Limited ? "Limited" : "Full";
}

void print_image_settings() {
    std::cout << "Image: matrix=" << color_matrix_name()
              << " range=" << input_range_name()
              << " brightness=" << g_brightness
              << " contrast=" << g_contrast
              << " gamma=" << g_gamma
              << " saturation=" << g_saturation << '\n';
    std::cout << "Chroma reconstruction: " << chroma_filter_name() << '\n';
}

void reset_image_settings() {
    g_color_matrix = ColorMatrix::BT709;
    g_input_range = InputRange::Limited;
    g_chroma_filter = ChromaFilter::BilinearCentered;
    g_brightness = 0.0f;
    g_contrast = 1.0f;
    g_gamma = 1.0f;
    g_saturation = 1.0f;
    std::cout << "Image settings reset.\n";
    print_image_settings();
}

void adjust_setting(float& value, float delta, float minimum, float maximum, const char* name) {
    value = std::clamp(value + delta, minimum, maximum);
    std::cout << name << ": " << value << '\n';
}

void update_hud_text() {
    const auto& output = selected_output_resolution();
    const std::string text =
        std::to_string(g_capture_width) + "x" + std::to_string(g_capture_height) + " CAP  " +
        std::to_string(output.width) + "x" + std::to_string(output.height) + " OUT  " +
        aspect_ratio_name() + "  " + std::to_string(g_hud_fps) + " FPS";
    g_hud_renderer.set_text(text);
}

void update_hud_fps(CaptureSession& capture) {
    using clock = std::chrono::steady_clock;
    static auto last_update = clock::now();
    static std::uint64_t last_frame_count = capture.frame_count();

    const auto now = clock::now();
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_update).count();
    if (elapsed_ms < 1000) return;

    const std::uint64_t frame_count = capture.frame_count();
    const std::uint64_t frame_delta = frame_count - last_frame_count;
    const double fps = static_cast<double>(frame_delta) * 1000.0 / static_cast<double>(elapsed_ms);
    g_hud_fps = static_cast<UINT>(fps + 0.5);
    update_hud_text();

    last_frame_count = frame_count;
    last_update = now;
}

const char* display_mode_name(DisplayMode mode) {
    switch (mode) {
    case DisplayMode::FitAspectRatio: return "Fit aspect ratio";
    case DisplayMode::FixedResolution: return "Fixed resolution";
    case DisplayMode::Stretch: return "Stretch";
    }
    return "Unknown";
}

void print_selected_resolution() {
    const auto& resolution = selected_output_resolution();
    std::cout << "Output resolution bounds: " << resolution.width << 'x' << resolution.height << '\n';
}

void cycle_scaling_filter() {
    switch (g_scaling_filter) {
    case ScalingFilter::Nearest:
        g_scaling_filter = ScalingFilter::Bilinear;
        break;
    case ScalingFilter::Bilinear:
        g_scaling_filter = ScalingFilter::SharpBilinear;
        break;
    case ScalingFilter::SharpBilinear:
        g_scaling_filter = ScalingFilter::Nearest;
        break;
    }
    std::cout << "Scaling filter: " << scaling_filter_name() << '\n';
}

void toggle_chroma_filter() {
    g_chroma_filter = g_chroma_filter == ChromaFilter::Nearest
        ? ChromaFilter::BilinearCentered
        : ChromaFilter::Nearest;
    std::cout << "Chroma reconstruction: " << chroma_filter_name() << '\n';
}

void toggle_color_matrix() {
    g_color_matrix = g_color_matrix == ColorMatrix::BT601 ? ColorMatrix::BT709 : ColorMatrix::BT601;
    std::cout << "Color matrix: " << color_matrix_name() << '\n';
}

void toggle_input_range() {
    g_input_range = g_input_range == InputRange::Limited ? InputRange::Full : InputRange::Limited;
    std::cout << "Input range: " << input_range_name() << '\n';
}

void toggle_aspect_ratio() {
    g_aspect_ratio_mode = g_aspect_ratio_mode == AspectRatioMode::Ratio4x3
        ? AspectRatioMode::Ratio16x9
        : AspectRatioMode::Ratio4x3;
    std::cout << "Aspect ratio: " << aspect_ratio_name() << '\n';
    update_hud_text();
}

void cycle_display_mode() {
    switch (g_display_mode) {
    case DisplayMode::FitAspectRatio:
        g_display_mode = DisplayMode::FixedResolution;
        break;
    case DisplayMode::FixedResolution:
        g_display_mode = DisplayMode::Stretch;
        break;
    case DisplayMode::Stretch:
        g_display_mode = DisplayMode::FitAspectRatio;
        break;
    }
    std::cout << "Display mode: " << display_mode_name(g_display_mode) << '\n';
}

void ensure_window_is_large_enough(HWND window) {
    if (g_window_state.fullscreen) return;

    const auto& resolution = selected_output_resolution();
    RECT client_rect{};
    GetClientRect(window, &client_rect);

    if ((client_rect.right - client_rect.left) >= static_cast<LONG>(resolution.width) &&
        (client_rect.bottom - client_rect.top) >= static_cast<LONG>(resolution.height)) {
        return;
    }

    RECT window_rect{0, 0, static_cast<LONG>(resolution.width), static_cast<LONG>(resolution.height)};
    const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE));
    const DWORD ex_style = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_EXSTYLE));
    AdjustWindowRectEx(&window_rect, style, FALSE, ex_style);

    SetWindowPos(
        window,
        nullptr,
        0,
        0,
        window_rect.right - window_rect.left,
        window_rect.bottom - window_rect.top,
        SWP_NOMOVE | SWP_NOZORDER | SWP_NOOWNERZORDER
    );
}

void cycle_output_resolution(HWND window) {
    g_output_resolution_index = (g_output_resolution_index + 1) % kOutputResolutions.size();
    g_display_mode = DisplayMode::FixedResolution;
    print_selected_resolution();
    update_hud_text();
    ensure_window_is_large_enough(window);
}

void enter_borderless_fullscreen(HWND window) {
    if (g_window_state.fullscreen) return;

    g_window_state.style = GetWindowLongPtrW(window, GWL_STYLE);
    g_window_state.ex_style = GetWindowLongPtrW(window, GWL_EXSTYLE);
    g_window_state.placement.length = sizeof(WINDOWPLACEMENT);

    if (!GetWindowPlacement(window, &g_window_state.placement)) {
        throw std::runtime_error("Failed to save window placement");
    }

    MONITORINFO monitor_info{};
    monitor_info.cbSize = sizeof(monitor_info);
    const HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    if (monitor == nullptr || !GetMonitorInfoW(monitor, &monitor_info)) {
        throw std::runtime_error("Failed to query current monitor");
    }

    SetWindowLongPtrW(window, GWL_STYLE, g_window_state.style & ~WS_OVERLAPPEDWINDOW);

    if (!SetWindowPos(
            window,
            HWND_TOP,
            monitor_info.rcMonitor.left,
            monitor_info.rcMonitor.top,
            monitor_info.rcMonitor.right - monitor_info.rcMonitor.left,
            monitor_info.rcMonitor.bottom - monitor_info.rcMonitor.top,
            SWP_NOOWNERZORDER | SWP_FRAMECHANGED
        )) {
        throw std::runtime_error("Failed to enter borderless fullscreen");
    }

    g_window_state.fullscreen = true;
    std::cout << "Fullscreen: on\n";
}

void exit_borderless_fullscreen(HWND window) {
    if (!g_window_state.fullscreen) return;

    SetWindowLongPtrW(window, GWL_STYLE, g_window_state.style);
    SetWindowLongPtrW(window, GWL_EXSTYLE, g_window_state.ex_style);

    if (!SetWindowPlacement(window, &g_window_state.placement)) {
        throw std::runtime_error("Failed to restore window placement");
    }

    SetWindowPos(
        window,
        nullptr,
        0,
        0,
        0,
        0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED
    );

    g_window_state.fullscreen = false;
    ensure_window_is_large_enough(window);
    std::cout << "Fullscreen: off\n";
}

void toggle_borderless_fullscreen(HWND window) {
    if (g_window_state.fullscreen) exit_borderless_fullscreen(window);
    else enter_borderless_fullscreen(window);
}

D3D11_VIEWPORT calculate_video_viewport() {
    D3D11_VIEWPORT viewport{};
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;

    const float client_width = static_cast<float>(g_d3d.viewport_width);
    const float client_height = static_cast<float>(g_d3d.viewport_height);

    if (g_display_mode == DisplayMode::Stretch) {
        viewport.Width = client_width;
        viewport.Height = client_height;
        return viewport;
    }

    float bounds_width = client_width;
    float bounds_height = client_height;

    if (g_display_mode == DisplayMode::FixedResolution) {
        const auto& resolution = selected_output_resolution();
        bounds_width = std::min(client_width, static_cast<float>(resolution.width));
        bounds_height = std::min(client_height, static_cast<float>(resolution.height));
    }

    const float display_aspect = selected_display_aspect();
    float width = bounds_width;
    float height = bounds_height;

    if (bounds_width / bounds_height > display_aspect) width = bounds_height * display_aspect;
    else height = bounds_width / display_aspect;

    viewport.TopLeftX = std::max(0.0f, (client_width - width) * 0.5f);
    viewport.TopLeftY = std::max(0.0f, (client_height - height) * 0.5f);
    viewport.Width = width;
    viewport.Height = height;
    return viewport;
}

CaptureDeviceInfo select_capture_device() {
    const auto devices = enumerate_video_capture_devices();
    std::wcout << L"Video capture devices found: " << devices.size() << L'\n';

    for (std::size_t i = 0; i < devices.size(); ++i) {
        std::wcout << L"[" << i << L"] " << devices[i].name << L'\n';
    }

    for (const auto& device : devices) {
        if (device.symbolic_link.find(kCaptureHardwareId) != std::wstring::npos) {
            std::wcout << L"Selected capture device: " << device.name
                       << L" (VID_345F:PID_2131)\n\n";
            return device;
        }
    }

    throw std::runtime_error("Capture card VID_345F:PID_2131 was not found");
}

ID3DBlob* compile_shader(const char* source, const char* entry_point, const char* target) {
    ID3DBlob* shader_blob = nullptr;
    ID3DBlob* error_blob = nullptr;

    const HRESULT result = D3DCompile(
        source,
        std::strlen(source),
        nullptr,
        nullptr,
        nullptr,
        entry_point,
        target,
        D3DCOMPILE_ENABLE_STRICTNESS,
        0,
        &shader_blob,
        &error_blob
    );

    if (FAILED(result)) {
        std::string message = "Failed to compile D3D shader";
        if (error_blob != nullptr) {
            message += ": ";
            message.append(
                static_cast<const char*>(error_blob->GetBufferPointer()),
                error_blob->GetBufferSize()
            );
            error_blob->Release();
        }
        if (shader_blob != nullptr) shader_blob->Release();
        throw std::runtime_error(message);
    }

    if (error_blob != nullptr) error_blob->Release();
    return shader_blob;
}

void create_render_target() {
    ID3D11Texture2D* back_buffer = nullptr;
    throw_if_failed(
        g_d3d.swap_chain->GetBuffer(0, IID_ID3D11Texture2D, reinterpret_cast<void**>(&back_buffer)),
        "Failed to get swap-chain back buffer"
    );

    const HRESULT result = g_d3d.device->CreateRenderTargetView(
        back_buffer,
        nullptr,
        &g_d3d.render_target
    );
    back_buffer->Release();
    throw_if_failed(result, "Failed to create render target view");
}

void destroy_render_target() {
    if (g_d3d.render_target != nullptr) {
        g_d3d.render_target->Release();
        g_d3d.render_target = nullptr;
    }
}

void resize_swap_chain(UINT width, UINT height) {
    if (g_d3d.swap_chain == nullptr || width == 0 || height == 0) return;

    destroy_render_target();
    throw_if_failed(
        g_d3d.swap_chain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0),
        "Failed to resize swap-chain buffers"
    );

    g_d3d.viewport_width = width;
    g_d3d.viewport_height = height;
    create_render_target();
}

void initialize_video_renderer(UINT capture_width, UINT capture_height) {
    ID3DBlob* vertex_blob = compile_shader(kVertexShaderSource, "main", "vs_5_0");
    const HRESULT vertex_result = g_d3d.device->CreateVertexShader(
        vertex_blob->GetBufferPointer(),
        vertex_blob->GetBufferSize(),
        nullptr,
        &g_d3d.video_vertex_shader
    );
    vertex_blob->Release();
    throw_if_failed(vertex_result, "Failed to create video vertex shader");

    ID3DBlob* pixel_blob = compile_shader(kPixelShaderSource, "main", "ps_5_0");
    const HRESULT pixel_result = g_d3d.device->CreatePixelShader(
        pixel_blob->GetBufferPointer(),
        pixel_blob->GetBufferSize(),
        nullptr,
        &g_d3d.video_pixel_shader
    );
    pixel_blob->Release();
    throw_if_failed(pixel_result, "Failed to create NV12 pixel shader");

    D3D11_BUFFER_DESC settings_desc{};
    settings_desc.ByteWidth = sizeof(ImageSettingsGpu);
    settings_desc.Usage = D3D11_USAGE_DYNAMIC;
    settings_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    settings_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    throw_if_failed(
        g_d3d.device->CreateBuffer(&settings_desc, nullptr, &g_d3d.image_settings_buffer),
        "Failed to create image settings constant buffer"
    );

    D3D11_TEXTURE2D_DESC texture_desc{};
    texture_desc.Width = capture_width;
    texture_desc.Height = capture_height + capture_height / 2;
    texture_desc.MipLevels = 1;
    texture_desc.ArraySize = 1;
    texture_desc.Format = DXGI_FORMAT_R8_UNORM;
    texture_desc.SampleDesc.Count = 1;
    texture_desc.Usage = D3D11_USAGE_DYNAMIC;
    texture_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    texture_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    throw_if_failed(
        g_d3d.device->CreateTexture2D(&texture_desc, nullptr, &g_d3d.nv12_texture),
        "Failed to create native NV12 upload texture"
    );

    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc{};
    srv_desc.Format = DXGI_FORMAT_R8_UNORM;
    srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Texture2D.MipLevels = 1;

    throw_if_failed(
        g_d3d.device->CreateShaderResourceView(g_d3d.nv12_texture, &srv_desc, &g_d3d.nv12_srv),
        "Failed to create NV12 shader resource view"
    );
}

void initialize_d3d(HWND window, const CaptureSession& capture) {
    DXGI_SWAP_CHAIN_DESC swap_chain_desc{};
    swap_chain_desc.BufferCount = 2;
    swap_chain_desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swap_chain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swap_chain_desc.OutputWindow = window;
    swap_chain_desc.SampleDesc.Count = 1;
    swap_chain_desc.Windowed = TRUE;
    swap_chain_desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL requested_levels[] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };
    D3D_FEATURE_LEVEL created_level{};

    throw_if_failed(
        D3D11CreateDeviceAndSwapChain(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            0,
            requested_levels,
            static_cast<UINT>(std::size(requested_levels)),
            D3D11_SDK_VERSION,
            &swap_chain_desc,
            &g_d3d.swap_chain,
            &g_d3d.device,
            &created_level,
            &g_d3d.context
        ),
        "Failed to initialize Direct3D 11"
    );

    RECT client_rect{};
    GetClientRect(window, &client_rect);
    g_d3d.viewport_width = static_cast<UINT>(client_rect.right - client_rect.left);
    g_d3d.viewport_height = static_cast<UINT>(client_rect.bottom - client_rect.top);

    g_capture_width = capture.capture_width();
    g_capture_height = capture.capture_height();

    create_render_target();
    initialize_video_renderer(g_capture_width, g_capture_height);
    g_hud_renderer.initialize(g_d3d.device);
    update_hud_text();

    std::cout << "Direct3D initialized. Feature level: 0x"
              << std::hex << static_cast<unsigned>(created_level) << std::dec << '\n';
}

bool upload_latest_frame(CaptureSession& capture) {
    static std::vector<std::uint8_t> frame;
    static std::uint64_t sequence = 0;

    if (!capture.copy_latest_frame(frame, sequence)) return false;

    const std::size_t expected_bytes = capture.frame_bytes();
    if (frame.size() < expected_bytes) return false;

    D3D11_MAPPED_SUBRESOURCE mapped{};
    throw_if_failed(
        g_d3d.context->Map(g_d3d.nv12_texture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped),
        "Failed to map NV12 upload texture"
    );

    const std::size_t row_bytes = capture.capture_width();
    const std::uint32_t rows = capture.capture_height() + capture.capture_height() / 2;

    const auto* source = frame.data();
    auto* destination = static_cast<std::uint8_t*>(mapped.pData);

    for (std::uint32_t row = 0; row < rows; ++row) {
        std::memcpy(
            destination + static_cast<std::size_t>(row) * mapped.RowPitch,
            source + static_cast<std::size_t>(row) * row_bytes,
            row_bytes
        );
    }

    g_d3d.context->Unmap(g_d3d.nv12_texture, 0);
    return true;
}

void update_image_settings_buffer() {
    ImageSettingsGpu settings{
        g_brightness,
        g_contrast,
        g_gamma,
        g_saturation,
        static_cast<std::uint32_t>(g_color_matrix),
        static_cast<std::uint32_t>(g_input_range),
        static_cast<std::uint32_t>(g_scaling_filter),
        static_cast<std::uint32_t>(g_chroma_filter)
    };

    D3D11_MAPPED_SUBRESOURCE mapped{};
    throw_if_failed(
        g_d3d.context->Map(g_d3d.image_settings_buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped),
        "Failed to map image settings constant buffer"
    );
    std::memcpy(mapped.pData, &settings, sizeof(settings));
    g_d3d.context->Unmap(g_d3d.image_settings_buffer, 0);
}

void render_frame(CaptureSession& capture) {
    if (g_d3d.context == nullptr || g_d3d.render_target == nullptr || g_d3d.swap_chain == nullptr) return;

    constexpr float clear_color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    static bool has_video_frame = false;

    if (upload_latest_frame(capture)) has_video_frame = true;

    g_d3d.context->OMSetRenderTargets(1, &g_d3d.render_target, nullptr);
    g_d3d.context->ClearRenderTargetView(g_d3d.render_target, clear_color);

    if (has_video_frame && g_d3d.viewport_width > 0 && g_d3d.viewport_height > 0) {
        update_image_settings_buffer();
        const D3D11_VIEWPORT viewport = calculate_video_viewport();
        g_d3d.context->RSSetViewports(1, &viewport);
        g_d3d.context->IASetInputLayout(nullptr);
        g_d3d.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g_d3d.context->VSSetShader(g_d3d.video_vertex_shader, nullptr, 0);
        g_d3d.context->PSSetShader(g_d3d.video_pixel_shader, nullptr, 0);
        g_d3d.context->PSSetConstantBuffers(0, 1, &g_d3d.image_settings_buffer);
        g_d3d.context->PSSetShaderResources(0, 1, &g_d3d.nv12_srv);
        g_d3d.context->Draw(3, 0);

        ID3D11ShaderResourceView* null_srv = nullptr;
        g_d3d.context->PSSetShaderResources(0, 1, &null_srv);
    }

    g_hud_renderer.render(g_d3d.context, g_d3d.viewport_width, g_d3d.viewport_height);
    g_d3d.swap_chain->Present(0, 0);
    update_hud_fps(capture);
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
    switch (message) {
    case WM_GETMINMAXINFO:
        if (!g_window_state.fullscreen) {
            const auto& resolution = selected_output_resolution();
            RECT minimum_rect{0, 0, static_cast<LONG>(resolution.width), static_cast<LONG>(resolution.height)};
            const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE));
            const DWORD ex_style = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_EXSTYLE));
            AdjustWindowRectEx(&minimum_rect, style, FALSE, ex_style);

            auto* minmax = reinterpret_cast<MINMAXINFO*>(l_param);
            minmax->ptMinTrackSize.x = minimum_rect.right - minimum_rect.left;
            minmax->ptMinTrackSize.y = minimum_rect.bottom - minimum_rect.top;
        }
        return 0;

    case WM_SIZE:
        if (w_param != SIZE_MINIMIZED && g_d3d.swap_chain != nullptr) {
            try {
                resize_swap_chain(LOWORD(l_param), HIWORD(l_param));
            } catch (const std::exception& error) {
                std::cerr << error.what() << '\n';
                PostQuitMessage(1);
            }
        }
        return 0;

    case WM_KEYDOWN:
        try {
            const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;

            if (w_param == VK_F11) {
                toggle_borderless_fullscreen(window);
                return 0;
            }
            if (w_param == VK_ESCAPE && g_window_state.fullscreen) {
                exit_borderless_fullscreen(window);
                return 0;
            }
            if (w_param == 'R') {
                cycle_output_resolution(window);
                return 0;
            }
            if (w_param == 'A') {
                toggle_aspect_ratio();
                return 0;
            }
            if (w_param == 'Q') {
                cycle_scaling_filter();
                return 0;
            }
            if (w_param == 'U') {
                toggle_chroma_filter();
                return 0;
            }
            if (w_param == 'C') {
                toggle_color_matrix();
                return 0;
            }
            if (w_param == 'L') {
                toggle_input_range();
                return 0;
            }
            if (w_param == 'B') {
                adjust_setting(g_brightness, shift ? -0.05f : 0.05f, -0.50f, 0.50f, "Brightness");
                return 0;
            }
            if (w_param == 'K') {
                adjust_setting(g_contrast, shift ? -0.10f : 0.10f, 0.50f, 2.00f, "Contrast");
                return 0;
            }
            if (w_param == 'G') {
                adjust_setting(g_gamma, shift ? -0.10f : 0.10f, 0.50f, 2.50f, "Gamma");
                return 0;
            }
            if (w_param == 'S') {
                adjust_setting(g_saturation, shift ? -0.10f : 0.10f, 0.00f, 2.00f, "Saturation");
                return 0;
            }
            if (w_param == '0') {
                reset_image_settings();
                return 0;
            }
            if (w_param == 'M') {
                cycle_display_mode();
                return 0;
            }
            if (w_param == '1') {
                g_display_mode = DisplayMode::FitAspectRatio;
                std::cout << "Display mode: Fit aspect ratio\n";
                return 0;
            }
            if (w_param == '2') {
                g_display_mode = DisplayMode::FixedResolution;
                std::cout << "Display mode: Fixed resolution\n";
                print_selected_resolution();
                update_hud_text();
                ensure_window_is_large_enough(window);
                return 0;
            }
            if (w_param == '3') {
                g_display_mode = DisplayMode::Stretch;
                std::cout << "Display mode: Stretch\n";
                return 0;
            }
        } catch (const std::exception& error) {
            std::cerr << "Window mode error: " << error.what() << '\n';
        }
        break;

    case WM_CLOSE:
        DestroyWindow(window);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }

    return DefWindowProcW(window, message, w_param, l_param);
}

HWND create_window(HINSTANCE instance) {
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(WNDCLASSEXW);
    window_class.style = CS_HREDRAW | CS_VREDRAW;
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.lpszClassName = kWindowClassName;

    if (RegisterClassExW(&window_class) == 0) {
        throw std::runtime_error("Failed to register Win32 window class");
    }

    RECT rectangle{0, 0, 1280, 720};
    AdjustWindowRect(&rectangle, WS_OVERLAPPEDWINDOW, FALSE);

    HWND window = CreateWindowExW(
        0,
        kWindowClassName,
        kWindowTitle,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        rectangle.right - rectangle.left,
        rectangle.bottom - rectangle.top,
        nullptr,
        nullptr,
        instance,
        nullptr
    );

    if (window == nullptr) throw std::runtime_error("Failed to create Win32 window");

    ShowWindow(window, SW_SHOWDEFAULT);
    UpdateWindow(window);
    return window;
}

} // namespace

int main() {
    try {
        MediaFoundationRuntime media_foundation;
        CaptureDeviceInfo device = select_capture_device();
        CaptureSession capture(std::move(device));
        capture.start();

        AudioPassthrough audio;
        audio.start();

        const HINSTANCE instance = GetModuleHandleW(nullptr);
        HWND window = create_window(instance);
        initialize_d3d(window, capture);

        std::cout << "NV12 renderer active.\n";
        std::cout << "Capture mode: " << capture.capture_width() << 'x' << capture.capture_height()
                  << " @ " << capture.capture_fps() << " fps, source=" << capture.source_format_name() << ".\n";
        print_image_settings();
        std::cout << "Scaling filter: " << scaling_filter_name()
                  << " (Q cycles Nearest/Bilinear/Sharp bilinear).\n";
        std::cout << "Chroma reconstruction: " << chroma_filter_name()
                  << " (U toggles legacy nearest / bilinear centered).\n";
        std::cout << "Default mode: Fixed resolution.\n";
        std::cout << "Aspect ratio: " << aspect_ratio_name() << " (A toggles 4:3/16:9).\n";
        print_selected_resolution();
        std::cout << "Hotkeys: F11 fullscreen, Esc leave fullscreen, R output resolution, A aspect ratio, Q scaling filter.\n";
        std::cout << "          U chroma reconstruction, C BT.601/BT.709, L Limited/Full range, 0 reset image settings.\n";
        std::cout << "          B brightness, K contrast, G gamma, S saturation; hold Shift to decrease.\n";
        std::cout << "          M cycle display modes, 1 Fit aspect ratio, 2 Fixed resolution, 3 Stretch.\n";
        std::cout << "Close the window to exit.\n";

        MSG message{};
        bool running = true;

        while (running) {
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (message.message == WM_QUIT) {
                    running = false;
                    break;
                }
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }

            if (running) render_frame(capture);
        }

        audio.stop();
        capture.stop();
        std::cout << "Captured frames: " << capture.frame_count() << '\n';
        return static_cast<int>(message.wParam);
    } catch (const std::exception& error) {
        std::cerr << "Fatal error: " << error.what() << '\n';
        MessageBoxA(nullptr, error.what(), "console-video-capture", MB_OK | MB_ICONERROR);
        return 1;
    }
}
