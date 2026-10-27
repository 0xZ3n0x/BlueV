// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/bluetooth/realtek_firmware.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

#include "core/bytes.hpp"

namespace bluev
{

namespace
{

constexpr std::uint16_t kReadRomVersion = 0xfc6d;
constexpr std::uint16_t kDownloadPatch = 0xfc20;
constexpr std::uint16_t kReset = 0x0c03;

constexpr std::size_t kFragment = 252;
constexpr std::size_t kEpatchHeader = 14;   // signature(8) + version(4) + patches(2)
constexpr std::size_t kEpatchHeaderV2 = 20; // signature(8) + version(8) + sections(4)
constexpr std::size_t kV2Tail = 7;          // the extension section after the last section

// v2 section opcodes, from btrtl
constexpr std::uint32_t kSectionSnippets = 1;
constexpr std::uint32_t kSectionDummy = 2;
constexpr std::uint32_t kSectionSecurity = 3;

// The USB chips btrtl knows, with the firmware file for each. The UART variants
// and the 8723A (which uses a different, older download) are left out.
constexpr RealtekChip kChips[] = {
    {0x8723, 0x000b, 0x06, "RTL8723BU", "rtl8723b_fw.bin", "rtl8723b_config.bin", false},
    {0x8723, 0x000d, 0x08, "RTL8723DU", "rtl8723d_fw.bin", "rtl8723d_config.bin", true},
    {0x8821, 0x000a, 0x06, "RTL8821AU", "rtl8821a_fw.bin", "rtl8821a_config.bin", false},
    {0x8821, 0x000c, 0x08, "RTL8821CU", "rtl8821c_fw.bin", "rtl8821c_config.bin", false},
    {0x8761, 0x000a, 0x06, "RTL8761AU", "rtl8761a_fw.bin", "rtl8761a_config.bin", false},
    {0x8761, 0x000b, 0x0a, "RTL8761BU", "rtl8761bu_fw.bin", "rtl8761bu_config.bin", false},
    {0x8761, 0x000e, 0x00, "RTL8761CU", "rtl8761cu_fw.bin", "rtl8761cu_config.bin", false},
    {0x8822, 0x000b, 0x07, "RTL8822BU", "rtl8822b_fw.bin", "rtl8822b_config.bin", true},
    {0x8822, 0x000c, 0x0a, "RTL8822CU", "rtl8822cu_fw.bin", "rtl8822cu_config.bin", false},
    {0x8852, 0x000a, 0x0b, "RTL8852AU", "rtl8852au_fw.bin", "rtl8852au_config.bin", false},
    {0x8852, 0x000b, 0x0b, "RTL8852BU", "rtl8852bu_fw.bin", "rtl8852bu_config.bin", false},
    {0x8852, 0x000c, 0x0c, "RTL8852CU", "rtl8852cu_fw.bin", "rtl8852cu_config.bin", false},
    {0x8851, 0x000b, 0x0c, "RTL8851BU", "rtl8851bu_fw.bin", "rtl8851bu_config.bin", false},
    {0x8922, 0x000a, 0x0c, "RTL8922AU", "rtl8922au_fw.bin", "rtl8922au_config.bin", false},
    {0x8852, 0x0087, 0x0c, "RTL8852BTU", "rtl8852btu_fw.bin", "rtl8852btu_config.bin", false},
};

bool hasSignature(const std::vector<std::uint8_t>& data, const char* signature)
{
    return data.size() >= 8 && std::memcmp(data.data(), signature, 8) == 0;
}

// v1: header, then two arrays per patch (chip id, length, offset), then the
// patches. The patch for this chip is copied and stamped with the file's
// version in its last four bytes.
bool buildV1(const std::vector<std::uint8_t>& firmware, std::uint8_t romVersion, std::vector<std::uint8_t>& out,
             std::string& why)
{
    if (firmware.size() < kEpatchHeader)
    {
        why = "the file is too short";
        return false;
    }
    const std::uint16_t patches = getLe16(firmware.data() + 12);

    if (patches == 0 || patches > 256)
    {
        why = "it has no patch table";
        return false;
    }
    const std::size_t table = kEpatchHeader + 8u * patches;

    if (firmware.size() < table)
    {
        why = "the patch table is cut off";
        return false;
    }

    const std::uint8_t* ids = firmware.data() + kEpatchHeader;
    const std::uint8_t* lengths = ids + 2u * patches;
    const std::uint8_t* offsets = lengths + 2u * patches;
    const std::uint16_t wanted = static_cast<std::uint16_t>(romVersion + 1);

    for (std::uint16_t i = 0; i < patches; i++)
    {
        if (getLe16(ids + 2u * i) != wanted)
        {
            continue;
        }
        const std::uint16_t length = getLe16(lengths + 2u * i);
        const std::uint32_t offset = getLe32(offsets + 4u * i);

        if (offset == 0 || length < 4 || offset > firmware.size() || length > firmware.size() - offset)
        {
            why = "its patch for this chip is out of range";
            return false;
        }
        out.assign(firmware.begin() + offset, firmware.begin() + offset + length);
        std::memcpy(out.data() + out.size() - 4, firmware.data() + 8, 4); // the file's version
        return true;
    }
    why = "it has no patch for ROM version " + std::to_string(romVersion);
    return false;
}

struct Piece
{
    std::uint8_t priority;
    std::vector<std::uint8_t> data;
};

// v2: a section list. Each section carries subsections for the chip's ROM
// version (the "eco"); they are applied in priority order. Security sections
// are only used by chips with a key id, which is not the case for the sticks
// this loader is for, so they are skipped.
bool buildV2(const std::vector<std::uint8_t>& firmware, std::uint8_t romVersion, std::vector<std::uint8_t>& out,
             std::string& why)
{
    if (firmware.size() < kEpatchHeaderV2 + kV2Tail)
    {
        why = "the file is too short";
        return false;
    }
    const std::size_t end = firmware.size() - kV2Tail;
    const std::uint32_t sections = getLe32(firmware.data() + 16);
    std::size_t pos = kEpatchHeaderV2;
    std::vector<Piece> pieces;

    for (std::uint32_t s = 0; s < sections; s++)
    {
        if (pos + 8 > end)
        {
            why = "a section is cut off";
            return false;
        }
        const std::uint32_t opcode = getLe32(firmware.data() + pos);
        const std::uint32_t length = getLe32(firmware.data() + pos + 4);

        pos += 8;
        if (length > end - pos)
        {
            why = "a section is cut off";
            return false;
        }
        const std::size_t sectionEnd = pos + length;

        switch (opcode)
        {
        case kSectionSnippets:
        case kSectionDummy: {
            if (length < 4)
            {
                why = "a section is cut off";
                return false;
            }
            const std::uint16_t count = getLe16(firmware.data() + pos);
            std::size_t at = pos + 4;

            for (std::uint16_t i = 0; i < count; i++)
            {
                if (at + 8 > sectionEnd)
                {
                    why = "a subsection is cut off";
                    return false;
                }
                const std::uint8_t eco = firmware[at];
                const std::uint8_t priority = firmware[at + 1];
                const std::uint32_t size = getLe32(firmware.data() + at + 4);

                at += 8;
                if (size > sectionEnd - at)
                {
                    why = "a subsection is cut off";
                    return false;
                }
                if (eco == static_cast<std::uint8_t>(romVersion + 1))
                {
                    pieces.push_back(
                        {priority, std::vector<std::uint8_t>(firmware.begin() + at, firmware.begin() + at + size)});
                }
                at += size;
            }
            break;
        }
        case kSectionSecurity:
            // it needs the chip's key id, which this loader does not read
            break;
        default:
            break;
        }
        pos = sectionEnd;
    }
    if (pieces.empty())
    {
        why = "it has no patch for ROM version " + std::to_string(romVersion);
        return false;
    }
    std::stable_sort(pieces.begin(), pieces.end(),
                     [](const Piece& a, const Piece& b) { return a.priority < b.priority; });
    out.clear();
    for (const Piece& piece : pieces)
    {
        out.insert(out.end(), piece.data.begin(), piece.data.end());
    }
    return true;
}

} // namespace

const RealtekChip* realtekChipFor(const ChipIdentity& identity)
{
    for (const RealtekChip& chip : kChips)
    {
        if (chip.lmpSubversion == identity.lmpSubversion && chip.hciRevision == identity.hciRevision &&
            chip.hciVersion == identity.hciVersion)
        {
            return &chip;
        }
    }
    return nullptr;
}

bool realtekInRomMode(const ChipIdentity& identity)
{
    for (const RealtekChip& chip : kChips)
    {
        if (chip.lmpSubversion == identity.lmpSubversion)
        {
            return true;
        }
    }
    return false;
}

bool realtekImage(const std::vector<std::uint8_t>& firmware, const std::vector<std::uint8_t>& config,
                  std::uint8_t romVersion, std::vector<std::uint8_t>& out, std::string& why)
{
    out.clear();
    why.clear();
    if (hasSignature(firmware, "Realtech"))
    {
        if (!buildV1(firmware, romVersion, out, why))
        {
            return false;
        }
    }
    else if (hasSignature(firmware, "RTBTCore"))
    {
        if (!buildV2(firmware, romVersion, out, why))
        {
            return false;
        }
    }
    else
    {
        why = "it is not a Realtek firmware file";
        return false;
    }
    // the chip parses the vendor config appended after the patch
    out.insert(out.end(), config.begin(), config.end());
    return true;
}

std::vector<std::vector<std::uint8_t>> realtekFragments(const std::vector<std::uint8_t>& image)
{
    std::vector<std::vector<std::uint8_t>> fragments;
    const std::size_t count = image.size() / kFragment + 1;
    std::size_t index = 0;

    for (std::size_t i = 0; i < count; i++)
    {
        std::uint8_t header = static_cast<std::uint8_t>(index++);
        std::size_t length = kFragment;

        if (header == 0x7f) // 0x80 is the end marker
        {
            index = 1;
        }
        if (i == count - 1)
        {
            header |= 0x80;
            length = image.size() % kFragment;
        }
        std::vector<std::uint8_t> fragment;
        fragment.reserve(length + 1);
        fragment.push_back(header);
        if (length > 0)
        {
            fragment.insert(fragment.end(), image.begin() + i * kFragment, image.begin() + i * kFragment + length);
        }
        fragments.push_back(std::move(fragment));
    }
    return fragments;
}

FirmwareResult realtekInstall(const RealtekChip& chip, IFirmwareStore& files, FirmwareHci& hci)
{
    if (!hci.sync(kReadRomVersion, nullptr, 0, 3))
    {
        return {false, "the adapter did not report its Realtek ROM version"};
    }
    std::uint32_t length = 0;
    const std::uint8_t* reply = hci.reply(length);

    if (reply == nullptr || length < 7 || reply[5] != 0)
    {
        return {false, "the adapter did not report its Realtek ROM version"};
    }
    const std::uint8_t romVersion = reply[6];

    std::vector<std::uint8_t> firmware;
    if (!files.read(chip.firmwareFile, firmware))
    {
        return {false,
                std::string("this adapter needs firmware: put ") + chip.firmwareFile + " in /data/bluev/firmware/"};
    }
    std::vector<std::uint8_t> config;
    if (chip.configFile != nullptr && !files.read(chip.configFile, config) && chip.configRequired)
    {
        return {false, std::string("this adapter needs firmware: put ") + chip.firmwareFile + " and " +
                           chip.configFile + " in /data/bluev/firmware/"};
    }

    std::vector<std::uint8_t> image;
    std::string why;
    if (!realtekImage(firmware, config, romVersion, image, why))
    {
        return {false,
                std::string("the ") + chip.name + " firmware " + chip.firmwareFile + " could not be used: " + why};
    }

    const std::vector<std::vector<std::uint8_t>> fragments = realtekFragments(image);
    for (std::size_t i = 0; i < fragments.size(); i++)
    {
        if (!hci.sync(kDownloadPatch, fragments[i].data(), static_cast<std::uint8_t>(fragments[i].size()), 1))
        {
            return {false, "the adapter stopped accepting the firmware (fragment " + std::to_string(i + 1) + " of " +
                               std::to_string(fragments.size()) + ")"};
        }
    }
    if (!hci.sync(kReset, nullptr, 0, 5))
    {
        return {false, "the adapter did not answer the reset after the firmware upload"};
    }
    return {true, std::string(chip.name) + " firmware " + chip.firmwareFile + " uploaded (" +
                      std::to_string(image.size()) + " bytes in " + std::to_string(fragments.size()) + " fragments)"};
}

} // namespace bluev
