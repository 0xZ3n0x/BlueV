// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/audio/avdtp.hpp"

#include <cstring>

namespace bluev::avdtp
{

bool parseHeader(const std::uint8_t* frame, int length, Header& out)
{
    if (length < 2 || (frame[0] & 0x0c) != 0)
    {
        return false; // short, or not a single packet
    }
    out.label = frame[0] >> 4;
    out.type = frame[0] & 3;
    out.signal = frame[1] & 0x3f;
    return true;
}

std::size_t buildCommand(std::uint8_t label, std::uint8_t signal, const std::uint8_t* params, int length,
                         std::uint8_t* out)
{
    out[0] = static_cast<std::uint8_t>((label & 0xf) << 4);
    out[1] = signal;
    if (length > 0)
    {
        std::memcpy(out + 2, params, static_cast<std::size_t>(length));
    }
    return static_cast<std::size_t>(length) + 2;
}

std::size_t buildAccept(std::uint8_t label, std::uint8_t signal, std::uint8_t* out)
{
    out[0] = static_cast<std::uint8_t>(((label & 0xf) << 4) | kAccept);
    out[1] = signal;
    return 2;
}

std::size_t buildReject(std::uint8_t label, std::uint8_t signal, std::uint8_t error, std::uint8_t* out)
{
    out[0] = static_cast<std::uint8_t>(((label & 0xf) << 4) | kReject);
    out[1] = signal;
    out[2] = error;
    return 3;
}

std::size_t buildDiscoverAccept(std::uint8_t label, std::uint8_t* out)
{
    out[0] = static_cast<std::uint8_t>(((label & 0xf) << 4) | kAccept);
    out[1] = kDiscover;
    out[2] = 1 << 2; // seid 1, not in use
    out[3] = 0x00;   // audio, source
    return 4;
}

std::size_t buildCapabilitiesAccept(std::uint8_t label, std::uint8_t* out)
{
    static const std::uint8_t kCaps[10] = {0x01, 0x00,              // media transport
                                           0x07, 0x06, 0x00, 0x00,  // media codec: audio, SBC
                                           0xff, 0xff, 0x02, 0x35}; // all options, bitpool 2..53
    out[0] = static_cast<std::uint8_t>(((label & 0xf) << 4) | kAccept);
    out[1] = kGetCapabilities;
    std::memcpy(out + 2, kCaps, sizeof kCaps);
    return 12;
}

bool parseSetConfiguration(const std::uint8_t* buf, int len, PeerConfig& out)
{
    out = PeerConfig{};
    if (len < 14)
    {
        return false;
    }
    out.seid = buf[3]; // the device's own endpoint
    int o = 4;         // skip the acceptor and initiator seids

    while (o + 2 <= len)
    {
        const int clen = buf[o + 1];

        if (o + 2 + clen > len)
        {
            break;
        }
        if (buf[o] == 7 && clen >= 6 && buf[o + 2] == 0 && buf[o + 3] == 0)
        {
            std::memcpy(&out.caps, buf + o + 4, 4);
            out.haveSbc = true;
        }
        o += 2 + clen;
    }
    return true;
}

int parseEndpoints(const std::uint8_t* rsp, int length, Endpoint* out, int max)
{
    int n = 0;

    // {seid << 2 | in use, type << 3 | media type} pairs after the header
    for (int k = 2; k + 1 < length; k += 2)
    {
        if (n >= max)
        {
            break;
        }
        out[n].seid = static_cast<std::uint8_t>(rsp[k] >> 2);
        out[n].isSink = ((rsp[k + 1] >> 3) & 1) != 0;
        n++;
    }
    return n;
}

bool findSbcCapabilities(const std::uint8_t* rsp, int length, std::uint32_t& caps)
{
    int off = 2;

    if (length < 4)
    {
        return false;
    }
    do
    {
        const int clen = rsp[off + 1];

        if (length < off + 2 + clen)
        {
            break;
        }
        if (clen > 5 && rsp[off] == 7 && rsp[off + 3] == 0)
        {
            std::memcpy(&caps, rsp + off + 4, 4);
            return true;
        }
        off += 2 + clen;
    } while (off + 2 <= length);
    return false;
}

} // namespace bluev::avdtp
