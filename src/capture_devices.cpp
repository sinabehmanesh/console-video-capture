#include "capture_devices.h"

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <objbase.h>

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

std::wstring get_allocated_string(IMFActivate* activate, REFGUID key) {
    wchar_t* value = nullptr;
    UINT32 length = 0;

    const HRESULT result = activate->GetAllocatedString(key, &value, &length);
    if (FAILED(result)) {
        return {};
    }

    std::wstring text(value, length);
    CoTaskMemFree(value);
    return text;
}

} // namespace

std::vector<CaptureDeviceInfo> enumerate_video_capture_devices() {
    IMFAttributes* attributes = nullptr;
    IMFActivate** devices = nullptr;
    UINT32 device_count = 0;

    throw_if_failed(
        MFCreateAttributes(&attributes, 1),
        "Failed to create Media Foundation attributes"
    );

    const HRESULT set_type_result = attributes->SetGUID(
        MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
        MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID
    );

    if (FAILED(set_type_result)) {
        attributes->Release();
        throw_if_failed(set_type_result, "Failed to configure video capture enumeration");
    }

    const HRESULT enum_result = MFEnumDeviceSources(attributes, &devices, &device_count);
    attributes->Release();
    throw_if_failed(enum_result, "Failed to enumerate video capture devices");

    std::vector<CaptureDeviceInfo> result;
    result.reserve(device_count);

    for (UINT32 i = 0; i < device_count; ++i) {
        CaptureDeviceInfo info;
        info.name = get_allocated_string(devices[i], MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME);
        info.symbolic_link = get_allocated_string(
            devices[i],
            MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK
        );

        result.push_back(std::move(info));
        devices[i]->Release();
    }

    CoTaskMemFree(devices);
    return result;
}
