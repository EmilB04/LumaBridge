#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace luma::app {

std::wstring AzothOledAssetDirectory();
bool ExportAzothOledBanner(const std::wstring& text, int fontSize, bool invert, std::string& error);
bool ExportAzothOledEffect(int index, std::wstring& path, std::string& error);
// Empty when Armoury Crate's original ASUS preview assets are unavailable.
const std::vector<uint8_t>& AsusAzothOledPreview(int index, double seconds);

}  // namespace luma::app
