#pragma once

#include <string>

namespace luma::app {

std::wstring AzothOledAssetDirectory();
bool ExportAzothOledBanner(const std::wstring& text, int fontSize, bool invert, std::string& error);

}  // namespace luma::app
