#include "capture_session.h"

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <objbase.h>

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

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

IMFMediaType* find_yuy2_1080p60(IMFSourceReader* reader) {
    for (DWORD index = 0;; ++index) {
        IMFMediaType* media_type = nullptr;
        const HRESULT result = reader->GetNativeMediaType(
            MF_SOURCE_READER_FIRST_VIDEO_STREAM,
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

        const bool matches =
            SUCCEEDED(media_type->GetGUID(MF_MT_SUBTYPE, &subtype)) &&
            subtype == MFVideoFormat_YUY2 &&
            SUCCEEDED(MFGetAttributeSize(media_type, MF_MT_FRAME_SIZE, &width, &height)) &&
            width == 1920 &&
            height == 1080 &&
            SUCCEEDED(MFGetAttributeRatio(media_type, MF_MT_FRAME_RATE, &fps_num, &fps_den)) &&
            fps_den != 0 &&
            fps_num == 60 * fps_den;

        if (matches) {
            return media_type;
        }

        media_type->Release();
    }

    return nullptr;
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

        // Ask Media Foundation to avoid unnecessary buffering where supported.
        reader_attributes->SetUINT32(MF_LOW_LATENCY, TRUE);

        const HRESULT reader_result = MFCreateSourceReaderFromMediaSource(
            media_source,
            reader_attributes,
            &reader
        );
        reader_attributes->Release();
        throw_if_failed(reader_result, "Failed to create capture source reader");

        IMFMediaType* selected_type = find_yuy2_1080p60(reader);
        if (selected_type == nullptr) {
            throw std::runtime_error("Capture device does not expose 1920x1080 @ 60 YUY2");
        }

        const HRESULT set_type_result = reader->SetCurrentMediaType(
            MF_SOURCE_READER_FIRST_VIDEO_STREAM,
            nullptr,
            selected_type
        );
        selected_type->Release();
        throw_if_failed(set_type_result, "Failed to select 1920x1080 @ 60 YUY2");

        std::wcout << L"Capture started on " << device_.name
                   << L" using 1920x1080 @ 60 fps YUY2.\n";

        running_.store(true, std::memory_order_release);

        auto last_report = std::chrono::steady_clock::now();
        std::uint64_t frames_at_last_report = 0;

        while (!stop_token.stop_requested()) {
            DWORD stream_index = 0;
            DWORD flags = 0;
            LONGLONG timestamp = 0;
            IMFSample* sample = nullptr;

            const HRESULT read_result = reader->ReadSample(
                MF_SOURCE_READER_FIRST_VIDEO_STREAM,
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
