#pragma once

#include <windows.h>

namespace app_icon {

constexpr int kResourceId = 101;

inline HICON load(HINSTANCE instance, int width, int height) {
    return static_cast<HICON>(LoadImageW(
        instance,
        MAKEINTRESOURCEW(kResourceId),
        IMAGE_ICON,
        width,
        height,
        LR_DEFAULTCOLOR
    ));
}

inline void apply_to_window(HWND window, HINSTANCE instance) {
    const HICON large_icon = load(
        instance,
        GetSystemMetrics(SM_CXICON),
        GetSystemMetrics(SM_CYICON)
    );
    const HICON small_icon = load(
        instance,
        GetSystemMetrics(SM_CXSMICON),
        GetSystemMetrics(SM_CYSMICON)
    );

    if (large_icon != nullptr) {
        SendMessageW(window, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(large_icon));
        SetClassLongPtrW(window, GCLP_HICON, reinterpret_cast<LONG_PTR>(large_icon));
    }
    if (small_icon != nullptr) {
        SendMessageW(window, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(small_icon));
        SetClassLongPtrW(window, GCLP_HICONSM, reinterpret_cast<LONG_PTR>(small_icon));
    }
}

inline ATOM register_class_ex(const WNDCLASSEXW* source) {
    if (source == nullptr) return 0;

    WNDCLASSEXW window_class = *source;
    const HINSTANCE instance = window_class.hInstance;

    if (window_class.hIcon == nullptr) {
        window_class.hIcon = load(
            instance,
            GetSystemMetrics(SM_CXICON),
            GetSystemMetrics(SM_CYICON)
        );
    }
    if (window_class.hIconSm == nullptr) {
        window_class.hIconSm = load(
            instance,
            GetSystemMetrics(SM_CXSMICON),
            GetSystemMetrics(SM_CYSMICON)
        );
    }

    return ::RegisterClassExW(&window_class);
}

inline HWND create_window_ex(
    DWORD ex_style,
    LPCWSTR class_name,
    LPCWSTR window_name,
    DWORD style,
    int x,
    int y,
    int width,
    int height,
    HWND parent,
    HMENU menu,
    HINSTANCE instance,
    LPVOID param
) {
    HWND window = ::CreateWindowExW(
        ex_style,
        class_name,
        window_name,
        style,
        x,
        y,
        width,
        height,
        parent,
        menu,
        instance,
        param
    );

    if (window != nullptr) {
        apply_to_window(window, instance);
    }

    return window;
}

} // namespace app_icon

// main.cpp already calls the normal Win32 APIs. Force-including this header lets us
// transparently attach the embedded resource icon without duplicating window code.
#define RegisterClassExW app_icon::register_class_ex
#define CreateWindowExW app_icon::create_window_ex
