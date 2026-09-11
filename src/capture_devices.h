#pragma once

#include <string>
#include <vector>

struct CaptureDeviceInfo {
    std::wstring name;
    std::wstring symbolic_link;
};

std::vector<CaptureDeviceInfo> enumerate_video_capture_devices();
