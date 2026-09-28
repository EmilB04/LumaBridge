#include "ghub_settings.h"

#include <windows.h>
#include <winhttp.h>

#include <string>
#include <vector>

#include "device_sleep.h"

namespace luma::app {

std::optional<bool> GHubTurnsOffOnInactivity() {
    std::optional<bool> result;
    HINTERNET session = WinHttpOpen(L"LumaBridge", WINHTTP_ACCESS_TYPE_NO_PROXY, nullptr, nullptr, 0);
    if (!session) return result;
    WinHttpSetTimeouts(session, 1000, 1000, 1000, 1000);
    HINTERNET connect = WinHttpConnect(session, L"127.0.0.1", 9010, 0);
    HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", L"/", nullptr, nullptr, nullptr, 0) : nullptr;
    HINTERNET ws = nullptr;
    if (request && WinHttpSetOption(request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0) &&
        WinHttpSendRequest(request, L"Origin: file://\r\nSec-WebSocket-Protocol: json\r\n", static_cast<DWORD>(-1), nullptr,
                           0, 0, 0) &&
        WinHttpReceiveResponse(request, nullptr))
        ws = WinHttpWebSocketCompleteUpgrade(request, 0);
    if (ws) {
        std::string get = R"({"msgId":"lumabridge-sleep","verb":"GET","path":"/lighting/turn_off_for_inactivity"})";
        if (WinHttpWebSocketSend(ws, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, get.data(), static_cast<DWORD>(get.size())) ==
            NO_ERROR) {
            // G HUB may send other messages first: read a few, up to ~3 s.
            const ULONGLONG until = GetTickCount64() + 3000;
            std::vector<char> buf(64 * 1024);
            std::string message;
            while (!result && GetTickCount64() < until) {
                DWORD got = 0;
                WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
                if (WinHttpWebSocketReceive(ws, buf.data(), static_cast<DWORD>(buf.size()), &got, &type) != NO_ERROR ||
                    type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE)
                    break;
                message.append(buf.data(), got);
                if (type == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE) continue;
                if (message.find("lumabridge-sleep") != std::string::npos) {
                    result = sleep::ParseGHubEnabled(message);
                    break;
                }
                message.clear();
            }
        }
        WinHttpWebSocketClose(ws, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
        WinHttpCloseHandle(ws);
    }
    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return result;
}

}  // namespace luma::app
