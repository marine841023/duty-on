#include "windows_audio_player.h"

#include <windows.h>
#include <initguid.h>
#include <propkeydef.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <utility>

namespace dutyon {
namespace {

using Microsoft::WRL::ComPtr;

std::string utf8(const wchar_t* text) {
    if (!text || !*text) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) return {};
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), size, nullptr, nullptr);
    result.pop_back();
    return result;
}

HRESULT buildTopology(IMFMediaSource* source, const std::wstring& endpoint,
                      IMFTopology** result) {
    ComPtr<IMFPresentationDescriptor> presentation;
    HRESULT hr = source->CreatePresentationDescriptor(&presentation);
    if (FAILED(hr)) return hr;

    DWORD stream_count = 0;
    hr = presentation->GetStreamDescriptorCount(&stream_count);
    if (FAILED(hr)) return hr;
    ComPtr<IMFStreamDescriptor> audio;
    for (DWORD i = 0; i < stream_count; ++i) {
        BOOL selected = FALSE;
        ComPtr<IMFStreamDescriptor> stream;
        ComPtr<IMFMediaTypeHandler> handler;
        GUID type{};
        if (FAILED(hr = presentation->GetStreamDescriptorByIndex(i, &selected, &stream)) ||
            FAILED(hr = stream->GetMediaTypeHandler(&handler)) ||
            FAILED(hr = handler->GetMajorType(&type))) return hr;
        if (!audio && type == MFMediaType_Audio) {
            hr = presentation->SelectStream(i);
            audio = std::move(stream);
        } else {
            hr = presentation->DeselectStream(i);
        }
        if (FAILED(hr)) return hr;
    }
    if (!audio) return MF_E_INVALIDMEDIATYPE;

    ComPtr<IMFActivate> renderer;
    hr = MFCreateAudioRendererActivate(&renderer);
    if (FAILED(hr)) return hr;
    if (!endpoint.empty()) {
        hr = renderer->SetString(MF_AUDIO_RENDERER_ATTRIBUTE_ENDPOINT_ID, endpoint.c_str());
        if (FAILED(hr)) return hr;
    }

    ComPtr<IMFTopology> topology;
    ComPtr<IMFTopologyNode> input, output;
    if (FAILED(hr = MFCreateTopology(&topology)) ||
        FAILED(hr = MFCreateTopologyNode(MF_TOPOLOGY_SOURCESTREAM_NODE, &input)) ||
        FAILED(hr = input->SetUnknown(MF_TOPONODE_SOURCE, source)) ||
        FAILED(hr = input->SetUnknown(MF_TOPONODE_PRESENTATION_DESCRIPTOR, presentation.Get())) ||
        FAILED(hr = input->SetUnknown(MF_TOPONODE_STREAM_DESCRIPTOR, audio.Get())) ||
        FAILED(hr = MFCreateTopologyNode(MF_TOPOLOGY_OUTPUT_NODE, &output)) ||
        FAILED(hr = output->SetObject(renderer.Get())) ||
        FAILED(hr = topology->AddNode(input.Get())) ||
        FAILED(hr = topology->AddNode(output.Get())) ||
        FAILED(hr = input->ConnectOutput(0, output.Get(), 0))) return hr;
    return topology.CopyTo(result);
}

void logFailure(HRESULT hr) {
    std::fprintf(stderr, "[Audio] Playback failed (0x%08lx)\n", static_cast<unsigned long>(hr));
}

}  // namespace

struct WindowsAudioPlayer::Impl {
    HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ComPtr<IMMDeviceEnumerator> devices;
    ComPtr<IMFMediaSession> session;
    ComPtr<IMFMediaSource> source;
    ComPtr<IMFAudioStreamVolume> stream_volume;
    int volume_percent = 100;
    bool mf_started = false;

    Impl() {
        if (FAILED(com) && com != RPC_E_CHANGED_MODE) return;
        CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                         IID_PPV_ARGS(&devices));
        mf_started = SUCCEEDED(MFStartup(MF_VERSION));
    }

    ~Impl() {
        stop();
        devices.Reset();
        if (mf_started) MFShutdown();
        if (SUCCEEDED(com)) CoUninitialize();
    }

    void stop() {
        stream_volume.Reset();
        if (session) {
            session->Shutdown();
            session.Reset();
        }
        if (source) {
            source->Shutdown();
            source.Reset();
        }
    }

    HRESULT applyVolume() const {
        UINT32 channels = 0;
        const HRESULT hr = stream_volume->GetChannelCount(&channels);
        if (FAILED(hr)) return hr;
        const std::vector<float> levels(channels, volume_percent / 100.0f);
        return stream_volume->SetAllVolumes(channels, levels.data());
    }
};

WindowsAudioPlayer::WindowsAudioPlayer() : impl_(std::make_unique<Impl>()) {}
WindowsAudioPlayer::~WindowsAudioPlayer() = default;

void WindowsAudioPlayer::setVolume(int percent) {
    percent = std::clamp(percent, 0, 100);
    if (impl_->volume_percent == percent) return;
    impl_->volume_percent = percent;
    if (impl_->stream_volume) {
        const HRESULT hr = impl_->applyVolume();
        if (FAILED(hr)) {
            logFailure(hr);
            stop();
        }
    }
}

std::vector<WindowsAudioPlayer::Output> WindowsAudioPlayer::outputs() const {
    std::vector<Output> result;
    ComPtr<IMMDeviceCollection> collection;
    if (!impl_->devices || FAILED(impl_->devices->EnumAudioEndpoints(
            eRender, DEVICE_STATE_ACTIVE, &collection))) return result;
    UINT count = 0;
    if (FAILED(collection->GetCount(&count))) return result;
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        ComPtr<IPropertyStore> props;
        LPWSTR raw_id = nullptr;
        if (FAILED(collection->Item(i, &device)) ||
            FAILED(device->OpenPropertyStore(STGM_READ, &props)) ||
            FAILED(device->GetId(&raw_id))) continue;
        auto id = utf8(raw_id);
        CoTaskMemFree(raw_id);
        PROPVARIANT name{};
        if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &name)) &&
            name.vt == VT_LPWSTR) {
            auto label = utf8(name.pwszVal);
            if (!id.empty() && !label.empty())
                result.push_back({std::move(id), std::move(label)});
        }
        PropVariantClear(&name);
    }
    return result;
}

void WindowsAudioPlayer::play(const std::string& path, const std::string& endpoint_id) {
    stop();
    if (!impl_->mf_started || path.empty()) return;
    const auto endpoint = std::filesystem::u8path(endpoint_id).wstring();
    if (!endpoint.empty()) {
        // Leave the app silent when the saved output is unavailable.
        ComPtr<IMMDevice> device;
        DWORD state = 0;
        if (!impl_->devices || FAILED(impl_->devices->GetDevice(endpoint.c_str(), &device)) ||
            FAILED(device->GetState(&state)) || state != DEVICE_STATE_ACTIVE) return;
    }
    ComPtr<IMFSourceResolver> resolver;
    ComPtr<IUnknown> object;
    ComPtr<IMFTopology> topology;
    MF_OBJECT_TYPE type = MF_OBJECT_INVALID;
    const auto file = std::filesystem::u8path(path).wstring();
    HRESULT hr;
    if (FAILED(hr = MFCreateSourceResolver(&resolver)) ||
        FAILED(hr = resolver->CreateObjectFromURL(file.c_str(), MF_RESOLUTION_MEDIASOURCE,
            nullptr, &type, &object)) ||
        FAILED(hr = object.As(&impl_->source)) ||
        FAILED(hr = buildTopology(impl_->source.Get(), endpoint, &topology)) ||
        FAILED(hr = MFCreateMediaSession(nullptr, &impl_->session)) ||
        FAILED(hr = impl_->session->SetTopology(0, topology.Get()))) {
        logFailure(hr);
        stop();
    }
}

void WindowsAudioPlayer::stop() { impl_->stop(); }

void WindowsAudioPlayer::tick() {
    while (impl_->session) {
        ComPtr<IMFMediaEvent> event;
        HRESULT hr = impl_->session->GetEvent(MF_EVENT_FLAG_NO_WAIT, &event);
        if (hr == MF_E_NO_EVENTS_AVAILABLE) return;
        MediaEventType type = MEUnknown;
        if (SUCCEEDED(hr)) hr = event->GetType(&type);
        if (SUCCEEDED(hr)) {
            HRESULT status = S_OK;
            hr = event->GetStatus(&status);
            if (SUCCEEDED(hr)) hr = status;
        }
        if (SUCCEEDED(hr) && type == MESessionTopologyStatus) {
            UINT32 topology_status = 0;
            hr = event->GetUINT32(MF_EVENT_TOPOLOGY_STATUS, &topology_status);
            if (SUCCEEDED(hr) && topology_status == MF_TOPOSTATUS_READY) {
                hr = MFGetService(impl_->session.Get(), MR_STREAM_VOLUME_SERVICE,
                                  IID_PPV_ARGS(&impl_->stream_volume));
                if (SUCCEEDED(hr)) hr = impl_->applyVolume();
                if (SUCCEEDED(hr)) {
                    PROPVARIANT start{};
                    hr = impl_->session->Start(&GUID_NULL, &start);
                }
            }
        }
        if (FAILED(hr)) logFailure(hr);
        if (FAILED(hr) || type == MESessionEnded) stop();
    }
}

}  // namespace dutyon
