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
#include <utility>

namespace {

constexpr DWORD kVideoStream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);

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

const wchar_t* subtype_name(const GUID& subtype) {
    if (subtype == MFVideoFormat_NV12) return L"NV12";
    if (subtype == MFVideoFormat_MJPG) return L"MJPG";
    if (subtype == MFVideoFormat_YUY2) return L"YUY2";
    if (subtype == MFVideoFormat_RGB32) return L"RGB32";
    if (subtype == MFVideoFormat_H264) return L"H264";
    return L"other";
}

void log_native_formats(IMFSourceReader* reader) {
    std::wcout << L"Native capture formats:\n";

    for (DWORD index = 0;; ++index) {
        IMFMediaType* media_type = nullptr;
        const HRESULT result = reader->GetNativeMediaType(kVideoStream, index, &media_type);

        if (result == MF_E_NO_MORE_TYPES) break;
        if (FAILED(result)) break;

        GUID subtype{};
        UINT32 width = 0;
        UINT32 height = 0;
        UINT32 fps_num = 0;
        UINT32 fps_den = 1;

        media_type->GetGUID(MF_MT_SUBTYPE, &subtype);
        MFGetAttributeSize(media_type, MF_MT_FRAME_SIZE, &width, &height);
        MFGetAttributeRatio(media_type, MF_MT_FRAME_RATE, &fps_num, &fps_den);

        const double fps = fps_den == 0
            ? 0.0
            : static_cast<double>(fps_num) / static_cast<double>(fps_den);

        std::wcout << L"  [" << index << L"] "
                   << width << L"x" << height << L" @ " << fps
                   << L" " << subtype_name(subtype) << L'\n';

        media_type->Release();
    }
}

bool select_native_mode(
    IMFSourceReader* reader,
    const GUID& wanted_subtype,
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t fps
) {
    for (DWORD index = 0;; ++index) {
        IMFMediaType* media_type = nullptr;
        const HRESULT result = reader->GetNativeMediaType(kVideoStream, index, &media_type);

        if (result == MF_E_NO_MORE_TYPES) break;
        if (FAILED(result)) return false;

        GUID subtype{};
        UINT32 type_width = 0;
        UINT32 type_height = 0;
        UINT32 fps_num = 0;
        UINT32 fps_den = 1;

        const bool matches =
            SUCCEEDED(media_type->GetGUID(MF_MT_SUBTYPE, &subtype)) &&
            subtype == wanted_subtype &&
            SUCCEEDED(MFGetAttributeSize(media_type, MF_MT_FRAME_SIZE, &type_width, &type_height)) &&
            type_width == width &&
            type_height == height &&
            SUCCEEDED(MFGetAttributeRatio(media_type, MF_MT_FRAME_RATE, &fps_num, &fps_den)) &&
            fps_den != 0 &&
            fps_num == fps * fps_den;

        if (matches) {
            const HRESULT set_result = reader->SetCurrentMediaType(
                kVideoStream,
                nullptr,
                media_type
            );
            media_type->Release();
            return SUCCEEDED(set_result);
        }

        media_type->Release();
    }

    return false;
}

bool request_nv12_output(
    IMFSourceReader* reader,
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t fps
) {
    IMFMediaType* output_type = nullptr;
    if (FAILED(MFCreateMediaType(&output_type)) || output_type == nullptr) return false;

    HRESULT result = output_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(result)) result = output_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    if (SUCCEEDED(result)) result = MFSetAttributeSize(output_type, MF_MT_FRAME_SIZE, width, height);
    if (SUCCEEDED(result)) result = MFSetAttributeRatio(output_type, MF_MT_FRAME_RATE, fps, 1);
    if (SUCCEEDED(result)) result = MFSetAttributeRatio(output_type, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (SUCCEEDED(result)) {
        result = output_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    }
    if (SUCCEEDED(result)) {
        result = reader->SetCurrentMediaType(kVideoStream, nullptr, output_type);
    }

    output_type->Release();
    return SUCCEEDED(result);
}

bool copy_nv12_sample(
    IMFSample* sample,
    std::vector<std::uint8_t>& destination,
    std::uint32_t width,
    std::uint32_t height
) {
    IMFMediaBuffer* buffer = nullptr;
    if (FAILED(sample->ConvertToContiguousBuffer(&buffer)) || buffer == nullptr) {
        return false;
    }

    const std::size_t y_bytes = static_cast<std::size_t>(width) * height;
    const std::size_t frame_bytes = y_bytes + y_bytes / 2;
    bool copied = false;

    IMF2DBuffer* buffer_2d = nullptr;
    if (SUCCEEDED(buffer->QueryInterface(IID_PPV_ARGS(&buffer_2d))) && buffer_2d != nullptr) {
        BYTE* scanline = nullptr;
        LONG pitch = 0;

        if (SUCCEEDED(buffer_2d->Lock2D(&scanline, &pitch)) &&
            scanline != nullptr &&
            pitch >= static_cast<LONG>(width)) {

            destination.resize(frame_bytes);
            const std::size_t stride = static_cast<std::size_t>(pitch);

            for (std::uint32_t row = 0; row < height; ++row) {
                std::memcpy(
                    destination.data() + static_cast<std::size_t>(row) * width,
                    scanline + static_cast<std::size_t>(row) * stride,
                    width
                );
            }

            const BYTE* uv_plane = scanline + static_cast<std::size_t>(height) * stride;
            std::uint8_t* uv_destination = destination.data() + y_bytes;

            for (std::uint32_t row = 0; row < height / 2; ++row) {
                std::memcpy(
                    uv_destination + static_cast<std::size_t>(row) * width,
                    uv_plane + static_cast<std::size_t>(row) * stride,
                    width
                );
            }

            copied = true;
            buffer_2d->Unlock2D();
        }

        buffer_2d->Release();
    }

    if (!copied) {
        BYTE* data = nullptr;
        DWORD max_length = 0;
        DWORD current_length = 0;

        if (SUCCEEDED(buffer->Lock(&data, &max_length, &current_length)) && data != nullptr) {
            if (current_length >= frame_bytes) {
                destination.resize(frame_bytes);
                std::memcpy(destination.data(), data, frame_bytes);
                copied = true;
            } else {
                static bool warned_short_buffer = false;
                if (!warned_short_buffer) {
                    std::cerr << "NV12 sample is smaller than expected: "
                              << current_length << " bytes, expected at least "
                              << frame_bytes << " bytes.\n";
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

CaptureSession::CaptureMode CaptureSession::choose_capture_mode() {
    std::cout << "\nXbox capture mode:\n"
              << "  [1] 1280x720 @ 60 fps  NV12 native\n"
              << "  [2] 1920x1080 @ 50 fps NV12 native\n"
              << "  [3] 1920x1080 @ 30 fps NV12 native\n"
              << "  [4] 1920x1080 @ 50 fps MJPEG -> NV12 (diagnostic)\n"
              << "  [5] 1920x1080 @ 30 fps MJPEG -> NV12 (diagnostic)\n"
              << "Select mode [1]: ";

    std::string choice;
    std::getline(std::cin, choice);

    if (choice == "2") return {1920, 1080, 50, CaptureSource::NativeNV12};
    if (choice == "3") return {1920, 1080, 30, CaptureSource::NativeNV12};
    if (choice == "4") return {1920, 1080, 50, CaptureSource::NativeMJPGToNV12};
    if (choice == "5") return {1920, 1080, 30, CaptureSource::NativeMJPGToNV12};
    return {1280, 720, 60, CaptureSource::NativeNV12};
}

CaptureSession::CaptureSession(CaptureDeviceInfo device)
    : device_(std::move(device)), mode_(choose_capture_mode()) {}

CaptureSession::~CaptureSession() {
    stop();
}

void CaptureSession::start() {
    if (thread_.joinable()) return;

    {
        std::scoped_lock lock(frame_mutex_);
        latest_frame_.resize(frame_bytes());
        latest_frame_sequence_ = 0;
    }

    frame_count_.store(0, std::memory_order_relaxed);
    thread_ = std::jthread([this](std::stop_token stop_token) {
        capture_loop(stop_token);
    });
}

void CaptureSession::stop() {
    if (!thread_.joinable()) return;
    thread_.request_stop();
    thread_.join();
}

std::uint64_t CaptureSession::frame_count() const noexcept {
    return frame_count_.load(std::memory_order_relaxed);
}

bool CaptureSession::running() const noexcept {
    return running_.load(std::memory_order_acquire);
}

std::uint32_t CaptureSession::capture_width() const noexcept {
    return mode_.width;
}

std::uint32_t CaptureSession::capture_height() const noexcept {
    return mode_.height;
}

std::uint32_t CaptureSession::capture_fps() const noexcept {
    return mode_.fps;
}

std::size_t CaptureSession::frame_bytes() const noexcept {
    const std::size_t y_bytes = static_cast<std::size_t>(mode_.width) * mode_.height;
    return y_bytes + y_bytes / 2;
}

const char* CaptureSession::source_format_name() const noexcept {
    return mode_.source == CaptureSource::NativeNV12 ? "NV12 native" : "MJPEG -> NV12";
}

bool CaptureSession::copy_latest_frame(
    std::vector<std::uint8_t>& destination,
    std::uint64_t& sequence
) const {
    std::scoped_lock lock(frame_mutex_);

    if (latest_frame_sequence_ == 0 || latest_frame_sequence_ == sequence) {
        return false;
    }

    destination = latest_frame_;
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
            MFCreateAttributes(&reader_attributes, 2),
            "Failed to create source-reader attributes"
        );
        reader_attributes->SetUINT32(MF_LOW_LATENCY, TRUE);
        if (mode_.source == CaptureSource::NativeMJPGToNV12) {
            reader_attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
        }

        const HRESULT reader_result = MFCreateSourceReaderFromMediaSource(
            media_source,
            reader_attributes,
            &reader
        );
        reader_attributes->Release();
        throw_if_failed(reader_result, "Failed to create capture source reader");

        log_native_formats(reader);

        if (mode_.source == CaptureSource::NativeNV12) {
            if (!select_native_mode(reader, MFVideoFormat_NV12, mode_.width, mode_.height, mode_.fps)) {
                throw std::runtime_error("Selected native NV12 capture mode is not available on this device");
            }
        } else {
            if (!select_native_mode(reader, MFVideoFormat_MJPG, mode_.width, mode_.height, mode_.fps)) {
                throw std::runtime_error("Selected native MJPEG capture mode is not available on this device");
            }
            if (!request_nv12_output(reader, mode_.width, mode_.height, mode_.fps)) {
                throw std::runtime_error("Media Foundation could not decode MJPEG to NV12");
            }
        }

        std::wcout << L"Capture started on " << device_.name << L" using "
                   << (mode_.source == CaptureSource::NativeNV12 ? L"native NV12 " : L"native MJPEG -> NV12 ")
                   << mode_.width << L"x" << mode_.height
                   << L" @ " << mode_.fps << L" fps.\n";

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

            throw_if_failed(read_result, "ReadSample failed");

            if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
                if (sample != nullptr) sample->Release();
                break;
            }

            if (sample != nullptr) {
                std::vector<std::uint8_t> captured_frame;
                if (copy_nv12_sample(sample, captured_frame, mode_.width, mode_.height)) {
                    {
                        std::scoped_lock lock(frame_mutex_);
                        latest_frame_.swap(captured_frame);
                        ++latest_frame_sequence_;
                    }

                    if (!reported_first_frame) {
                        std::cout << "First " << source_format_name() << " frame published to renderer ("
                                  << frame_bytes() << " bytes).\n";
                        reported_first_frame = true;
                    }

                    frame_count_.fetch_add(1, std::memory_order_relaxed);
                }

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

    if (reader != nullptr) reader->Release();
    if (media_source != nullptr) {
        media_source->Shutdown();
        media_source->Release();
    }

    CoUninitialize();
}
