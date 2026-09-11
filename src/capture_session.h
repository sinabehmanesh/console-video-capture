#pragma once

#include "capture_devices.h"

#include <atomic>
#include <cstdint>
#include <thread>

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

private:
    void capture_loop(std::stop_token stop_token);

    CaptureDeviceInfo device_;
    std::jthread thread_;
    std::atomic<std::uint64_t> frame_count_{0};
    std::atomic<bool> running_{false};
};
