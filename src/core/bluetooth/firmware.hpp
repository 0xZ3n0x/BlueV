// SPDX-License-Identifier: GPL-3.0-or-later
// Which controller family an adapter belongs to, and whether BlueV can bring
// that family's firmware up. The upload itself is per family; only Realtek is
// implemented, but the seam keeps a Broadcom/Intel/MediaTek loader out of the
// Bluetooth stack.
#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace bluev
{

class IFirmwareStore;

enum class FirmwareFamily
{
    Unknown,
    Csr,
    Realtek,
    Broadcom,
    Intel,
    MediaTek
};

// What Read Local Version says about the controller. This is the whole input
// for identification; a family that names its firmware per USB id can add the
// ids here when it gets a loader.
struct ChipIdentity
{
    std::uint16_t manufacturer = 0;
    std::uint8_t hciVersion = 0;
    std::uint16_t hciRevision = 0;
    std::uint8_t lmpVersion = 0;
    std::uint16_t lmpSubversion = 0;
};

// Reads the identity from the whole Command Complete event of Read Local
// Version, as the stack keeps it. False when it is not that reply.
bool chipIdentityFromLocalVersion(const std::uint8_t* event, std::uint32_t length, ChipIdentity& out);

FirmwareFamily firmwareFamily(const ChipIdentity& identity);
const char* firmwareFamilyName(FirmwareFamily family);
// True when this build has an uploader for the family.
bool firmwareUploadSupported(FirmwareFamily family);

// The HCI surface an uploader needs: a command whose answer is waited for, one
// that is sent without waiting, time passing, and the last Command Complete
// event. BtStack provides it; tests provide fakes.
struct FirmwareHci
{
    // `attempts` is how many times to send the command when no reply comes.
    std::function<bool(std::uint16_t opcode, const std::uint8_t* params, std::uint8_t length, int attempts)> sync;
    // The last Command Complete event and its length.
    std::function<const std::uint8_t*(std::uint32_t& length)> reply;
    // Sends a command without waiting for an answer (a vendor command whose
    // event may not come at all).
    std::function<bool(std::uint16_t opcode, const std::uint8_t* params, std::uint8_t length)> send;
    // Lets time pass, so events that arrive meanwhile are handled.
    std::function<void(int milliseconds)> pause;
};

struct FirmwareResult
{
    bool ok = true;
    std::string message; // why it failed, or what was uploaded
};

// Runs the uploader for this chip, when there is one. `ok` false means the
// adapter cannot be used; a non-empty message is logged either way. Families
// whose ROM works, or that have no uploader yet, return ok with a note (or
// empty for nothing to say).
FirmwareResult installFirmware(const ChipIdentity& identity, IFirmwareStore& files, FirmwareHci& hci);

} // namespace bluev
