// SPDX-License-Identifier: GPL-3.0-or-later
// HCI opcodes the stack sends, and the event codes it handles.
#pragma once

#include <cstdint>

namespace bluev::hci
{

constexpr std::uint16_t kInquiry = 0x0401;
constexpr std::uint16_t kInquiryCancel = 0x0402;
constexpr std::uint16_t kCreateConn = 0x0405;
constexpr std::uint16_t kDisconnect = 0x0406;
constexpr std::uint16_t kCreateConnCancel = 0x0408;
constexpr std::uint16_t kAcceptConnReq = 0x0409;
constexpr std::uint16_t kLinkKeyReply = 0x040b;
constexpr std::uint16_t kLinkKeyNegReply = 0x040c;
constexpr std::uint16_t kPinCodeReply = 0x040d;
constexpr std::uint16_t kAuthRequested = 0x0411;
constexpr std::uint16_t kSetConnEncryption = 0x0413;
constexpr std::uint16_t kIoCapabilityReply = 0x042b;
constexpr std::uint16_t kUserConfirmReply = 0x042c;
constexpr std::uint16_t kRoleDiscovery = 0x0809;
constexpr std::uint16_t kSetEventMask = 0x0c01;
constexpr std::uint16_t kReset = 0x0c03;
constexpr std::uint16_t kSetEventFilter = 0x0c05;
constexpr std::uint16_t kWriteLocalName = 0x0c13;
constexpr std::uint16_t kReadPageTimeout = 0x0c17;
constexpr std::uint16_t kReadScanEnable = 0x0c19;
constexpr std::uint16_t kWriteScanEnable = 0x0c1a;
constexpr std::uint16_t kWriteClassOfDevice = 0x0c24;
constexpr std::uint16_t kWriteInquiryMode = 0x0c45;
constexpr std::uint16_t kWriteSimplePairing = 0x0c56;
constexpr std::uint16_t kReadLocalVersion = 0x1001;
constexpr std::uint16_t kReadLocalFeatures = 0x1003;
constexpr std::uint16_t kReadBufferSize = 0x1005;
constexpr std::uint16_t kReadBdAddr = 0x1009;

} // namespace bluev::hci
