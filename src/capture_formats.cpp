#include "capture_formats.h"

#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <objbase.h>

#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

void throw_if_failed(HRESULT result, const char* message) {
    if (FAILED(result)) {
        throw std::runtime_error(
            std::string(message) + " (HRESULT=" + std::to_string(static_cast<long>(result)) + ")"
        );
    }
}

std::wstring subtype_name(const GUID& subtype) {
    if (subtype == MFVideoFormat_MJPG) {
        return L"MJPG";
    }
    if (subtype == MFVideoFormat_YUY2) {
        return L"YUY2";
    }
    if (subtype == MFVideoFormat_NV12) {
        return L"NV12";
    }
    if (subtype == MFVideoFormat_RGB32) {
        return L"RGB32";
    }
    if (subtype == MFVideoFormat_RGB24) {
        return L"RGB24";
    }
    if (subtype == MFVideoFormat_UYVY) {
        return L"UYVY";
    }
    if (subtype == MFVideoFormat_H264) {
        return L"H264";
    }
    if (subtype == MFVideoFormat_I420) {
        return L"I420";
    }
    if (subtype == MFVideoFormat_YV12) {
        return L"YV12";
    }

    wchar_t buffer[64]{};
    if (StringFromGUID2(subtype, buffer, static_cast<int>(std::size(buffer))) > 0) {
        return buffer;
    }

    return L"unknown";
}

IMFMediaSource* create_media_source(const CaptureDeviceInfo& device) {
    IMFAttributes* attributes = nullptr;
    IMFMediaSource* media_source = nullptr;

    throw_if_failed(
        MFCreateAttributes(&attributes, 2),
        "Failed to create device-source attributes"
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
    throw_if_failed(result, "Failed to open video capture device");
    return media_source;
}

} // namespace

std::vector<CaptureFormatInfo> enumerate_video_formats(const CaptureDeviceInfo& device) {
    IMFMediaSource* media_source = create_media_source(device);
    IMFSourceReader* reader = nullptr;

    const HRESULT reader_result = MFCreateSourceReaderFromMediaSource(
        media_source,
        nullptr,
        &reader
    );

    if (FAILED(reader_result)) {
        media_source->Shutdown();
        media_source->Release();
        throw_if_failed(reader_result, "Failed to create Media Foundation source reader");
    }

    std::vector<CaptureFormatInfo> formats;

    for (DWORD index = 0;; ++index) {
        IMFMediaType* media_type = nullptr;
        const HRESULT type_result = reader->GetNativeMediaType(
            MF_SOURCE_READER_FIRST_VIDEO_STREAM,
            index,
            &media_type
        );

        if (type_result == MF_E_NO_MORE_TYPES) {
            break;
        }

        if (FAILED(type_result)) {
            reader->Release();
            media_source->Shutdown();
            media_source->Release();
            throw_if_failed(type_result, "Failed to enumerate native video media types");
        }

        CaptureFormatInfo format;

        UINT32 width = 0;
        UINT32 height = 0;
        if (SUCCEEDED(MFGetAttributeSize(media_type, MF_MT_FRAME_SIZE, &width, &height))) {
            format.width = width;
            format.height = height;
        }

        UINT32 fps_numerator = 0;
        UINT32 fps_denominator = 1;
        if (SUCCEEDED(MFGetAttributeRatio(
                media_type,
                MF_MT_FRAME_RATE,
                &fps_numerator,
                &fps_denominator
            ))) {
            format.fps_numerator = fps_numerator;
            format.fps_denominator = fps_denominator == 0 ? 1 : fps_denominator;
        }

        GUID subtype{};
        if (SUCCEEDED(media_type->GetGUID(MF_MT_SUBTYPE, &subtype))) {
            format.subtype = subtype_name(subtype);
        } else {
            format.subtype = L"unknown";
        }

        formats.push_back(std::move(format));
        media_type->Release();
    }

    reader->Release();
    media_source->Shutdown();
    media_source->Release();

    return formats;
}
