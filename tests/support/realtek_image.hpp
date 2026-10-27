// SPDX-License-Identifier: GPL-3.0-or-later
// Small Realtek firmware files for the tests: a v1 "Realtech" blob whose first
// patch is for `romVersion`, and a v2 "RTBTCore" blob with subsections in a
// different priority order plus one for another ROM version.
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

inline std::vector<std::uint8_t> realtekV1Image(std::uint8_t romVersion)
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
    return fw;
}

inline std::vector<std::uint8_t> realtekV2Image(std::uint8_t romVersion)
{
    std::vector<std::uint8_t> fw = {'R', 'T', 'B', 'T', 'C', 'o', 'r', 'e'};

    for (int i = 1; i <= 8; i++) // the file's version, not used by the parser
    {
        fw.push_back(static_cast<std::uint8_t>(i));
    }

    std::vector<std::uint8_t> snippets;

    putLe16(snippets, 3); // subsections
    putLe16(snippets, 0);
    const auto subsection = [&snippets](std::uint8_t eco, std::uint8_t priority,
                                        const std::vector<std::uint8_t>& data) {
        snippets.push_back(eco);
        snippets.push_back(priority);
        snippets.push_back(0); // cb
        snippets.push_back(0);
        putLe32(snippets, static_cast<std::uint32_t>(data.size()));
        snippets.insert(snippets.end(), data.begin(), data.end());
    };
    subsection(static_cast<std::uint8_t>(romVersion + 1), 2, {0xaa, 0xbb, 0xcc, 0xdd});
    subsection(static_cast<std::uint8_t>(romVersion + 1), 1, {0x11, 0x22, 0x33});
    subsection(static_cast<std::uint8_t>(romVersion + 2), 0, {0xee, 0xff});

    putLe32(fw, 2); // sections: snippets, then a security section that is skipped

    putLe32(fw, 1);
    putLe32(fw, static_cast<std::uint32_t>(snippets.size()));
    fw.insert(fw.end(), snippets.begin(), snippets.end());

    putLe32(fw, 3);
    putLe32(fw, 4);
    fw.insert(fw.end(), {0, 0, 0, 0});

    fw.insert(fw.end(), 7, 0x5a); // the extension section after the last section
    return fw;
}

} // namespace bluev::testing
