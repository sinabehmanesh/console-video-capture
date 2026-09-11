#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <mfapi.h>
#include <objbase.h>

#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "capture_devices.h"
#include "capture_session.h"

namespace {

constexpr wchar_t kWindowClassName[] = L"PS2CaptureStreamWindowClass";
constexpr wchar_t kWindowTitle[] = L"PS2 Capture Stream - Stage 5";
constexpr wchar_t kCaptureHardwareId[] = L"vid_345f&pid_2131";

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
Texture2D<float4> yuy2_texture : register(t0);

struct PSIn {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

float4 main(PSIn input) : SV_TARGET {
    const uint source_width = 1920;
    const uint source_height = 1080;

    uint source_x = min((uint)(saturate(input.uv.x) * source_width), source_width - 1);
    uint source_y = min((uint)(saturate(input.uv.y) * source_height), source_height - 1);

    float4 packed = yuy2_texture.Load(int3(source_x / 2, source_y, 0));

    float y = ((source_x & 1) == 0) ? packed.r : packed.b;
    float u = packed.g - (128.0 / 255.0);
    float v = packed.a - (128.0 / 255.0);

    // BT.709 limited-range YUV -> RGB, appropriate for the 1080p capture path.
    float luma = 1.164383 * (y - (16.0 / 255.0));
    float3 rgb;
    rgb.r = luma + 1.792741 * v;
    rgb.g = luma - 0.213249 * u - 0.532909 * v;
    rgb.b = luma + 2.112402 * u;

    return float4(saturate(rgb), 1.0);
}
)";

struct D3DState {
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    IDXGISwapChain* swap_chain = nullptr;
    ID3D11RenderTargetView* render_target = nullptr;
    ID3D11VertexShader* video_vertex_shader = nullptr;
    ID3D11PixelShader* video_pixel_shader = nullptr;
    ID3D11Texture2D* yuy2_texture = nullptr;
    ID3D11ShaderResourceView* yuy2_srv = nullptr;
    UINT viewport_width = 0;
    UINT viewport_height = 0;

    ~D3DState() {
        if (context != nullptr) {
            context->ClearState();
        }
        if (yuy2_srv != nullptr) {
            yuy2_srv->Release();
        }
        if (yuy2_texture != nullptr) {
            yuy2_texture->Release();
        }
        if (video_pixel_shader != nullptr) {
            video_pixel_shader->Release();
        }
        if (video_vertex_shader != nullptr) {
            video_vertex_shader->Release();
        }
        if (render_target != nullptr) {
            render_target->Release();
        }
        if (swap_chain != nullptr) {
            swap_chain->Release();
        }
        if (context != nullptr) {
            context->Release();
        }
        if (device != nullptr) {
            device->Release();
        }
    }
};

class MediaFoundationRuntime {
public:
    MediaFoundationRuntime() {
        const HRESULT com_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(com_result)) {
            throw std::runtime_error(
                "Failed to initialize COM (HRESULT=" +
                std::to_string(static_cast<long>(com_result)) + ")"
            );
        }
        com_initialized_ = true;

        const HRESULT mf_result = MFStartup(MF_VERSION, MFSTARTUP_FULL);
        if (FAILED(mf_result)) {
            CoUninitialize();
            com_initialized_ = false;
            throw std::runtime_error(
                "Failed to initialize Media Foundation (HRESULT=" +
                std::to_string(static_cast<long>(mf_result)) + ")"
            );
        }
        mf_initialized_ = true;
    }

    ~MediaFoundationRuntime() {
        if (mf_initialized_) {
            MFShutdown();
        }
        if (com_initialized_) {
            CoUninitialize();
        }
    }

    MediaFoundationRuntime(const MediaFoundationRuntime&) = delete;
    MediaFoundationRuntime& operator=(const MediaFoundationRuntime&) = delete;

private:
    bool com_initialized_ = false;
    bool mf_initialized_ = false;
};

D3DState g_d3d;

void throw_if_failed(HRESULT result, const char* message) {
    if (FAILED(result)) {
        throw std::runtime_error(
            std::string(message) + " (HRESULT=" + std::to_string(static_cast<long>(result)) + ")"
        );
    }
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
        if (shader_blob != nullptr) {
            shader_blob->Release();
        }
        throw std::runtime_error(message);
    }

    if (error_blob != nullptr) {
        error_blob->Release();
    }

    return shader_blob;
}

void create_render_target() {
    ID3D11Texture2D* back_buffer = nullptr;
    const HRESULT result = g_d3d.swap_chain->GetBuffer(
        0,
        IID_ID3D11Texture2D,
        reinterpret_cast<void**>(&back_buffer)
    );
    throw_if_failed(result, "Failed to get swap-chain back buffer");

    const HRESULT view_result = g_d3d.device->CreateRenderTargetView(
        back_buffer,
        nullptr,
        &g_d3d.render_target
    );

    back_buffer->Release();
    throw_if_failed(view_result, "Failed to create render target view");
}

void destroy_render_target() {
    if (g_d3d.render_target != nullptr) {
        g_d3d.render_target->Release();
        g_d3d.render_target = nullptr;
    }
}

void resize_swap_chain(UINT width, UINT height) {
    if (g_d3d.swap_chain == nullptr || width == 0 || height == 0) {
        return;
    }

    destroy_render_target();

    const HRESULT result = g_d3d.swap_chain->ResizeBuffers(
        0,
        width,
        height,
        DXGI_FORMAT_UNKNOWN,
        0
    );
    throw_if_failed(result, "Failed to resize swap-chain buffers");

    g_d3d.viewport_width = width;
    g_d3d.viewport_height = height;
    create_render_target();
}

void initialize_video_renderer() {
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
    throw_if_failed(pixel_result, "Failed to create YUY2 pixel shader");

    D3D11_TEXTURE2D_DESC texture_desc{};
    texture_desc.Width = CaptureSession::kCaptureWidth / 2;
    texture_desc.Height = CaptureSession::kCaptureHeight;
    texture_desc.MipLevels = 1;
    texture_desc.ArraySize = 1;
    texture_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texture_desc.SampleDesc.Count = 1;
    texture_desc.Usage = D3D11_USAGE_DYNAMIC;
    texture_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    texture_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    throw_if_failed(
        g_d3d.device->CreateTexture2D(&texture_desc, nullptr, &g_d3d.yuy2_texture),
        "Failed to create YUY2 upload texture"
    );

    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc{};
    srv_desc.Format = texture_desc.Format;
    srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Texture2D.MipLevels = 1;

    throw_if_failed(
        g_d3d.device->CreateShaderResourceView(
            g_d3d.yuy2_texture,
            &srv_desc,
            &g_d3d.yuy2_srv
        ),
        "Failed to create YUY2 shader resource view"
    );
}

void initialize_d3d(HWND window) {
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

    const HRESULT result = D3D11CreateDeviceAndSwapChain(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        0,
        requested_levels,
        static_cast<UINT>(sizeof(requested_levels) / sizeof(requested_levels[0])),
        D3D11_SDK_VERSION,
        &swap_chain_desc,
        &g_d3d.swap_chain,
        &g_d3d.device,
        &created_level,
        &g_d3d.context
    );

    throw_if_failed(result, "Failed to initialize Direct3D 11");

    RECT client_rect{};
    GetClientRect(window, &client_rect);
    g_d3d.viewport_width = static_cast<UINT>(client_rect.right - client_rect.left);
    g_d3d.viewport_height = static_cast<UINT>(client_rect.bottom - client_rect.top);

    create_render_target();
    initialize_video_renderer();

    std::cout << "Direct3D initialized. Feature level: 0x"
              << std::hex << static_cast<unsigned>(created_level) << std::dec << '\n';
}

bool upload_latest_frame(CaptureSession& capture) {
    static std::vector<std::uint8_t> frame;
    static std::uint64_t sequence = 0;

    if (!capture.copy_latest_frame(frame, sequence)) {
        return false;
    }

    D3D11_MAPPED_SUBRESOURCE mapped{};
    const HRESULT map_result = g_d3d.context->Map(
        g_d3d.yuy2_texture,
        0,
        D3D11_MAP_WRITE_DISCARD,
        0,
        &mapped
    );
    throw_if_failed(map_result, "Failed to map YUY2 upload texture");

    constexpr std::size_t source_row_bytes =
        static_cast<std::size_t>(CaptureSession::kCaptureWidth) *
        CaptureSession::kBytesPerPixel;

    const auto* source = frame.data();
    auto* destination = static_cast<std::uint8_t*>(mapped.pData);

    for (std::uint32_t row = 0; row < CaptureSession::kCaptureHeight; ++row) {
        std::memcpy(
            destination + static_cast<std::size_t>(row) * mapped.RowPitch,
            source + static_cast<std::size_t>(row) * source_row_bytes,
            source_row_bytes
        );
    }

    g_d3d.context->Unmap(g_d3d.yuy2_texture, 0);
    return true;
}

void render_frame(CaptureSession& capture) {
    if (g_d3d.context == nullptr || g_d3d.render_target == nullptr || g_d3d.swap_chain == nullptr) {
        return;
    }

    constexpr float clear_color[4] = {
        0.0f,
        0.0f,
        0.0f,
        1.0f,
    };

    static bool has_video_frame = false;
    if (upload_latest_frame(capture)) {
        has_video_frame = true;
    }

    g_d3d.context->OMSetRenderTargets(1, &g_d3d.render_target, nullptr);
    g_d3d.context->ClearRenderTargetView(g_d3d.render_target, clear_color);

    if (has_video_frame && g_d3d.viewport_width > 0 && g_d3d.viewport_height > 0) {
        D3D11_VIEWPORT viewport{};
        viewport.Width = static_cast<float>(g_d3d.viewport_width);
        viewport.Height = static_cast<float>(g_d3d.viewport_height);
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;
        g_d3d.context->RSSetViewports(1, &viewport);

        g_d3d.context->IASetInputLayout(nullptr);
        g_d3d.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g_d3d.context->VSSetShader(g_d3d.video_vertex_shader, nullptr, 0);
        g_d3d.context->PSSetShader(g_d3d.video_pixel_shader, nullptr, 0);
        g_d3d.context->PSSetShaderResources(0, 1, &g_d3d.yuy2_srv);
        g_d3d.context->Draw(3, 0);

        ID3D11ShaderResourceView* null_srv = nullptr;
        g_d3d.context->PSSetShaderResources(0, 1, &null_srv);
    }

    g_d3d.swap_chain->Present(0, 0);
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
    switch (message) {
    case WM_SIZE:
        if (w_param != SIZE_MINIMIZED && g_d3d.swap_chain != nullptr) {
            const UINT width = LOWORD(l_param);
            const UINT height = HIWORD(l_param);
            try {
                resize_swap_chain(width, height);
            } catch (const std::exception& error) {
                std::cerr << error.what() << '\n';
                PostQuitMessage(1);
            }
        }
        return 0;

    case WM_CLOSE:
        DestroyWindow(window);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    default:
        return DefWindowProcW(window, message, w_param, l_param);
    }
}

HWND create_window(HINSTANCE instance) {
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.style = CS_HREDRAW | CS_VREDRAW;
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.lpszClassName = kWindowClassName;

    if (RegisterClassExW(&window_class) == 0) {
        throw std::runtime_error("Failed to register Win32 window class");
    }

    constexpr int initial_width = 960;
    constexpr int initial_height = 720;

    RECT rectangle{0, 0, initial_width, initial_height};
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

    if (window == nullptr) {
        throw std::runtime_error("Failed to create Win32 window");
    }

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

        const HINSTANCE instance = GetModuleHandleW(nullptr);
        HWND window = create_window(instance);
        initialize_d3d(window);

        std::cout << "Stage 5 running: live 1080p60 YUY2 capture + GPU YUV-to-RGB rendering.\n";
        std::cout << "The capture image should now be visible in the D3D11 window.\n";
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

            if (running) {
                render_frame(capture);
            }
        }

        capture.stop();
        std::cout << "Captured frames: " << capture.frame_count() << '\n';
        return static_cast<int>(message.wParam);
    } catch (const std::exception& error) {
        std::cerr << "Fatal error: " << error.what() << '\n';
        MessageBoxA(nullptr, error.what(), "ps2-capture-stream", MB_OK | MB_ICONERROR);
        return 1;
    }
}