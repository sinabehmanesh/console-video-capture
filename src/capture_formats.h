#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "capture_devices.h"

struct CaptureFormatInfo {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t fps_numerator = 0;
    std::uint32_t fps_denominator = 1;
    std::wstring subtype;
};

std::vector<CaptureFormatInfo> enumerate_video_formats(const CaptureDeviceInfo& device);
