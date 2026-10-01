#include "azoth_oled_assets.h"

#include <windows.h>

#include "azoth_oled.h"
#include "config.h"

namespace luma::app {

std::wstring AzothOledAssetDirectory() {
    const auto base = LocalAppDataDir();
    return base.empty() ? L"" : base + L"\\AzothOLED";
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
