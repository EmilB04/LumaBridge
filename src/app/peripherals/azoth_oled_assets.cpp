#include "azoth_oled_assets.h"

#include <windows.h>
#include <wincodec.h>
#include <shlobj.h>
#include <memory>

#include "azoth_oled.h"
#include "azoth_oled_effects.h"
#include "config.h"

namespace luma::app {

std::wstring AzothOledAssetDirectory() {
    const auto base = LocalAppDataDir();
    return base.empty() ? L"" : base + L"\\AzothOLED";
}

bool ExportAzothOledEffect(int index, std::wstring& path, std::string& error) {
    error.clear(); path.clear();
    const auto dir = AzothOledAssetDirectory();
    const auto gif = azoth::OledEffectGif(index);
    if (dir.empty() || gif.empty()) { error = "Couldn't prepare this animation."; return false; }
    CreateDirectoryW(LocalAppDataDir().c_str(), nullptr);
    if (!CreateDirectoryW(dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        error = "Couldn't create the upload folder (error " + std::to_string(GetLastError()) + ").";
        return false;
    }
    const auto target = dir + L"\\" + azoth::kLumaAnimationFiles[index];
    HANDLE file = CreateFileW(target.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = "Couldn't save the animation (error " + std::to_string(GetLastError()) + ").";
        return false;
    }
    DWORD written = 0;
    const bool ok = WriteFile(file, gif.data(), static_cast<DWORD>(gif.size()), &written, nullptr) && written == gif.size();
    const DWORD writeError = ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(file);
    if (!ok) error = "Couldn't finish saving the animation (error " + std::to_string(writeError) + ").";
    else path = target;
    return ok;
}

namespace {
template<class T> struct ReleaseCom { void operator()(T* p) const { if (p) p->Release(); } };
template<class T> using ComPtr = std::unique_ptr<T, ReleaseCom<T>>;
struct PreviewFrame { std::vector<uint8_t> gray; double end = 0; };
struct PreviewAnimation { bool loaded = false; std::vector<PreviewFrame> frames; };

std::wstring AsusPreviewPath(int index) {
    PWSTR programFiles = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramFilesX86, 0, nullptr, &programFiles))) return {};
    const std::wstring base = std::wstring(programFiles) + L"\\ASUS\\ArmouryDevice\\View\\";
    CoTaskMemFree(programFiles);
    // Original-Azoth regional modules share this preset order and artwork.
    for (const wchar_t* module : {L"6789", L"6787", L"6791"}) {
        const std::wstring dir = base + module + L"\\resources\\";
        const auto pattern = dir + azoth::kAsusAnimationFiles[index] + L"-*.gif";
        WIN32_FIND_DATAW data{};
        HANDLE search = FindFirstFileW(pattern.c_str(), &data);
        if (search != INVALID_HANDLE_VALUE) {
            FindClose(search);
            if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return dir + data.cFileName;
        }
    }
    return {};
}

unsigned GifMetadata(IWICMetadataQueryReader* reader, const wchar_t* name, unsigned fallback) {
    if (!reader) return fallback;
    PROPVARIANT value{};
    unsigned result = fallback;
    if (SUCCEEDED(reader->GetMetadataByName(name, &value))) {
        if (value.vt == VT_UI1) result = value.bVal;
        else if (value.vt == VT_UI2) result = value.uiVal;
        else if (value.vt == VT_UI4) result = value.ulVal;
    }
    PropVariantClear(&value);
    return result;
}

std::vector<PreviewFrame> LoadAsusPreview(int index) {
    const auto path = AsusPreviewPath(index);
    if (path.empty()) return {};
    IWICImagingFactory* rawFactory = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                               IID_IWICImagingFactory, reinterpret_cast<void**>(&rawFactory)))) return {};
    ComPtr<IWICImagingFactory> factory(rawFactory);
    IWICBitmapDecoder* rawDecoder = nullptr;
    if (FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &rawDecoder))) return {};
    ComPtr<IWICBitmapDecoder> decoder(rawDecoder);
    UINT count = 0;
    if (FAILED(decoder->GetFrameCount(&count)) || !count || count > 500) return {};
    IWICMetadataQueryReader* rawReader = nullptr;
    decoder->GetMetadataQueryReader(&rawReader);
    ComPtr<IWICMetadataQueryReader> global(rawReader);
    const unsigned width = GifMetadata(global.get(), L"/logscrdesc/Width", 208);
    const unsigned height = GifMetadata(global.get(), L"/logscrdesc/Height", 64);
    if (!width || width > azoth::kOledWidth || !height || height > azoth::kOledHeight) return {};
    std::vector<uint8_t> canvas(azoth::kOledWidth * azoth::kOledHeight, 0);
    std::vector<PreviewFrame> frames;
    double end = 0;
    for (UINT i = 0; i < count; ++i) {
        IWICBitmapFrameDecode* rawFrame = nullptr;
        if (FAILED(decoder->GetFrame(i, &rawFrame))) return {};
        ComPtr<IWICBitmapFrameDecode> frame(rawFrame);
        rawReader = nullptr;
        frame->GetMetadataQueryReader(&rawReader);
        ComPtr<IWICMetadataQueryReader> metadata(rawReader);
        const unsigned left = GifMetadata(metadata.get(), L"/imgdesc/Left", 0);
        const unsigned top = GifMetadata(metadata.get(), L"/imgdesc/Top", 0);
        const unsigned delay = GifMetadata(metadata.get(), L"/grctlext/Delay", 5);
        const unsigned disposal = GifMetadata(metadata.get(), L"/grctlext/Disposal", 1);
        UINT fw = 0, fh = 0;
        if (FAILED(frame->GetSize(&fw, &fh)) || !fw || !fh || fw > width || fh > height ||
            left + fw > width || top + fh > height) return {};
        IWICFormatConverter* rawConverter = nullptr;
        if (FAILED(factory->CreateFormatConverter(&rawConverter))) return {};
        ComPtr<IWICFormatConverter> converter(rawConverter);
        if (FAILED(converter->Initialize(frame.get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone,
                                         nullptr, 0, WICBitmapPaletteTypeCustom))) return {};
        std::vector<uint8_t> bgra(fw * fh * 4);
        if (FAILED(converter->CopyPixels(nullptr, fw * 4, static_cast<UINT>(bgra.size()), bgra.data()))) return {};
        const auto before = disposal == 3 ? canvas : std::vector<uint8_t>{};
        const unsigned margin = (azoth::kOledWidth - width) / 2;
        for (UINT y = 0; y < fh; ++y)
            for (UINT x = 0; x < fw; ++x) {
                const size_t at = (y * fw + x) * 4;
                if (bgra[at + 3]) canvas[(top + y) * azoth::kOledWidth + margin + left + x] =
                    static_cast<uint8_t>((bgra[at] * 11 + bgra[at + 1] * 59 + bgra[at + 2] * 30) / 100);
            }
        end += std::max(delay, 2u) / 100.0;
        frames.push_back({canvas, end});
        if (disposal == 2)
            for (UINT y = 0; y < fh; ++y)
                std::fill_n(canvas.begin() + (top + y) * azoth::kOledWidth + margin + left, fw, 0);
        else if (disposal == 3) canvas = before;
    }
    return frames;
}
} // namespace

const std::vector<uint8_t>& AsusAzothOledPreview(int index, double seconds) {
    static std::array<PreviewAnimation, 6> animations;
    static const std::vector<uint8_t> empty;
    if (index < 0 || index >= static_cast<int>(animations.size()) || !std::isfinite(seconds)) return empty;
    auto& animation = animations[index];
    if (!animation.loaded) { animation.frames = LoadAsusPreview(index); animation.loaded = true; }
    if (animation.frames.empty()) return empty;
    const double length = animation.frames.back().end;
    double time = std::fmod(seconds, length);
    if (time < 0) time += length;
    const auto frame = std::upper_bound(animation.frames.begin(), animation.frames.end(), time,
        [](double t, const PreviewFrame& f) { return t < f.end; });
    return (frame == animation.frames.end() ? animation.frames.back() : *frame).gray;
}

bool ExportAzothOledBanner(const std::wstring& text, int fontSize, bool invert, std::string& error) {
    error.clear();
    const auto dir = AzothOledAssetDirectory();
    if (dir.empty() || text.empty()) { error = "Enter some text before exporting."; return false; }
    CreateDirectoryW(LocalAppDataDir().c_str(), nullptr);
    if (!CreateDirectoryW(dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        error = "Couldn't create the upload folder (error " + std::to_string(GetLastError()) + ").";
        return false;
    }
    HDC dc = CreateCompatibleDC(nullptr);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = azoth::kOledWidth;
    info.bmiHeader.biHeight = -azoth::kOledHeight;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    HBITMAP image = dc ? CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0) : nullptr;
    if (!image) {
        if (dc) DeleteDC(dc);
        error = "Couldn't create the banner image.";
        return false;
    }
    HGDIOBJ oldImage = SelectObject(dc, image);
    RECT bounds{0, 0, azoth::kOledWidth, azoth::kOledHeight};
    HBRUSH background = CreateSolidBrush(invert ? RGB(255, 255, 255) : RGB(0, 0, 0));
    FillRect(dc, &bounds, background);
    DeleteObject(background);
    SetTextColor(dc, invert ? RGB(0, 0, 0) : RGB(255, 255, 255));
    SetBkMode(dc, TRANSPARENT);
    HFONT font = nullptr;
    HGDIOBJ oldFont = nullptr;
    const int count = static_cast<int>(std::min<size_t>(text.size(), 255));
    // Fit long banners while keeping the chosen size as the upper limit.
    for (int size = std::clamp(fontSize, 8, 48); size >= 8; --size) {
        font = CreateFontW(-size, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        if (!font) break;
        const auto previous = SelectObject(dc, font);
        if (!oldFont) oldFont = previous;
        SIZE extent{};
        GetTextExtentPoint32W(dc, text.c_str(), count, &extent);
        if (extent.cx <= azoth::kOledWidth - 8 || size == 8) break;
        SelectObject(dc, oldFont);
        DeleteObject(font);
        font = nullptr;
    }
    if (font) DrawTextW(dc, text.c_str(), count, &bounds, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    GdiFlush();
    std::vector<uint8_t> gray(azoth::kOledWidth * azoth::kOledHeight);
    const auto* source = static_cast<const uint8_t*>(pixels);
    for (size_t i = 0; i < gray.size(); ++i) gray[i] = source[i * 4];
    if (oldFont) SelectObject(dc, oldFont);
    if (font) DeleteObject(font);
    SelectObject(dc, oldImage);
    DeleteObject(image);
    DeleteDC(dc);
    if (!font) { error = "Couldn't render the banner font."; return false; }

    const auto bmp = azoth::OledBitmap(gray);
    const auto path = dir + L"\\banner.bmp";
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = "Couldn't save banner.bmp (error " + std::to_string(GetLastError()) + ").";
        return false;
    }
    DWORD written = 0;
    const bool ok = WriteFile(file, bmp.data(), static_cast<DWORD>(bmp.size()), &written, nullptr) && written == bmp.size();
    const DWORD writeError = ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(file);
    if (!ok) error = "Couldn't finish saving banner.bmp (error " + std::to_string(writeError) + ").";
    return ok;
}

}  // namespace luma::app
