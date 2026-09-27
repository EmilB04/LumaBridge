// Games installed on this PC, for the Games List page: Steam and Epic manifests, the
// Windows "installed apps" list (EA, Ubisoft, GOG, Riot, Rockstar, Battle.net, ...), Xbox
// game folders, the games Windows' Game Bar knows, and exes the user added. Each game's
// folder is also checked (file names only) for the lighting SDK files games ship.
// Slow (disk access): run it on a background thread.
#pragma once

#include <string>
#include <vector>

namespace luma::app {

struct InstalledGame {
    std::wstring name;
    std::wstring store;    // "Steam", "Epic Games", ..., "Added by you"
    std::wstring dir;      // install folder
    std::wstring exePath;  // main exe when known (Epic, Game Bar, added by you)
    bool manual = false;   // added by the user (can be removed from the list)
    std::string sdk;       // lighting SDK whose files are in its folder ("Corsair iCUE"), if any
    std::vector<std::string> exeNames;  // lower-case exe names in its folder, to match games seen running
};

std::vector<InstalledGame> ScanInstalledGames(const std::vector<std::wstring>& manualExes);

}  // namespace luma::app
