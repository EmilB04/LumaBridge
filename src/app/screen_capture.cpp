#include "screen_capture.h"

#include <d3d11.h>
#include <dxgi1_5.h>

#include <cstring>
#include <vector>

#include "log.h"

namespace luma::app {
namespace {

template <typename T>
void Release(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

constexpr DWORD kFrameMs = 80;  // ~12 summaries per second
constexpr UINT kMaxSampleWidth = 64;

}  // namespace

void ScreenCapture::Start() {
    if (thread_.joinable()) return;
    stop_ = false;
    thread_ = std::thread(&ScreenCapture::Run, this);
}

void ScreenCapture::Stop() {
    if (!thread_.joinable()) return;
    stop_ = true;
    thread_.join();
    std::lock_guard<std::mutex> lock(mutex_);
    have_ = false;
    problem_.clear();
}

std::string ScreenCapture::problem() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return problem_;
}

bool ScreenCapture::Latest(games::ScreenColors* out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!have_) return false;
    *out = latest_;
    return true;
}

void ScreenCapture::Run() {
    LUMA_INFO("screen colors: started");
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    IDXGIOutputDuplication* dup = nullptr;
    ID3D11Texture2D* mipTex = nullptr;   // full size, with a mip chain
    ID3D11ShaderResourceView* mipSrv = nullptr;
    ID3D11Texture2D* staging = nullptr;  // one small mip level, CPU-readable
    UINT mipLevel = 0, sampleW = 0, sampleH = 0, texW = 0, texH = 0;
    int failures = 0;
    bool loggedFrame = false;
    uint64_t blackSince = 0;  // the screen has read as black since then (0: it hasn't)
    auto setProblem = [&](const std::string& p) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (p != problem_ && !p.empty()) LUMA_WARN("screen colors: %s", p.c_str());
        problem_ = p;
    };

    auto resetDup = [&] {
        Release(staging);
        Release(mipSrv);
        Release(mipTex);
        Release(dup);
        texW = texH = 0;
    };

    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                 &device, nullptr, &context))) {
        LUMA_WARN("screen colors: no Direct3D 11 device");
        return;
    }

    std::vector<uint8_t> pixels;
    while (!stop_) {
        if (!dup) {
            IDXGIDevice* dxgiDevice = nullptr;
            IDXGIAdapter* adapter = nullptr;
            IDXGIOutput* output = nullptr;
            IDXGIOutput1* output1 = nullptr;
            HRESULT hr = device->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
            if (SUCCEEDED(hr)) hr = dxgiDevice->GetAdapter(&adapter);
            if (SUCCEEDED(hr)) hr = adapter->EnumOutputs(0, &output);  // primary monitor
            // Windows 10 1703+: ask for 8-bit BGRA, which also reads an HDR screen (converted).
            IDXGIOutput5* output5 = nullptr;
            if (SUCCEEDED(hr) && SUCCEEDED(output->QueryInterface(IID_PPV_ARGS(&output5)))) {
                const DXGI_FORMAT formats[] = {DXGI_FORMAT_B8G8R8A8_UNORM};
                if (FAILED(output5->DuplicateOutput1(device, 0, 1, formats, &dup))) dup = nullptr;
                Release(output5);
            }
            if (SUCCEEDED(hr) && !dup) hr = output->QueryInterface(IID_PPV_ARGS(&output1));
            if (SUCCEEDED(hr) && !dup) hr = output1->DuplicateOutput(device, &dup);
            Release(output1);
            Release(output);
            Release(adapter);
            Release(dxgiDevice);
            if (FAILED(hr)) {
                if (failures++ == 0) {
                    char msg[160];
                    snprintf(msg, sizeof msg, "Windows won't let LumaBridge read the screen (error 0x%08lX).",
                             static_cast<unsigned long>(hr));
                    setProblem(msg);
                }
                for (int i = 0; i < 20 && !stop_; ++i) Sleep(100);
                continue;
            }
            failures = 0;
        }

        DXGI_OUTDUPL_FRAME_INFO info{};
        IDXGIResource* resource = nullptr;
        HRESULT hr = dup->AcquireNextFrame(kFrameMs, &info, &resource);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) continue;  // nothing changed on screen
        if (FAILED(hr)) {                             // mode change, UAC prompt, ...
            resetDup();
            Sleep(200);
            continue;
        }
        ID3D11Texture2D* frame = nullptr;
        resource->QueryInterface(IID_PPV_ARGS(&frame));
        Release(resource);
        if (frame) {
            D3D11_TEXTURE2D_DESC d;
            frame->GetDesc(&d);
            if (d.Width != texW || d.Height != texH) {
                Release(staging);
                Release(mipSrv);
                Release(mipTex);
                texW = d.Width;
                texH = d.Height;
                mipLevel = 0;
                while ((texW >> mipLevel) > kMaxSampleWidth) ++mipLevel;
                sampleW = texW >> mipLevel ? texW >> mipLevel : 1;
                sampleH = texH >> mipLevel ? texH >> mipLevel : 1;
                D3D11_TEXTURE2D_DESC m = d;
                m.MipLevels = mipLevel + 1;
                m.ArraySize = 1;
                m.SampleDesc = {1, 0};
                m.Usage = D3D11_USAGE_DEFAULT;
                m.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
                m.CPUAccessFlags = 0;
                m.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
                D3D11_TEXTURE2D_DESC st = d;
                st.Width = sampleW;
                st.Height = sampleH;
                st.MipLevels = 1;
                st.ArraySize = 1;
                st.SampleDesc = {1, 0};
                st.Usage = D3D11_USAGE_STAGING;
                st.BindFlags = 0;
                st.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                st.MiscFlags = 0;
                if (FAILED(device->CreateTexture2D(&m, nullptr, &mipTex)) ||
                    FAILED(device->CreateShaderResourceView(mipTex, nullptr, &mipSrv)) ||
                    FAILED(device->CreateTexture2D(&st, nullptr, &staging))) {
                    LUMA_WARN("screen colors: unsupported screen format %d", static_cast<int>(d.Format));
                    resetDup();
                    frame->Release();
                    dup = nullptr;
                    for (int i = 0; i < 50 && !stop_; ++i) Sleep(100);
                    continue;
                }
            }
            if (mipTex && staging) {
                context->CopySubresourceRegion(mipTex, 0, 0, 0, 0, frame, 0, nullptr);
                context->GenerateMips(mipSrv);
                context->CopySubresourceRegion(staging, 0, 0, 0, 0, mipTex, mipLevel, nullptr);
                D3D11_MAPPED_SUBRESOURCE map;
                if (SUCCEEDED(context->Map(staging, 0, D3D11_MAP_READ, 0, &map))) {
                    pixels.resize(static_cast<size_t>(sampleW) * sampleH * 4);
                    for (UINT y = 0; y < sampleH; ++y)
                        memcpy(pixels.data() + static_cast<size_t>(y) * sampleW * 4,
                               static_cast<const uint8_t*>(map.pData) + static_cast<size_t>(y) * map.RowPitch,
                               static_cast<size_t>(sampleW) * 4);
                    context->Unmap(staging, 0);
                    const games::ScreenColors c =
                        games::SummarizeScreen(pixels, static_cast<int>(sampleW), static_cast<int>(sampleH));
                    if (!loggedFrame) {
                        LUMA_INFO("screen colors: reading the screen (%ux%u, format %d)", texW, texH,
                                  static_cast<int>(d.Format));
                        loggedFrame = true;
                    }
                    // A screen that stays black: the game hides itself from capture.
                    const uint64_t now = GetTickCount64();
                    auto dark = [](Rgb x) { return x.r < 16 && x.g < 16 && x.b < 16; };
                    if (dark(c.left) && dark(c.right)) {
                        if (!blackSince) blackSince = now;
                        if (now - blackSince > 3000)
                            setProblem("The screen reads as black: the game hides itself from screen capture, or "
                                       "runs in exclusive fullscreen. Try Borderless in the game's display settings.");
                    } else {
                        blackSince = 0;
                        setProblem("");
                    }
                    std::lock_guard<std::mutex> lock(mutex_);
                    latest_ = c;
                    have_ = true;
                }
            }
            frame->Release();
        }
        dup->ReleaseFrame();
        Sleep(kFrameMs);
    }
    resetDup();
    Release(context);
    Release(device);
    LUMA_INFO("screen colors: stopped");
}

}  // namespace luma::app
