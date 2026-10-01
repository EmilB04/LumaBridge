// USB receiver presence and keyboard readiness are separate. The receiver answers its
// version query even when the keyboard is off; 12 01 checks the paired keyboard's power.
#pragma once

#include <string>
#include "azoth_protocol.h"

namespace luma::app::azoth {

enum class Connection { Unknown, Disconnected, ReceiverOnly, Wired, Wireless };

inline Connection SelectConnection(bool wiredPresent, bool receiverPresent, bool keyboardReady, bool asleep) {
    if (wiredPresent) return Connection::Wired;
    if (!receiverPresent) return Connection::Disconnected;
    return keyboardReady && !asleep ? Connection::Wireless : Connection::ReceiverOnly;
}

inline bool KeyboardConnected(Connection state) {
    return state == Connection::Wired || state == Connection::Wireless;
}

inline bool ControlPath(Link link, std::wstring path) {
    for (auto& c : path) if (c >= L'A' && c <= L'Z') c += L'a' - L'A';
    return path.find(link == Link::Wired ? L"&mi_01#" : L"&mi_02&col02#") != std::wstring::npos;
}

inline Report StatusQuery(Link link, uint8_t subcommand) {
    Report r{};
    r[0] = link == Link::Wired ? 0 : 2;
    r[1] = 0x12;
    r[2] = subcommand;
    return r;
}

inline bool VersionReply(Link link, const Report& reply, size_t size) {
    return size >= 6 && reply[0] == (link == Link::Wired ? 0 : 2) &&
           reply[1] == 0x12 && reply[2] == 0 && reply[5] != 0xFA;
}

inline bool PowerReply(Link link, const Report& reply, size_t size) {
    return size >= 10 && reply[0] == (link == Link::Wired ? 0 : 2) &&
           reply[1] == 0x12 && reply[2] == 1 && reply[5] != 0xFA && reply[6] <= 100;
}

}  // namespace luma::app::azoth
