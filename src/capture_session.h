#pragma once

#include "capture_devices.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

class CaptureSession {
public:
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

    // Copies only when a newer frame than sequence is available.
    // sequence is updated to the copied frame's sequence number.
    [[nodiscard]] bool copy_latest_frame(
        std::vector<std::uint8_t>& destination,
        std::uint64_t& sequence
    ) const;

private:
    void capture_loop(std::stop_token stop_token);

    CaptureDeviceInfo device_;
    std::jthread thread_;
    std::atomic<std::uint64_t> frame_count_{0};
    std::atomic<bool> running_{false};

    mutable std::mutex frame_mutex_;
    std::vector<std::uint8_t> latest_frame_;
    std::uint64_t latest_frame_sequence_ = 0;
};
