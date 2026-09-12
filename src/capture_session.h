#pragma once

#include "capture_devices.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

class CaptureSession {
public:
    explicit CaptureSession(CaptureDeviceInfo device);
    ~CaptureSession();

    CaptureSession(const CaptureSession&) = delete;
    CaptureSession& operator=(const CaptureSession&) = delete;

    void start();
    void stop();

    [[nodiscard]] std::uint64_t frame_count() const noexcept;
    [[nodiscard]] bool running() const noexcept;

    [[nodiscard]] std::uint32_t capture_width() const noexcept;
    [[nodiscard]] std::uint32_t capture_height() const noexcept;
    [[nodiscard]] std::uint32_t capture_fps() const noexcept;
    [[nodiscard]] std::size_t frame_bytes() const noexcept;

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
