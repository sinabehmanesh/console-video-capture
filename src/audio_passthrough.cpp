#include "audio_passthrough.h"

#include <windows.h>
#include <audioclient.h>
#include <avrt.h>
#include <propkeydef.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>
#include <propvarutil.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <deque>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

constexpr wchar_t kCaptureHardwareId[] = L"vid_345f&pid_2131";
constexpr wchar_t kCaptureAudioFriendlyName[] = L"usb2 digital audio";

void throw_if_failed(HRESULT result, const char* message) {
    if (FAILED(result)) {
        throw std::runtime_error(
            std::string(message) + " (HRESULT=" + std::to_string(static_cast<long>(result)) + ")"
        );
    }
}

std::wstring lowercase(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    return value;
}

std::wstring device_property_string(IMMDevice* device, const PROPERTYKEY& key) {
    IPropertyStore* store = nullptr;
    if (FAILED(device->OpenPropertyStore(STGM_READ, &store)) || store == nullptr) {
        return {};
    }

    PROPVARIANT value;
    PropVariantInit(&value);
    std::wstring result;

    if (SUCCEEDED(store->GetValue(key, &value)) && value.vt == VT_LPWSTR && value.pwszVal != nullptr) {
        result = value.pwszVal;
    }

    PropVariantClear(&value);
    store->Release();
    return result;
}

std::wstring endpoint_id(IMMDevice* device) {
    LPWSTR id = nullptr;
    if (FAILED(device->GetId(&id)) || id == nullptr) {
        return {};
    }

    std::wstring result = id;
    CoTaskMemFree(id);
    return result;
}

bool endpoint_matches_capture_card(IMMDevice* device) {
    const std::wstring friendly = lowercase(device_property_string(device, PKEY_Device_FriendlyName));
    const std::wstring instance = lowercase(device_property_string(device, PKEY_Device_InstanceId));
    const std::wstring id = lowercase(endpoint_id(device));

    const bool hardware_id_match =
        friendly.find(kCaptureHardwareId) != std::wstring::npos ||
        instance.find(kCaptureHardwareId) != std::wstring::npos ||
        id.find(kCaptureHardwareId) != std::wstring::npos;

    // Windows exposes this capture card's audio interface as a separate USB
    // audio endpoint, so its MMDevice properties do not necessarily contain
    // the video interface VID/PID string. Use the observed friendly name as a
    // narrow fallback after trying the stable hardware-id match first.
    const bool friendly_name_match =
        friendly.find(kCaptureAudioFriendlyName) != std::wstring::npos;

    return hardware_id_match || friendly_name_match;
}

IMMDevice* find_capture_audio_endpoint(IMMDeviceEnumerator* enumerator) {
    IMMDeviceCollection* collection = nullptr;
    throw_if_failed(
        enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &collection),
        "Failed to enumerate audio capture endpoints"
    );

    UINT count = 0;
    throw_if_failed(collection->GetCount(&count), "Failed to count audio capture endpoints");

    std::wcout << L"Audio capture endpoints found: " << count << L'\n';

    IMMDevice* selected = nullptr;

    for (UINT index = 0; index < count; ++index) {
        IMMDevice* device = nullptr;
        if (FAILED(collection->Item(index, &device)) || device == nullptr) {
            continue;
        }

        const std::wstring friendly = device_property_string(device, PKEY_Device_FriendlyName);
        const std::wstring instance = device_property_string(device, PKEY_Device_InstanceId);

        std::wcout << L"[audio " << index << L"] "
                   << (friendly.empty() ? L"<unnamed>" : friendly);
        if (!instance.empty()) {
            std::wcout << L"\n    " << instance;
        }
        std::wcout << L'\n';

        if (selected == nullptr && endpoint_matches_capture_card(device)) {
            selected = device;
        } else {
            device->Release();
        }
    }

    collection->Release();
    return selected;
}

std::wstring friendly_name(IMMDevice* device) {
    const std::wstring value = device_property_string(device, PKEY_Device_FriendlyName);
    return value.empty() ? L"<unnamed>" : value;
}

} // namespace

AudioPassthrough::~AudioPassthrough() {
    stop();
}

void AudioPassthrough::start() {
    if (thread_.joinable()) {
        return;
    }

    thread_ = std::jthread([this](std::stop_token stop_token) {
        audio_loop(stop_token);
    });
}

void AudioPassthrough::stop() {
    if (!thread_.joinable()) {
        return;
    }

    thread_.request_stop();
    thread_.join();
}

bool AudioPassthrough::running() const noexcept {
    return running_.load(std::memory_order_acquire);
}

void AudioPassthrough::audio_loop(std::stop_token stop_token) {
    const HRESULT com_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com_result)) {
        std::cerr << "Audio thread COM initialization failed: "
                  << static_cast<long>(com_result) << '\n';
        return;
    }

    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* capture_device = nullptr;
    IMMDevice* render_device = nullptr;
    IAudioClient* capture_client = nullptr;
    IAudioClient* render_client = nullptr;
    IAudioCaptureClient* capture_service = nullptr;
    IAudioRenderClient* render_service = nullptr;
    WAVEFORMATEX* capture_format = nullptr;
    HANDLE capture_event = nullptr;
    HANDLE render_event = nullptr;
    HANDLE mmcss_handle = nullptr;
    DWORD mmcss_task_index = 0;

    try {
        throw_if_failed(
            CoCreateInstance(
                __uuidof(MMDeviceEnumerator),
                nullptr,
                CLSCTX_ALL,
                __uuidof(IMMDeviceEnumerator),
                reinterpret_cast<void**>(&enumerator)
            ),
            "Failed to create MMDevice enumerator"
        );

        capture_device = find_capture_audio_endpoint(enumerator);
        if (capture_device == nullptr) {
            std::cerr << "Audio passthrough disabled: capture-card audio endpoint was not found.\n";
            enumerator->Release();
            CoUninitialize();
            return;
        }

        throw_if_failed(
            enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &render_device),
            "Failed to get default audio output endpoint"
        );

        std::wcout << L"Selected audio input: " << friendly_name(capture_device) << L'\n';
        std::wcout << L"Selected audio output: " << friendly_name(render_device) << L'\n';

        throw_if_failed(
            capture_device->Activate(
                __uuidof(IAudioClient),
                CLSCTX_ALL,
                nullptr,
                reinterpret_cast<void**>(&capture_client)
            ),
            "Failed to activate capture audio client"
        );

        throw_if_failed(
            render_device->Activate(
                __uuidof(IAudioClient),
                CLSCTX_ALL,
                nullptr,
                reinterpret_cast<void**>(&render_client)
            ),
            "Failed to activate render audio client"
        );

        throw_if_failed(
            capture_client->GetMixFormat(&capture_format),
            "Failed to get capture audio format"
        );

        const DWORD render_flags =
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
            AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
            AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;

        throw_if_failed(
            capture_client->Initialize(
                AUDCLNT_SHAREMODE_SHARED,
                AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                0,
                0,
                capture_format,
                nullptr
            ),
            "Failed to initialize capture audio client"
        );

        throw_if_failed(
            render_client->Initialize(
                AUDCLNT_SHAREMODE_SHARED,
                render_flags,
                0,
                0,
                capture_format,
                nullptr
            ),
            "Failed to initialize render audio client"
        );

        capture_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        render_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (capture_event == nullptr || render_event == nullptr) {
            throw std::runtime_error("Failed to create WASAPI event handles");
        }

        throw_if_failed(capture_client->SetEventHandle(capture_event), "Failed to bind capture event");
        throw_if_failed(render_client->SetEventHandle(render_event), "Failed to bind render event");

        throw_if_failed(
            capture_client->GetService(
                __uuidof(IAudioCaptureClient),
                reinterpret_cast<void**>(&capture_service)
            ),
            "Failed to get audio capture service"
        );

        throw_if_failed(
            render_client->GetService(
                __uuidof(IAudioRenderClient),
                reinterpret_cast<void**>(&render_service)
            ),
            "Failed to get audio render service"
        );

        UINT32 render_buffer_frames = 0;
        throw_if_failed(
            render_client->GetBufferSize(&render_buffer_frames),
            "Failed to get render buffer size"
        );

        const std::size_t block_align = capture_format->nBlockAlign;
        if (block_align == 0) {
            throw std::runtime_error("Capture audio format has an invalid block alignment");
        }

        const std::size_t max_pending_bytes =
            static_cast<std::size_t>(capture_format->nAvgBytesPerSec) / 20;

        std::deque<std::uint8_t> pending_audio;

        mmcss_handle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &mmcss_task_index);

        throw_if_failed(capture_client->Start(), "Failed to start audio capture");
        throw_if_failed(render_client->Start(), "Failed to start audio render");

        running_.store(true, std::memory_order_release);
        std::cout << "Audio passthrough started.\n";

        HANDLE events[2] = {capture_event, render_event};

        while (!stop_token.stop_requested()) {
            const DWORD wait_result = WaitForMultipleObjects(2, events, FALSE, 20);
            if (wait_result == WAIT_FAILED) {
                throw std::runtime_error("WASAPI event wait failed");
            }

            UINT32 next_packet_frames = 0;
            if (SUCCEEDED(capture_service->GetNextPacketSize(&next_packet_frames))) {
                while (next_packet_frames > 0) {
                    BYTE* data = nullptr;
                    UINT32 frames = 0;
                    DWORD flags = 0;

                    throw_if_failed(
                        capture_service->GetBuffer(
                            &data,
                            &frames,
                            &flags,
                            nullptr,
                            nullptr
                        ),
                        "Failed to read captured audio"
                    );

                    const std::size_t packet_bytes =
                        static_cast<std::size_t>(frames) * block_align;

                    if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || data == nullptr) {
                        pending_audio.insert(pending_audio.end(), packet_bytes, 0);
                    } else {
                        pending_audio.insert(pending_audio.end(), data, data + packet_bytes);
                    }

                    throw_if_failed(
                        capture_service->ReleaseBuffer(frames),
                        "Failed to release captured audio buffer"
                    );

                    if (pending_audio.size() > max_pending_bytes) {
                        const std::size_t excess = pending_audio.size() - max_pending_bytes;
                        const std::size_t aligned_excess = excess - (excess % block_align);
                        for (std::size_t i = 0; i < aligned_excess; ++i) {
                            pending_audio.pop_front();
                        }
                    }

                    throw_if_failed(
                        capture_service->GetNextPacketSize(&next_packet_frames),
                        "Failed to query next captured audio packet"
                    );
                }
            }

            UINT32 padding_frames = 0;
            throw_if_failed(
                render_client->GetCurrentPadding(&padding_frames),
                "Failed to query render padding"
            );

            const UINT32 available_frames = render_buffer_frames - padding_frames;
            const UINT32 pending_frames = static_cast<UINT32>(pending_audio.size() / block_align);
            const UINT32 frames_to_write = std::min(available_frames, pending_frames);

            if (frames_to_write > 0) {
                BYTE* output = nullptr;
                throw_if_failed(
                    render_service->GetBuffer(frames_to_write, &output),
                    "Failed to get audio render buffer"
                );

                const std::size_t bytes_to_write =
                    static_cast<std::size_t>(frames_to_write) * block_align;

                for (std::size_t i = 0; i < bytes_to_write; ++i) {
                    output[i] = pending_audio.front();
                    pending_audio.pop_front();
                }

                throw_if_failed(
                    render_service->ReleaseBuffer(frames_to_write, 0),
                    "Failed to release audio render buffer"
                );
            }
        }

        capture_client->Stop();
        render_client->Stop();
    } catch (const std::exception& error) {
        std::cerr << "Audio passthrough error: " << error.what() << '\n';
    }

    running_.store(false, std::memory_order_release);

    if (mmcss_handle != nullptr) {
        AvRevertMmThreadCharacteristics(mmcss_handle);
    }
    if (capture_event != nullptr) {
        CloseHandle(capture_event);
    }
    if (render_event != nullptr) {
        CloseHandle(render_event);
    }
    if (capture_format != nullptr) {
        CoTaskMemFree(capture_format);
    }
    if (capture_service != nullptr) {
        capture_service->Release();
    }
    if (render_service != nullptr) {
        render_service->Release();
    }
    if (capture_client != nullptr) {
        capture_client->Release();
    }
    if (render_client != nullptr) {
        render_client->Release();
    }
    if (capture_device != nullptr) {
        capture_device->Release();
    }
    if (render_device != nullptr) {
        render_device->Release();
    }
    if (enumerator != nullptr) {
        enumerator->Release();
    }

    CoUninitialize();
}
