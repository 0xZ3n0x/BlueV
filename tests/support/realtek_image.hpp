// SPDX-License-Identifier: GPL-3.0-or-later
// Small Realtek firmware files for the tests: a v1 "Realtech" blob whose first
// patch is for `romVersion`, and a v2 "RTBTCore" blob with subsections in a
// different priority order plus one for another ROM version. Both carry the
// extension that btrtl uses to check the project id.
#pragma once

#include <cstdint>
#include <vector>

namespace bluev::testing
{

inline void putLe16(std::vector<std::uint8_t>& out, unsigned value)
{
    out.push_back(static_cast<std::uint8_t>(value & 0xff));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
}

inline void putLe32(std::vector<std::uint8_t>& out, std::uint32_t value)
{
    putLe16(out, value & 0xffff);
    putLe16(out, value >> 16);
}

// The version instruction (opcode 0, length 1) and the extension signature
// that ends every Realtek container.
inline void putExtension(std::vector<std::uint8_t>& fw, std::uint8_t projectId)
{
    fw.push_back(projectId);
    fw.push_back(1);
    fw.push_back(0);
    fw.insert(fw.end(), {0x51, 0x04, 0xfd, 0x77});
}

inline std::vector<std::uint8_t> realtekV1Image(std::uint8_t romVersion, std::uint8_t projectId = 14)
{
    std::vector<std::uint8_t> fw = {'R', 'e', 'a', 'l', 't', 'e', 'c', 'h'};

    putLe32(fw, 0x11223344);                            // the version stamped into the patch
    putLe16(fw, 2);                                     // two patches
    putLe16(fw, static_cast<unsigned>(romVersion) + 1); // the one for this chip
    putLe16(fw, 0x0099);
    putLe16(fw, 8); // lengths
    putLe16(fw, 4);
    putLe32(fw, 30); // offsets: header 14 + table 16
    putLe32(fw, 38);
    for (int i = 1; i <= 8; i++) // the patch, 8 bytes
    {
        fw.push_back(static_cast<std::uint8_t>(i));
    }
    fw.insert(fw.end(), {0xaa, 0xbb, 0xcc, 0xdd});
    putExtension(fw, projectId);
    return fw;
}

inline std::vector<std::uint8_t> realtekV2Image(std::uint8_t romVersion, std::uint8_t projectId = 14)
{
    std::vector<std::uint8_t> fw = {'R', 'T', 'B', 'T', 'C', 'o', 'r', 'e'};

    for (int i = 1; i <= 8; i++) // the file's version, not used by the parser
    {
        fw.push_back(static_cast<std::uint8_t>(i));
    }

    std::vector<std::uint8_t> snippets;
    const auto subsection = [](std::vector<std::uint8_t>& out, std::uint8_t eco, std::uint8_t priority,
                               std::uint8_t keyId, const std::vector<std::uint8_t>& data) {
        out.push_back(eco);
        out.push_back(priority);
        out.push_back(keyId); // cb[0] for snippets, key id for security sections
        out.push_back(0);
        putLe32(out, static_cast<std::uint32_t>(data.size()));
        out.insert(out.end(), data.begin(), data.end());
    };

    putLe16(snippets, 4); // subsections
    putLe16(snippets, 0);
    subsection(snippets, static_cast<std::uint8_t>(romVersion + 1), 2, 0, {0xaa, 0xbb, 0xcc, 0xdd});
    subsection(snippets, static_cast<std::uint8_t>(romVersion + 1), 1, 0, {0x11, 0x22, 0x33});
    subsection(snippets, static_cast<std::uint8_t>(romVersion + 2), 0, 0, {0xee, 0xff});
    // the same priority as the one before: btrtl puts it first, in reverse file order
    subsection(snippets, static_cast<std::uint8_t>(romVersion + 1), 1, 0, {0x44, 0x55});

    std::vector<std::uint8_t> security;

    putLe16(security, 2); // subsections
    putLe16(security, 0);
    subsection(security, static_cast<std::uint8_t>(romVersion + 1), 0, 7, {0xe1, 0xe2});
    subsection(security, static_cast<std::uint8_t>(romVersion + 1), 0, 9, {0xf1, 0xf2});

    putLe32(fw, 2); // sections: snippets, then the security section

    putLe32(fw, 1);
    putLe32(fw, static_cast<std::uint32_t>(snippets.size()));
    fw.insert(fw.end(), snippets.begin(), snippets.end());

    putLe32(fw, 3);
    putLe32(fw, static_cast<std::uint32_t>(security.size()));
    fw.insert(fw.end(), security.begin(), security.end());

    putExtension(fw, projectId);
    return fw;
}

} // namespace bluev::testing
