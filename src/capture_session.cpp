#include "capture_session.h"

#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <objbase.h>

#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

constexpr DWORD kVideoStream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
constexpr std::size_t kRowBytes =
    static_cast<std::size_t>(CaptureSession::kCaptureWidth) *
    CaptureSession::kBytesPerPixel;

void throw_if_failed(HRESULT result, const char* message) {
    if (FAILED(result)) {
        throw std::runtime_error(
            std::string(message) + " (HRESULT=" + std::to_string(static_cast<long>(result)) + ")"
        );
    }
}

IMFMediaSource* create_media_source(const CaptureDeviceInfo& device) {
    IMFAttributes* attributes = nullptr;
    IMFMediaSource* media_source = nullptr;

    throw_if_failed(
        MFCreateAttributes(&attributes, 2),
        "Failed to create capture-session source attributes"
    );

    HRESULT result = attributes->SetGUID(
        MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
        MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID
    );

    if (SUCCEEDED(result)) {
        result = attributes->SetString(
            MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
            device.symbolic_link.c_str()
        );
    }

    if (SUCCEEDED(result)) {
        result = MFCreateDeviceSource(attributes, &media_source);
    }

    attributes->Release();
    throw_if_failed(result, "Failed to open capture device");
    return media_source;
}

IMFMediaType* find_yuy2_capture_type(IMFSourceReader* reader, UINT32& selected_fps) {
    IMFMediaType* fifty_hz_fallback = nullptr;

    for (DWORD index = 0;; ++index) {
        IMFMediaType* media_type = nullptr;
        const HRESULT result = reader->GetNativeMediaType(
            kVideoStream,
            index,
            &media_type
        );

        if (result == MF_E_NO_MORE_TYPES) {
            break;
        }
        throw_if_failed(result, "Failed to inspect capture media type");

        GUID subtype{};
        UINT32 width = 0;
        UINT32 height = 0;
        UINT32 fps_num = 0;
        UINT32 fps_den = 1;

        const bool dimensions_match =
            SUCCEEDED(media_type->GetGUID(MF_MT_SUBTYPE, &subtype)) &&
            subtype == MFVideoFormat_YUY2 &&
            SUCCEEDED(MFGetAttributeSize(media_type, MF_MT_FRAME_SIZE, &width, &height)) &&
            width == CaptureSession::kCaptureWidth &&
            height == CaptureSession::kCaptureHeight &&
            SUCCEEDED(MFGetAttributeRatio(media_type, MF_MT_FRAME_RATE, &fps_num, &fps_den)) &&
            fps_den != 0;

        if (dimensions_match) {
            if (fps_num == 60 * fps_den) {
                if (fifty_hz_fallback != nullptr) {
                    fifty_hz_fallback->Release();
                }
                selected_fps = 60;
                return media_type;
            }

            if (fps_num == 50 * fps_den && fifty_hz_fallback == nullptr) {
                fifty_hz_fallback = media_type;
                continue;
            }
        }

        media_type->Release();
    }

    if (fifty_hz_fallback != nullptr) {
        selected_fps = 50;
        return fifty_hz_fallback;
    }

    return nullptr;
}

bool copy_sample_to_frame(IMFSample* sample, std::vector<std::uint8_t>& destination) {
    IMFMediaBuffer* buffer = nullptr;
    const HRESULT buffer_result = sample->ConvertToContiguousBuffer(&buffer);
    if (FAILED(buffer_result) || buffer == nullptr) {
        return false;
    }

    bool copied = false;

    IMF2DBuffer* buffer_2d = nullptr;
    if (SUCCEEDED(buffer->QueryInterface(IID_PPV_ARGS(&buffer_2d))) && buffer_2d != nullptr) {
        BYTE* scanline = nullptr;
        LONG pitch = 0;
        if (SUCCEEDED(buffer_2d->Lock2D(&scanline, &pitch)) && scanline != nullptr) {
            const std::size_t absolute_pitch =
                static_cast<std::size_t>(pitch < 0 ? -static_cast<long long>(pitch) : pitch);

            if (absolute_pitch >= kRowBytes) {
                for (std::uint32_t row = 0; row < CaptureSession::kCaptureHeight; ++row) {
                    const BYTE* source_row = pitch >= 0
                        ? scanline + static_cast<std::size_t>(row) * absolute_pitch
                        : scanline - static_cast<std::size_t>(row) * absolute_pitch;

                    std::memcpy(
                        destination.data() + static_cast<std::size_t>(row) * kRowBytes,
                        source_row,
                        kRowBytes
                    );
                }
                copied = true;
            }

            buffer_2d->Unlock2D();
        }
        buffer_2d->Release();
    }

    if (!copied) {
        BYTE* data = nullptr;
        DWORD max_length = 0;
        DWORD current_length = 0;

        if (SUCCEEDED(buffer->Lock(&data, &max_length, &current_length)) && data != nullptr) {
            if (current_length >= CaptureSession::kFrameBytes) {
                std::memcpy(destination.data(), data, CaptureSession::kFrameBytes);
                copied = true;
            } else {
                static bool warned_short_buffer = false;
                if (!warned_short_buffer) {
                    std::cerr << "Capture sample buffer is smaller than expected: "
                              << current_length << " bytes, expected at least "
                              << CaptureSession::kFrameBytes << " bytes.\n";
                    warned_short_buffer = true;
                }
            }
            buffer->Unlock();
        }
    }

    buffer->Release();
    return copied;
}

} // namespace

CaptureSession::CaptureSession(CaptureDeviceInfo device)
    : device_(std::move(device)) {}

CaptureSession::~CaptureSession() {
    stop();
}

void CaptureSession::start() {
    if (thread_.joinable()) {
        return;
    }

    {
        std::scoped_lock lock(frame_mutex_);
        latest_frame_.resize(kFrameBytes);
        latest_frame_sequence_ = 0;
    }

    frame_count_.store(0, std::memory_order_relaxed);
    thread_ = std::jthread([this](std::stop_token stop_token) {
        capture_loop(stop_token);
    });
}

void CaptureSession::stop() {
    if (!thread_.joinable()) {
        return;
    }

    thread_.request_stop();
    thread_.join();
}

std::uint64_t CaptureSession::frame_count() const noexcept {
    return frame_count_.load(std::memory_order_relaxed);
}

bool CaptureSession::running() const noexcept {
    return running_.load(std::memory_order_acquire);
}

bool CaptureSession::copy_latest_frame(
    std::vector<std::uint8_t>& destination,
    std::uint64_t& sequence
) const {
    std::scoped_lock lock(frame_mutex_);

    if (latest_frame_sequence_ == 0 || latest_frame_sequence_ == sequence) {
        return false;
    }

    destination.resize(kFrameBytes);
    std::memcpy(destination.data(), latest_frame_.data(), kFrameBytes);
    sequence = latest_frame_sequence_;
    return true;
}

void CaptureSession::capture_loop(std::stop_token stop_token) {
    const HRESULT com_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com_result)) {
        std::cerr << "Capture thread COM initialization failed: "
                  << static_cast<long>(com_result) << '\n';
        return;
    }

    IMFMediaSource* media_source = nullptr;
    IMFSourceReader* reader = nullptr;

    try {
        media_source = create_media_source(device_);

        IMFAttributes* reader_attributes = nullptr;
        throw_if_failed(
            MFCreateAttributes(&reader_attributes, 1),
            "Failed to create source-reader attributes"
        );

        reader_attributes->SetUINT32(MF_LOW_LATENCY, TRUE);

        const HRESULT reader_result = MFCreateSourceReaderFromMediaSource(
            media_source,
            reader_attributes,
            &reader
        );
        reader_attributes->Release();
        throw_if_failed(reader_result, "Failed to create capture source reader");

        UINT32 selected_fps = 0;
        IMFMediaType* selected_type = find_yuy2_capture_type(reader, selected_fps);
        if (selected_type == nullptr) {
            throw std::runtime_error("Capture device does not expose 720x480 YUY2 at 60 or 50 fps");
        }

        const HRESULT set_type_result = reader->SetCurrentMediaType(
            kVideoStream,
            nullptr,
            selected_type
        );
        selected_type->Release();
        throw_if_failed(set_type_result, "Failed to select 720x480 YUY2 capture mode");

        std::wcout << L"Capture started on " << device_.name
                   << L" using 720x480 @ " << selected_fps << L" fps YUY2.\n";

        running_.store(true, std::memory_order_release);

        auto last_report = std::chrono::steady_clock::now();
        std::uint64_t frames_at_last_report = 0;
        bool reported_first_frame = false;

        while (!stop_token.stop_requested()) {
            DWORD stream_index = 0;
            DWORD flags = 0;
            LONGLONG timestamp = 0;
            IMFSample* sample = nullptr;

            const HRESULT read_result = reader->ReadSample(
                kVideoStream,
                0,
                &stream_index,
                &flags,
                &timestamp,
                &sample
            );

            if (FAILED(read_result)) {
                throw_if_failed(read_result, "ReadSample failed");
            }

            if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
                if (sample != nullptr) {
                    sample->Release();
                }
                break;
            }

            if (sample != nullptr) {
                std::vector<std::uint8_t> captured_frame(kFrameBytes);
                if (copy_sample_to_frame(sample, captured_frame)) {
                    {
                        std::scoped_lock lock(frame_mutex_);
                        latest_frame_.swap(captured_frame);
                        ++latest_frame_sequence_;
                    }

                    if (!reported_first_frame) {
                        std::cout << "First YUY2 frame published to renderer ("
                                  << kFrameBytes << " bytes).\n";
                        reported_first_frame = true;
                    }
                }

                frame_count_.fetch_add(1, std::memory_order_relaxed);
                sample->Release();
            }

            const auto now = std::chrono::steady_clock::now();
            if (now - last_report >= std::chrono::seconds(1)) {
                const std::uint64_t total_frames = frame_count();
                const std::uint64_t delta = total_frames - frames_at_last_report;
                std::cout << "Capture FPS: " << delta
                          << " | total frames: " << total_frames << '\n';
                frames_at_last_report = total_frames;
                last_report = now;
            }
        }
    } catch (const std::exception& error) {
        std::cerr << "Capture session error: " << error.what() << '\n';
    }

    running_.store(false, std::memory_order_release);

    if (reader != nullptr) {
        reader->Release();
    }
    if (media_source != nullptr) {
        media_source->Shutdown();
        media_source->Release();
    }

    CoUninitialize();
}
