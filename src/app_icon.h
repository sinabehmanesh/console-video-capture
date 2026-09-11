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

inline void apply_to_window_class(WNDCLASSEXW& window_class, HINSTANCE instance) {
    window_class.hIcon = load(instance, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON));
    window_class.hIconSm = load(instance, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON));
}

inline void apply_to_window(HWND window, HINSTANCE instance) {
    HICON large_icon = load(instance, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON));
    HICON small_icon = load(instance, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON));

    if (large_icon != nullptr) {
        SendMessageW(window, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(large_icon));
    }
    if (small_icon != nullptr) {
        SendMessageW(window, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(small_icon));
    }
}

} // namespace app_icon
