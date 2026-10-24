// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/bluetooth/device_filter.hpp"

#include <cstring>

namespace bluev
{

namespace
{

// names that mark TVs and the like (shown as "other" in the UI)
const char* const kJunkWords[] = {"tv",         "webos",   "bravia", "roku",    "oled",  "qled", "soundbar",
                                  "chromecast", "fire tv", "[lg]",   "hisense", "vizio", nullptr};

// case-insensitive substring search (ASCII)
bool containsNoCase(const char* hay, const char* needle)
{
    for (; *hay; hay++)
    {
        const char *h = hay, *n = needle;

        while (*h && *n)
        {
            char a = *h, b = *n;

            if (a >= 'A' && a <= 'Z')
            {
                a |= 0x20;
            }
            if (b >= 'A' && b <= 'Z')
            {
                b |= 0x20;
            }
            if (a != b)
            {
                break;
            }
            h++;
            n++;
        }
        if (*n == 0)
        {
            return true;
        }
    }
    return false;
}

} // namespace

std::string eirName(const std::uint8_t* eir, int length)
{
    std::string name;

    if (eir == nullptr || length <= 1)
    {
        return name;
    }
    int off = 0;
    while (off < length)
    {
        const std::uint8_t l = eir[off];

        if (l == 0 || off + 1 + l > length)
        {
            break;
        }
        if ((eir[off + 1] & 0xfe) == 8)
        { // shortened/complete
            std::size_t n = l - 1;

            if (n > 63)
            {
                n = 63;
            }
            name.assign(reinterpret_cast<const char*>(eir + off + 2), n);
            // a name ends at an embedded NUL
            name.resize(std::strlen(name.c_str()));
        }
        off += 1 + l;
    }
    return name;
}

bool isJunkDevice(std::uint32_t classOfDevice, const std::string& name)
{
    // The minor class field is 6 bits. 9..16 are the audio/video ones that are
    // not for listening: set-top box, VCR, cameras, monitor, display with
    // speaker, conferencing; 10 (hi-fi audio) is left out. The major class is
    // not looked at.
    const std::uint32_t c = ((classOfDevice >> 2) & 0x3f) - 9;

    if (c <= 7 && (0xfd >> (c & 0x1f) & 1))
    {
        return true;
    }
    if (name.empty())
    {
        return false;
    }
    for (const char* const* w = kJunkWords; *w; w++)
    {
        if (containsNoCase(name.c_str(), *w))
        {
            return true;
        }
    }
    return false;
}

} // namespace bluev
