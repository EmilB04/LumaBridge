#include "azoth_music_source.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <ksmedia.h>
#include <roapi.h>
#include <winstring.h>
#include <asyncinfo.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>

namespace luma::app::azoth {
namespace mediaabi {

// Leading ABI methods from Microsoft's windows.media.control.h (Universal API
// contract 7). Keeping these declarations local also supports MinGW, whose SDK
// does not include that header. These types need external linkage: Windows supplies
// their implementations, so the compiler must preserve virtual dispatch.
template<class T> struct MediaAsync : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE put_Completed(IUnknown*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Completed(IUnknown**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetResults(T**) = 0;
};
struct MediaProperties : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Title(HSTRING*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Subtitle(HSTRING*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_AlbumArtist(HSTRING*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Artist(HSTRING*) = 0;
};
struct PlaybackInfo : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Controls(IInspectable**) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_PlaybackStatus(int*) = 0;
};
// TimeSpan and DateTime are single 64-bit counts of 100 ns; DateTime counts from 1601
// in UTC, like FILETIME.
struct TimelineProperties : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_StartTime(int64_t*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_EndTime(int64_t*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_MinSeekTime(int64_t*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_MaxSeekTime(int64_t*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Position(int64_t*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_LastUpdatedTime(int64_t*) = 0;
};
struct MediaSession : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_SourceAppUserModelId(HSTRING*) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryGetMediaPropertiesAsync(MediaAsync<MediaProperties>**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetTimelineProperties(TimelineProperties**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPlaybackInfo(PlaybackInfo**) = 0;
};
struct MediaManager : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE GetCurrentSession(MediaSession**) = 0;
};
struct MediaManagerStatics : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE RequestAsync(MediaAsync<MediaManager>**) = 0;
};
} // namespace mediaabi

namespace {
using namespace mediaabi;
using Microsoft::WRL::ComPtr;
constexpr GUID kManagerStatics{0x2050c4ee, 0x11a0, 0x57de, {0xae,0xd7,0xc9,0x7c,0x70,0x33,0x82,0x45}};
constexpr GUID kAsyncInfo{0x00000036,0,0,{0xc0,0,0,0,0,0,0,0x46}};
constexpr GUID kPcm{1,0,0x0010,{0x80,0,0,0xaa,0,0x38,0x9b,0x71}};
constexpr GUID kFloat{3,0,0x0010,{0x80,0,0,0xaa,0,0x38,0x9b,0x71}};
std::string Failure(const char* what, HRESULT hr) {
    char text[160];
    std::snprintf(text, sizeof text, "%s (Windows error 0x%08lX).", what, static_cast<unsigned long>(hr));
    return text;
}
template<class T> bool Finish(ComPtr<MediaAsync<T>>& op, ComPtr<T>& result, HRESULT& error, uint64_t deadline) {
    if (!op) return false;
    ComPtr<IAsyncInfo> info;
    error = op->QueryInterface(kAsyncInfo, reinterpret_cast<void**>(info.GetAddressOf()));
    AsyncStatus status = Started;
    if (SUCCEEDED(error)) error = info->get_Status(&status);
    if (SUCCEEDED(error) && status == Started && GetTickCount64() < deadline) return false;
    if (SUCCEEDED(error) && status == Completed) error = op->GetResults(result.ReleaseAndGetAddressOf());
    else if (SUCCEEDED(error)) {
        if (status == Started) { info->Cancel(); error = HRESULT_FROM_WIN32(ERROR_TIMEOUT); }
        else { info->get_ErrorCode(&error); if (SUCCEEDED(error)) error = E_ABORT; }
    }
    op.Reset();
    return true;
}
template<class T> void Cancel(ComPtr<MediaAsync<T>>& op) {
    if (op) {
        ComPtr<IAsyncInfo> info;
        if (SUCCEEDED(op->QueryInterface(kAsyncInfo, reinterpret_cast<void**>(info.GetAddressOf())))) info->Cancel();
        op.Reset();
    }
}
std::wstring TakeString(HSTRING value) {
    UINT32 length = 0;
    const auto* text = WindowsGetStringRawBuffer(value, &length);
    // A malicious or broken player should not allocate an unbounded display string.
    std::wstring result(text ? text : L"", std::min<UINT32>(length, 512));
    WindowsDeleteString(value);
    return result;
}
int64_t UtcNow() {
    FILETIME now{};
    GetSystemTimePreciseAsFileTime(&now);
    return static_cast<int64_t>((uint64_t(now.dwHighDateTime) << 32) | now.dwLowDateTime);
}
}

struct MusicSource::Impl {
    HRESULT initialized = RoInitialize(RO_INIT_MULTITHREADED);
    OledContent mode = OledContent::Keep;
    MusicSnapshot snapshot;
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IAudioClient> audio;
    ComPtr<IAudioCaptureClient> capture;
    std::wstring endpoint;
    unsigned rate = 0, channels = 0, sampleBytes = 0, frameBytes = 0, validBits = 0;
    bool floatSamples = false;
    std::array<float, kSpectrumSamples> ring{};
    size_t cursor = 0;
    uint64_t nextEndpoint = 0, lastSamples = 0;
    SpectrumScale scale;
    int64_t reportedMs = 0, reportedAt = 0;
    bool playing = false;
    ComPtr<MediaManager> manager;
    ComPtr<MediaAsync<MediaManager>> managerOp;
    ComPtr<MediaAsync<MediaProperties>> propertiesOp;
    ComPtr<MediaSession> session;
    uint64_t nextSong = 0, deadline = 0;
    ~Impl() { Reset(); if (SUCCEEDED(initialized)) RoUninitialize(); }
    void CloseAudio() {
        if (audio) audio->Stop();
        capture.Reset(); audio.Reset(); endpoint.clear(); ring.fill(0); cursor = 0; lastSamples = 0; scale = {};
    }
    void Reset() {
        CloseAudio(); enumerator.Reset();
        Cancel(managerOp); Cancel(propertiesOp); session.Reset(); manager.Reset();
        snapshot = {}; nextEndpoint = nextSong = 0; mode = OledContent::Keep;
    }
    void Audio(uint64_t now) {
        HRESULT hr = S_OK;
        if (now >= nextEndpoint) {
            nextEndpoint = now + 2000;
            if (!enumerator) hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                                   IID_PPV_ARGS(enumerator.ReleaseAndGetAddressOf()));
            ComPtr<IMMDevice> device;
            if (SUCCEEDED(hr)) hr = enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &device);
            LPWSTR id = nullptr;
            if (SUCCEEDED(hr)) hr = device->GetId(&id);
            const std::wstring current = id ? id : L"";
            CoTaskMemFree(id);
            if (FAILED(hr) || current != endpoint || !capture) {
                CloseAudio();
                if (SUCCEEDED(hr)) hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                                       reinterpret_cast<void**>(audio.GetAddressOf()));
                WAVEFORMATEX* format = nullptr;
                if (SUCCEEDED(hr)) hr = audio->GetMixFormat(&format);
                if (SUCCEEDED(hr)) {
                    rate = format->nSamplesPerSec; channels = format->nChannels;
                    sampleBytes = format->wBitsPerSample / 8; frameBytes = format->nBlockAlign;
                    validBits = format->wBitsPerSample;
                    floatSamples = format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
                    bool pcm = format->wFormatTag == WAVE_FORMAT_PCM;
                    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE && format->cbSize >= 22) {
                        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
                        floatSamples = IsEqualGUID(ext->SubFormat, kFloat);
                        pcm = IsEqualGUID(ext->SubFormat, kPcm);
                        validBits = ext->Samples.wValidBitsPerSample;
                    }
                    if (!channels || channels > 32 || frameBytes != channels * sampleBytes ||
                        (!floatSamples && !pcm) || (floatSamples && sampleBytes != 4) ||
                        (!floatSamples && sampleBytes != 2 && sampleBytes != 3 && sampleBytes != 4) ||
                        validBits == 0 || validBits > sampleBytes * 8) hr = AUDCLNT_E_UNSUPPORTED_FORMAT;
                }
                if (SUCCEEDED(hr)) hr = audio->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
                                                         1000000, 0, format, nullptr);
                CoTaskMemFree(format);
                if (SUCCEEDED(hr)) hr = audio->GetService(IID_PPV_ARGS(capture.GetAddressOf()));
                if (SUCCEEDED(hr)) hr = audio->Start();
                if (SUCCEEDED(hr)) endpoint = current;
                else { CloseAudio(); snapshot.status = Failure("Can't read playback audio", hr); }
            }
        }
        if (!capture) { snapshot.levels = {}; return; }
        UINT32 count = 0;
        hr = capture->GetNextPacketSize(&count);
        unsigned packets = 0;
        while (SUCCEEDED(hr) && count && packets++ < 128) {
            BYTE* bytes = nullptr; DWORD flags = 0; UINT32 frames = 0;
            hr = capture->GetBuffer(&bytes, &frames, &flags, nullptr, nullptr);
            if (FAILED(hr)) break;
            if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) ring.fill(0);
            for (UINT32 frame = 0; frame < frames; ++frame) {
                double sum = 0;
                if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT) && bytes) {
                    for (unsigned channel = 0; channel < channels; ++channel) {
                        const BYTE* p = bytes + frame * frameBytes + channel * sampleBytes;
                        sum += PlaybackSample(p, sampleBytes, floatSamples);
                    }
                }
                ring[cursor] = static_cast<float>(sum / channels);
                cursor = (cursor + 1) % ring.size();
            }
            if (frames) lastSamples = now;
            hr = capture->ReleaseBuffer(frames);
            if (SUCCEEDED(hr)) hr = capture->GetNextPacketSize(&count);
        }
        if (FAILED(hr)) { CloseAudio(); snapshot.levels = {}; snapshot.status = Failure("Playback audio disconnected", hr); return; }
        Spectrum target{};
        if (lastSamples && now - lastSamples < 200) {
            std::array<float, kSpectrumSamples> samples{};
            for (size_t i = 0; i < samples.size(); ++i) samples[i] = ring[(cursor + i) % ring.size()];
            target = scale.Apply(AudioBands(samples, rate), now);
        }
        for (size_t i = 0; i < target.size(); ++i)
            snapshot.levels[i] = target[i] >= snapshot.levels[i] ? target[i] :
                                static_cast<uint8_t>(snapshot.levels[i] * .8);
        snapshot.status = "Listening to Windows playback audio";
    }
    void Song(uint64_t now) {
        HRESULT hr = S_OK;
        if (managerOp && Finish(managerOp, manager, hr, deadline)) {
            if (FAILED(hr)) { snapshot = {}; snapshot.status = Failure("Can't read Windows media sessions", hr); nextSong = now + 5000; }
        }
        if (propertiesOp) {
            ComPtr<MediaProperties> properties;
            if (!Finish(propertiesOp, properties, hr, deadline)) return;
            snapshot.title.clear(); snapshot.artist.clear();
            snapshot.durationMs = 0; playing = false;
            if (SUCCEEDED(hr) && properties) {
                HSTRING value = nullptr;
                hr = properties->get_Title(&value); snapshot.title = TakeString(value); value = nullptr;
                if (SUCCEEDED(hr)) hr = properties->get_Artist(&value);
                snapshot.artist = TakeString(value);
                snapshot.status = snapshot.title.empty() ? "The player hasn't shared a song title" : "Song info from Windows";
                ComPtr<PlaybackInfo> playback;
                int status = 0;
                if (SUCCEEDED(session->GetPlaybackInfo(&playback)) && playback && SUCCEEDED(playback->get_PlaybackStatus(&status))) {
                    if (status == 4) snapshot.status = "Playing"; else if (status == 5) snapshot.status = "Paused";
                    playing = status == 4;
                }
                ComPtr<TimelineProperties> timeline;
                int64_t start = 0, end = 0, position = 0, updated = 0;
                if (SUCCEEDED(session->GetTimelineProperties(&timeline)) && timeline &&
                    SUCCEEDED(timeline->get_StartTime(&start)) && SUCCEEDED(timeline->get_EndTime(&end)) &&
                    SUCCEEDED(timeline->get_Position(&position)) && SUCCEEDED(timeline->get_LastUpdatedTime(&updated)) &&
                    end > start) {
                    snapshot.durationMs = (end - start) / 10000;
                    reportedMs = (position - start) / 10000;
                    // Players that leave the report time empty are counted from now.
                    reportedAt = updated > 0 ? updated : UtcNow();
                }
            }
            if (FAILED(hr)) snapshot.status = Failure("Can't read song information", hr);
            session.Reset(); nextSong = now + 1000;
        }
        if (now < nextSong || managerOp || propertiesOp) return;
        nextSong = now + 1000;
        if (!manager) {
            HSTRING name = nullptr;
            const wchar_t cls[] = L"Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager";
            hr = WindowsCreateString(cls, static_cast<UINT32>(std::size(cls) - 1), &name);
            ComPtr<MediaManagerStatics> factory;
            if (SUCCEEDED(hr)) hr = RoGetActivationFactory(name, kManagerStatics, reinterpret_cast<void**>(factory.GetAddressOf()));
            WindowsDeleteString(name);
            if (SUCCEEDED(hr)) hr = factory->RequestAsync(managerOp.GetAddressOf());
            if (SUCCEEDED(hr)) { deadline = now + 3000; snapshot.status = "Reading Windows media sessions..."; }
            else { snapshot = {}; snapshot.status = Failure("Windows media sessions unavailable", hr); nextSong = now + 5000; }
        } else {
            hr = manager->GetCurrentSession(session.ReleaseAndGetAddressOf());
            if (SUCCEEDED(hr) && session) hr = session->TryGetMediaPropertiesAsync(propertiesOp.GetAddressOf());
            if (FAILED(hr)) { snapshot = {}; snapshot.status = Failure("Can't read the current player", hr); manager.Reset(); nextSong = now + 5000; }
            else if (!session) { snapshot = {}; snapshot.status = "No media session. Start playback in a supported player."; }
            else deadline = now + 3000;
        }
    }
};

MusicSource::MusicSource() : impl_(std::make_unique<Impl>()) {}
MusicSource::~MusicSource() = default;
void MusicSource::Reset() { impl_->Reset(); }
MusicSnapshot MusicSource::Poll(OledContent content) {
    auto& p = *impl_;
    if (content != p.mode) { p.Reset(); p.mode = content; }
    if (FAILED(p.initialized)) { p.snapshot.status = Failure("Windows media APIs unavailable", p.initialized); return p.snapshot; }
    if (content == OledContent::Equalizer) p.Audio(GetTickCount64());
    else if (content == OledContent::SongInfo) {
        p.Song(GetTickCount64());
        p.snapshot.positionMs = SongPosition(p.reportedMs, p.snapshot.durationMs, (UtcNow() - p.reportedAt) / 10000, p.playing);
    }
    return p.snapshot;
}

std::vector<uint8_t> RenderSong(const MusicSnapshot& song) {
    constexpr int width = 208, height = 64;
    std::vector<uint8_t> gray(width * height);
    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) return {};
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width; info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap || !bits) { if (bitmap) DeleteObject(bitmap); DeleteDC(dc); return {}; }
    auto oldBitmap = SelectObject(dc, bitmap);
    std::memset(bits, 0, width * height * 4);
    SetBkMode(dc, TRANSPARENT);
    auto font = [&](int size, int weight) {
        return CreateFontW(-size, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    };
    // Returns the drawn text's width.
    auto text = [&](const std::wstring& value, RECT rect, int size, int weight, UINT align, uint8_t shade) {
        HFONT f = font(size, weight);
        if (!f) return 0;
        auto oldFont = SelectObject(dc, f);
        SetTextColor(dc, RGB(shade, shade, shade));
        DrawTextW(dc, value.c_str(), static_cast<int>(value.size()), &rect,
                  align | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX | DT_VCENTER);
        SIZE extent{};
        GetTextExtentPoint32W(dc, value.c_str(), static_cast<int>(value.size()), &extent);
        SelectObject(dc, oldFont); DeleteObject(f);
        return static_cast<int>(extent.cx);
    };
    const std::wstring title = song.title.empty() ? L"No song playing" : song.title;
    const std::wstring artist = song.artist.empty() ? L"LumaBridge" : song.artist;
    if (song.durationMs > 0) {
        // Title, artist, then "0:37 ━━━━──── 4:03" along the bottom.
        text(title, {4, 0, width - 4, 24}, 17, FW_SEMIBOLD, DT_CENTER, 255);
        text(artist, {4, 22, width - 4, 40}, 13, FW_NORMAL, DT_CENTER, 200);
        const RECT row{3, 45, width - 3, 62};
        const int left = text(SongClock(song.positionMs), row, 11, FW_NORMAL, DT_LEFT, 190);
        const int right = text(SongClock(song.durationMs), row, 11, FW_NORMAL, DT_RIGHT, 190);
        const int x0 = row.left + left + 6, x1 = row.right - right - 6;
        if (x1 > x0) {
            const int fill = SongBarFill(song.positionMs, song.durationMs, x1 - x0);
            HBRUSH track = CreateSolidBrush(RGB(64, 64, 64)), done = CreateSolidBrush(RGB(255, 255, 255));
            const RECT all{x0, 52, x1, 55}, played{x0, 52, x0 + fill, 55};
            if (track) FillRect(dc, &all, track);
            if (done && fill > 0) FillRect(dc, &played, done);
            if (track) DeleteObject(track);
            if (done) DeleteObject(done);
        }
    } else {
        text(title, {4, 5, width - 4, 30}, 18, FW_SEMIBOLD, DT_CENTER, 255);
        text(artist, {4, 34, width - 4, 55}, 14, FW_NORMAL, DT_CENTER, 255);
    }
    GdiFlush();
    const auto* pixels = static_cast<const uint8_t*>(bits);
    for (size_t i = 0; i < gray.size(); ++i) gray[i] = std::max({pixels[i*4],pixels[i*4+1],pixels[i*4+2]});
    SelectObject(dc, oldBitmap); DeleteObject(bitmap); DeleteDC(dc);
    return gray;
}

} // namespace luma::app::azoth
