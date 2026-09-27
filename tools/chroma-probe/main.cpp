// Razer Chroma Broadcast probe: can LumaBridge receive the Chroma lighting games send to Razer
// Synapse (the way Philips Hue and Nanoleaf do, through Razer's Chroma Broadcast API)? Nothing
// here touches a game: it only talks to Razer's own software.
//
//   chroma-probe [seconds]      default 300
//
// 1. Lists Razer's processes and DLLs (RzChromaBroadcastAPI*.dll, RzChromaSDK*.dll) and asks
//    the Chroma SDK's local REST service (localhost:54235) for its version.
// 2. Loads the Broadcast API, lists its exports, initializes it and registers for its events.
// 3. Self-test: sends a few colors itself through the real Chroma SDK (red, green, blue, as a
//    game would) and checks whether they come back through the broadcast.
// 4. Listens for the rest of the time: start the game now. Every broadcast is logged (raw
//    bytes and the five colors).
// Output: the console and %LOCALAPPDATA%\LumaBridge\chroma-probe.txt.
//
// The Broadcast API as Razer's sample uses it (checked against the exports at run time):
//   RZRESULT Init(GUID appId); RZRESULT UnInit();
//   RZRESULT RegisterEventNotification(RZRESULT (*cb)(int type, int status, void* data));
//   RZRESULT UnRegisterEventNotification();
//   type 1 = effect (data: 5 COLORREFs, then more), type 2 = status (1 live, 2 not live).
#include <windows.h>
#include <tlhelp32.h>
#include <winhttp.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "chroma_types.h"

namespace {

FILE* g_out = nullptr;
std::mutex g_logMutex;
ULONGLONG g_start = 0;

void Log(const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    char buf[2048];
    vsnprintf(buf, sizeof buf, fmt, a);
    va_end(a);
    std::lock_guard<std::mutex> lock(g_logMutex);
    printf("%s\n", buf);
    if (g_out) {
        fprintf(g_out, "%s\n", buf);
        fflush(g_out);
    }
}

std::string Utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::string Hex(const uint8_t* p, size_t n) {
    std::string s;
    char b[4];
    for (size_t i = 0; i < n; ++i) {
        snprintf(b, sizeof b, "%02X ", p[i]);
        s += b;
    }
    if (!s.empty()) s.pop_back();
    return s;
}

bool ContainsI(const std::wstring& s, const wchar_t* part) {
    std::wstring a = s, b = part;
    for (auto& c : a) c = towlower(c);
    for (auto& c : b) c = towlower(c);
    return a.find(b) != std::wstring::npos;
}

std::string FileVersion(const std::wstring& path) {
    DWORD h = 0;
    const DWORD n = GetFileVersionInfoSizeW(path.c_str(), &h);
    if (!n) return "?";
    std::vector<BYTE> buf(n);
    VS_FIXEDFILEINFO* fi = nullptr;
    UINT len = 0;
    if (!GetFileVersionInfoW(path.c_str(), 0, n, buf.data()) ||
        !VerQueryValueW(buf.data(), L"\\", reinterpret_cast<void**>(&fi), &len) || !fi)
        return "?";
    char v[64];
    snprintf(v, sizeof v, "%u.%u.%u.%u", HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS),
             HIWORD(fi->dwFileVersionLS), LOWORD(fi->dwFileVersionLS));
    return v;
}

// Razer's processes (Synapse, the Chroma SDK service, Chroma Connect...).
void ListProcesses() {
    Log("Razer processes:");
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    int n = 0;
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe{};
        pe.dwSize = sizeof pe;
        for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
            const std::wstring name = pe.szExeFile;
            if (ContainsI(name, L"razer") || ContainsI(name, L"synapse") || ContainsI(name, L"chroma") ||
                ContainsI(name, L"rzsdk") || ContainsI(name, L"rzchroma")) {
                Log("  %s (pid %lu)", Utf8(name).c_str(), pe.th32ProcessID);
                ++n;
            }
        }
        CloseHandle(snap);
    }
    if (!n) Log("  none - is Razer Synapse installed and running?");
}

// RzChroma*.dll under `dir` (a few levels deep).
void FindDlls(const std::wstring& dir, int depth, std::vector<std::wstring>* out) {
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return;
    do {
        const std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        const std::wstring path = dir + L"\\" + name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (depth > 0) FindDlls(path, depth - 1, out);
        } else if (ContainsI(name, L"rzchroma") && ContainsI(name, L".dll")) {
            out->push_back(path);
        }
    } while (FindNextFileW(f, &fd));
    FindClose(f);
}

std::vector<std::wstring> ListDlls() {
    std::vector<std::wstring> dlls;
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    FindDlls(sys, 0, &dlls);
    for (const wchar_t* env : {L"ProgramFiles", L"ProgramFiles(x86)", L"ProgramW6432"}) {
        wchar_t pf[MAX_PATH];
        if (!GetEnvironmentVariableW(env, pf, MAX_PATH)) continue;
        WIN32_FIND_DATAW fd;
        HANDLE f = FindFirstFileW((std::wstring(pf) + L"\\Razer*").c_str(), &fd);
        if (f == INVALID_HANDLE_VALUE) continue;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) FindDlls(std::wstring(pf) + L"\\" + fd.cFileName, 4, &dlls);
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    // ProgramFiles and ProgramW6432 are the same folder for a 64-bit program: drop repeats.
    std::vector<std::wstring> unique;
    for (const auto& d : dlls) {
        bool seen = false;
        for (const auto& u : unique) seen = seen || _wcsicmp(u.c_str(), d.c_str()) == 0;
        if (!seen) unique.push_back(d);
    }
    Log("Razer Chroma DLLs:");
    for (const auto& d : unique) Log("  %s (version %s)", Utf8(d).c_str(), FileVersion(d).c_str());
    if (unique.empty()) Log("  none found");
    return unique;
}

// GET http://localhost:54235/razer/chromasdk: the Chroma SDK's REST service answers with its
// version when it runs.
void AskRestService() {
    Log("Chroma SDK REST service (localhost:54235):");
    HINTERNET s = WinHttpOpen(L"LumaBridge chroma-probe", WINHTTP_ACCESS_TYPE_NO_PROXY, nullptr, nullptr, 0);
    HINTERNET c = s ? WinHttpConnect(s, L"localhost", 54235, 0) : nullptr;
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", L"/razer/chromasdk", nullptr, nullptr, nullptr, 0) : nullptr;
    std::string body;
    if (r) WinHttpSetTimeouts(r, 2000, 2000, 2000, 2000);
    if (r && WinHttpSendRequest(r, nullptr, 0, nullptr, 0, 0, 0) && WinHttpReceiveResponse(r, nullptr)) {
        DWORD n = 0;
        while (WinHttpQueryDataAvailable(r, &n) && n) {
            std::string part(n, '\0');
            DWORD got = 0;
            if (!WinHttpReadData(r, part.data(), n, &got) || !got) break;
            body.append(part.data(), got);
        }
        Log("  %s", body.empty() ? "(empty answer)" : body.c_str());
    } else {
        Log("  no answer (error %lu) - the Chroma SDK service isn't running", GetLastError());
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    if (s) WinHttpCloseHandle(s);
}

// The names a loaded DLL exports.
std::vector<std::string> Exports(HMODULE m) {
    std::vector<std::string> out;
    auto* base = reinterpret_cast<const uint8_t*>(m);
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dir.VirtualAddress) return out;
    auto* ex = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + dir.VirtualAddress);
    auto* names = reinterpret_cast<const DWORD*>(base + ex->AddressOfNames);
    for (DWORD i = 0; i < ex->NumberOfNames; ++i) out.emplace_back(reinterpret_cast<const char*>(base + names[i]));
    return out;
}

// --- Broadcast events -------------------------------------------------------------------
std::mutex g_eventMutex;
int g_effects = 0, g_statuses = 0;
std::string g_lastEffect;

long BroadcastEvent(int type, int status, void* data) {
    const double t = (GetTickCount64() - g_start) / 1000.0;
    if (type == 1 && data) {
        const auto* p = static_cast<const uint8_t*>(data);
        const auto* c = static_cast<const COLORREF*>(data);
        std::string colors;
        for (int i = 0; i < 5; ++i) {
            char b[16];
            snprintf(b, sizeof b, "%s#%02X%02X%02X", i ? " " : "", GetRValue(c[i]), GetGValue(c[i]), GetBValue(c[i]));
            colors += b;
        }
        const std::string raw = Hex(p, 32);
        std::lock_guard<std::mutex> lock(g_eventMutex);
        ++g_effects;
        if (raw == g_lastEffect) return 0;  // the same again: logged once
        g_lastEffect = raw;
        Log("%8.3f  EFFECT  colors %s   raw %s", t, colors.c_str(), raw.c_str());
    } else {
        {
            std::lock_guard<std::mutex> lock(g_eventMutex);
            if (type == 2) ++g_statuses;
        }
        Log("%8.3f  event type %d, status %d%s", t, type, status,
            type == 2 ? (status == 1 ? " (broadcast live)" : status == 2 ? " (broadcast not live)" : "") : "");
    }
    return 0;
}

using InitFn = long (*)(GUID);
using UnInitFn = long (*)();
using CallbackFn = long (*)(int, int, void*);
using RegisterFn = long (*)(CallbackFn);

template <typename F>
F Proc(HMODULE m, const char* name) {
    return reinterpret_cast<F>(reinterpret_cast<void*>(GetProcAddress(m, name)));
}

// --- Self-test: colors sent through the real Chroma SDK, as a game would ------------------
void SelfTest(const std::vector<std::wstring>& dlls) {
    std::wstring sdkPath;
    for (const auto& d : dlls) {
        const std::wstring name = d.substr(d.find_last_of(L'\\') + 1);
        if (_wcsicmp(name.c_str(), L"RzChromaSDK64.dll") == 0) {
            sdkPath = d;
            break;
        }
    }
    Log("");
    if (sdkPath.empty()) {
        Log("Self-test skipped: RzChromaSDK64.dll (the SDK games use) wasn't found.");
        return;
    }
    Log("Self-test: sending red, green, blue, white through %s", Utf8(sdkPath).c_str());
    HMODULE sdk = LoadLibraryExW(sdkPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!sdk) {
        Log("  can't load it (error %lu)", GetLastError());
        return;
    }
    using SdkInit = long (*)();
    using KbEffect = long (*)(int, void*, GUID*);
    auto init = Proc<SdkInit>(sdk, "Init");
    auto uninit = Proc<SdkInit>(sdk, "UnInit");
    auto kb = Proc<KbEffect>(sdk, "CreateKeyboardEffect");
    if (!init || !uninit || !kb) {
        Log("  it lacks Init / UnInit / CreateKeyboardEffect");
        FreeLibrary(sdk);
        return;
    }
    const long r = init();
    Log("  Init: %ld%s", r, r == 0 ? " (ok)" : "");
    if (r == 0) {
        Sleep(1500);  // Synapse takes a moment to accept a new app
        const COLORREF colors[] = {RGB(255, 0, 0), RGB(0, 255, 0), RGB(0, 0, 255), RGB(255, 255, 255)};
        const char* names[] = {"red", "green", "blue", "white"};
        for (int i = 0; i < 4; ++i) {
            int before;
            {
                std::lock_guard<std::mutex> lock(g_eventMutex);
                before = g_effects;
            }
            COLORREF c = colors[i];
            const long e = kb(luma::chroma::keyboard::STATIC, &c, nullptr);
            Sleep(2500);
            int after;
            {
                std::lock_guard<std::mutex> lock(g_eventMutex);
                after = g_effects;
            }
            Log("  %s: CreateKeyboardEffect %ld, broadcasts since: %d", names[i], e, after - before);
        }
        uninit();
    }
    FreeLibrary(sdk);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    const int seconds = argc > 1 && _wtoi(argv[1]) > 0 ? _wtoi(argv[1]) : 300;
    wchar_t appData[MAX_PATH];
    std::wstring outPath = L"chroma-probe.txt";
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", appData, MAX_PATH)) {
        std::wstring dir = std::wstring(appData) + L"\\LumaBridge";
        CreateDirectoryW(dir.c_str(), nullptr);
        outPath = dir + L"\\chroma-probe.txt";
    }
    g_out = _wfopen(outPath.c_str(), L"w");
    g_start = GetTickCount64();
    Log("LumaBridge Razer Chroma Broadcast probe");
    Log("");
    ListProcesses();
    Log("");
    const auto dlls = ListDlls();
    Log("");
    AskRestService();
    Log("");

    std::wstring bcPath;
    for (const auto& d : dlls) {
        const std::wstring name = d.substr(d.find_last_of(L'\\') + 1);
        if (ContainsI(name, L"broadcast") && ContainsI(name, L"64")) bcPath = d;
    }
    if (bcPath.empty()) {
        Log("No 64-bit Chroma Broadcast DLL (RzChromaBroadcastAPI64.dll) found. In Razer Synapse, install the");
        Log("Chroma Connect module (Modules / Add-ons), then run this again.");
        if (g_out) fclose(g_out);
        return 1;
    }
    HMODULE bc = LoadLibraryExW(bcPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!bc) {
        Log("Can't load %s (error %lu)", Utf8(bcPath).c_str(), GetLastError());
        if (g_out) fclose(g_out);
        return 1;
    }
    Log("Broadcast API: %s", Utf8(bcPath).c_str());
    std::string names;
    for (const auto& e : Exports(bc)) names += (names.empty() ? "" : ", ") + e;
    Log("  exports: %s", names.c_str());
    auto init = Proc<InitFn>(bc, "Init");
    auto uninit = Proc<UnInitFn>(bc, "UnInit");
    auto reg = Proc<RegisterFn>(bc, "RegisterEventNotification");
    auto unreg = Proc<UnInitFn>(bc, "UnRegisterEventNotification");
    if (!init || !reg) {
        Log("  Init / RegisterEventNotification missing - send me this file, the API differs.");
        if (g_out) fclose(g_out);
        return 1;
    }
    // LumaBridge's own app ID (made up). Razer may only accept IDs it handed out; the result
    // says so.
    const GUID appId = {0x6c1f3b52, 0x9a4e, 0x4d2b, {0x8e, 0x51, 0x3a, 0x7c, 0x90, 0x12, 0x4f, 0xb6}};
    const long ri = init(appId);
    Log("  Init: %ld%s", ri, ri == 0 ? " (ok)" : " (refused)");
    if (ri != 0) {
        Log("  Razer refused the app ID. Send me this file.");
        if (g_out) fclose(g_out);
        return 1;
    }
    const long rr = reg(&BroadcastEvent);
    Log("  RegisterEventNotification: %ld%s", rr, rr == 0 ? " (ok)" : "");

    SelfTest(dlls);

    Log("");
    Log("Listening for %d s. Start the game now and play a bit (take damage, get kills, die).", seconds);
    Log("Everything goes to %s", Utf8(outPath).c_str());
    const ULONGLONG until = GetTickCount64() + seconds * 1000ULL;
    ULONGLONG next = GetTickCount64() + 15000;
    while (GetTickCount64() < until) {
        Sleep(250);
        if (GetTickCount64() >= next) {
            std::lock_guard<std::mutex> lock(g_eventMutex);
            printf("  %llu s: %d effect broadcasts, %d status events\n", (GetTickCount64() - g_start) / 1000,
                   g_effects, g_statuses);
            next += 15000;
        }
    }
    if (unreg) unreg();
    if (uninit) uninit();
    Log("");
    Log("Done: %d effect broadcasts, %d status events.", g_effects, g_statuses);
    if (g_out) fclose(g_out);
    return 0;
}
