#pragma once

#include "capture_devices.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

class CaptureSession {
public:
    // Renderer-side frame size. Xbox capture profiles can use either a native
    // 720p60 source or a native 1080p source; Media Foundation normalizes the
    // frames to 1080p YUY2 before they reach the renderer.
    static constexpr std::uint32_t kCaptureWidth = 1920;
    static constexpr std::uint32_t kCaptureHeight = 1080;
    static constexpr std::uint32_t kBytesPerPixel = 2;
    static constexpr std::size_t kFrameBytes =
        static_cast<std::size_t>(kCaptureWidth) *
        static_cast<std::size_t>(kCaptureHeight) *
        kBytesPerPixel;

    explicit CaptureSession(CaptureDeviceInfo device);
    ~CaptureSession();

    CaptureSession(const CaptureSession&) = delete;
    CaptureSession& operator=(const CaptureSession&) = delete;

    void start();
    void stop();

    [[nodiscard]] std::uint64_t frame_count() const noexcept;
    [[nodiscard]] bool running() const noexcept;

    [[nodiscard]] bool copy_latest_frame(
        std::vector<std::uint8_t>& destination,
        std::uint64_t& sequence
    ) const;

private:
    struct CaptureMode {
        std::uint32_t width;
        std::uint32_t height;
        std::uint32_t fps;
    };

    static CaptureMode choose_capture_mode();
    void capture_loop(std::stop_token stop_token);

    CaptureDeviceInfo device_;
    CaptureMode mode_;
    std::jthread thread_;
    std::atomic<std::uint64_t> frame_count_{0};
    std::atomic<bool> running_{false};

    mutable std::mutex frame_mutex_;
    std::vector<std::uint8_t> latest_frame_;
    std::uint64_t latest_frame_sequence_ = 0;
};
