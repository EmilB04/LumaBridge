// Only ASUS's installed RAM lighting workers may be stopped for native RAM control.
#pragma once

#include <cwctype>
#include <string>

namespace luma::app::asus {

inline bool IsRamLightingWorker(const std::wstring& path, const std::wstring& programFiles,
                                const std::wstring& programFilesX86) {
    auto lower = [](std::wstring text) {
        for (auto& c : text) c = static_cast<wchar_t>(std::towlower(c));
        return text;
    };
    const auto candidate = lower(path);
    for (const auto& root : {programFiles, programFilesX86}) {
        if (root.empty()) continue;
        const auto base = lower(root) + L"\\asus\\";
        if (candidate == base + L"kingston_aac_dram\\aackingstondramhal_x86.exe" ||
            candidate == base + L"kingston_aac_dram\\aackingstondramhal_x64.exe" ||
            candidate == base + L"asus_aac_dram\\aac3572dramhal_x86.exe") return true;
    }
    return false;
}

}  // namespace luma::app::asus
