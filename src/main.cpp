#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <mfapi.h>
#include <objbase.h>

#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>

#include "capture_devices.h"
#include "capture_formats.h"

namespace {

constexpr wchar_t kWindowClassName[] = L"PS2CaptureStreamWindowClass";
constexpr wchar_t kWindowTitle[] = L"PS2 Capture Stream - Stage 3";
constexpr wchar_t kPreferredCaptureDevice[] = L"USB3 Video";

struct D3DState {
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    IDXGISwapChain* swap_chain = nullptr;
    ID3D11RenderTargetView* render_target = nullptr;

    ~D3DState() {
        if (context != nullptr) {
            context->ClearState();
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

std::vector<CaptureDeviceInfo> print_capture_devices() {
    const auto devices = enumerate_video_capture_devices();

    std::wcout << L"Video capture devices found: " << devices.size() << L'\n';
    if (devices.empty()) {
        std::wcout << L"  No video capture devices detected.\n";
        return devices;
    }

    for (std::size_t i = 0; i < devices.size(); ++i) {
        const auto& device = devices[i];
        std::wcout << L"[" << i << L"] "
                   << (device.name.empty() ? L"(unnamed device)" : device.name)
                   << L'\n';

        if (!device.symbolic_link.empty()) {
            std::wcout << L"    " << device.symbolic_link << L'\n';
        }
    }

    return devices;
}

void print_capture_formats(const std::vector<CaptureDeviceInfo>& devices) {
    const CaptureDeviceInfo* selected = nullptr;

    for (const auto& device : devices) {
        if (device.name == kPreferredCaptureDevice) {
            selected = &device;
            break;
        }
    }

    if (selected == nullptr) {
        std::wcout << L"\nPreferred capture device '" << kPreferredCaptureDevice
                   << L"' was not found; skipping format enumeration.\n";
        return;
    }

    std::wcout << L"\nNative formats for " << selected->name << L":\n";

    const auto formats = enumerate_video_formats(*selected);
    if (formats.empty()) {
        std::wcout << L"  No native video formats reported.\n";
        return;
    }

    for (std::size_t i = 0; i < formats.size(); ++i) {
        const auto& format = formats[i];
        const double fps = format.fps_denominator == 0
            ? 0.0
            : static_cast<double>(format.fps_numerator) /
                static_cast<double>(format.fps_denominator);

        std::wcout << L"[" << std::setw(2) << i << L"] "
                   << format.width << L"x" << format.height
                   << L" @ " << std::fixed << std::setprecision(3) << fps
                   << L" fps - " << format.subtype << L'\n';
    }
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

    create_render_target();
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
    create_render_target();

    std::cout << "Direct3D initialized. Feature level: 0x"
              << std::hex << static_cast<unsigned>(created_level) << std::dec << '\n';
}

void render_frame() {
    if (g_d3d.context == nullptr || g_d3d.render_target == nullptr || g_d3d.swap_chain == nullptr) {
        return;
    }

    constexpr float clear_color[4] = {
        0.035f,
        0.035f,
        0.045f,
        1.0f,
    };

    g_d3d.context->OMSetRenderTargets(1, &g_d3d.render_target, nullptr);
    g_d3d.context->ClearRenderTargetView(g_d3d.render_target, clear_color);
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
        const auto devices = print_capture_devices();
        print_capture_formats(devices);

        const HINSTANCE instance = GetModuleHandleW(nullptr);
        HWND window = create_window(instance);
        initialize_d3d(window);

        std::cout << "Stage 3 running: native capture format discovery + D3D11 renderer.\n";
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
                render_frame();
            }
        }

        return static_cast<int>(message.wParam);
    } catch (const std::exception& error) {
        std::cerr << "Fatal error: " << error.what() << '\n';
        MessageBoxA(nullptr, error.what(), "ps2-capture-stream", MB_OK | MB_ICONERROR);
        return 1;
    }
}
