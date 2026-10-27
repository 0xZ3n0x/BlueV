// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/bluetooth/sdp_server.hpp"

#include <cstddef>
#include <cstring>

#include "core/bytes.hpp"
#include "core/ports/log.hpp"

namespace bluev
{

namespace
{

// DataElement size header for the fixed sizes 1/2/4/8/16
constexpr int kFixedSize[5] = {1, 2, 4, 8, 16};

// UUIDs we consider "A2DP related" when a device searches us
constexpr std::uint32_t kA2dpUuids[] = {0x110a, 0x110d, 0x0019, 0x0100};
constexpr std::uint32_t kPublicBrowseRoot = 0x1002;

// the advertised service record, attribute by attribute
struct Attribute
{
    std::uint32_t id;
    const std::uint8_t* blob;
    std::uint32_t length;
};

#define BLOB(text) reinterpret_cast<const std::uint8_t*>(text)
const Attribute kRecord[] = {
    {0x000, BLOB("\x0a\x00\x01\x00\x01"), 5}, // service record handle 0x00010001
    {0x001, BLOB("\x35\x03\x19\x11\x0a"), 5}, // class: audio source
    {0x004,
     BLOB("\x35\x10\x35\x06\x19\x01\x00\x09\x00\x19" // protocols: L2CAP on psm 0x19,
          "\x35\x06\x19\x00\x19\x09\x01\x03"),
     18},                                                          // then AVDTP 1.3
    {0x005, BLOB("\x35\x03\x19\x10\x02"), 5},                      // browse group: public root
    {0x009, BLOB("\x35\x08\x35\x06\x19\x11\x0d\x09\x01\x03"), 10}, // profile: A2DP 1.3
    {0x100,
     BLOB("\x25\x05"
          "BlueV"),
     7},                              // service name
    {0x311, BLOB("\x09\x00\x01"), 3}, // supported features: player
};
#undef BLOB

// Parses one DataElement header (not necessarily a sequence). Returns the
// header length and sets *elen to the data length; 0 when it does not fit.
int deHeader(const std::uint8_t* p, std::uint32_t avail, std::uint32_t* elen)
{
    if (static_cast<int>(avail) < 1)
    {
        return 0;
    }
    const std::uint8_t t = p[0];

    if (t < 8)
    {
        *elen = 0;
        return 1;
    }
    switch (t & 7)
    {
    case 0:
    case 1:
    case 2:
    case 3:
    case 4:
        *elen = static_cast<std::uint32_t>(kFixedSize[t & 7]);
        return static_cast<int>(*elen) < static_cast<int>(avail) ? 1 : 0;
    case 5:
        if (avail == 1)
        {
            return 0;
        }
        *elen = p[1];
        return (*elen + 2 <= avail) ? 2 : 0;
    case 6:
        if (avail < 3)
        {
            return 0;
        }
        *elen = getBe16(p + 1);
        return (*elen + 3 <= avail) ? 3 : 0;
    default:
        if (avail < 5)
        {
            return 0;
        }
        *elen = getBe32(p + 1);
        return (static_cast<int>(*elen + 5) <= static_cast<int>(avail)) ? 5 : 0;
    }
}

int seqHeader(const std::uint8_t* p, std::uint32_t avail, std::uint32_t* elen)
{
    const int h = deHeader(p, avail, elen);

    if (h == 0 || (p[0] & 0xf8) != 0x30)
    {
        return 0;
    }
    return h;
}

// Does the search pattern contain an A2DP UUID? 1 = yes, 0 = no, -1 = bad.
int patternMatches(const std::uint8_t* pat, std::uint32_t len, int* consumed)
{
    std::uint32_t plen = 0;
    const std::uint32_t hdr = static_cast<std::uint32_t>(seqHeader(pat, len, &plen));
    int hit = 0;

    if (hdr == 0)
    {
        return -1;
    }
    const std::uint32_t end = hdr + plen;

    *consumed = static_cast<int>(end);
    if (plen < 1)
    {
        return 0;
    }

    std::uint32_t off = hdr;
    while (off < end)
    {
        const std::uint8_t* e = pat + off;
        std::uint32_t elen = 0, uuid = 0;
        const std::uint32_t ehdr = static_cast<std::uint32_t>(deHeader(e, end - off, &elen));

        if (ehdr == 0 || (e[0] & 0xf8) != 0x18)
        {
            return -1;
        }

        // UUID value, big endian in 2 or 4 bytes
        if (elen == 0x10 || elen == 4)
        {
            uuid = getBe32(e + ehdr);
        }
        else if (elen == 2)
        {
            uuid = getBe16(e + ehdr);
        }

        bool ours = uuid == kPublicBrowseRoot;

        for (std::uint32_t known : kA2dpUuids)
        {
            if (uuid == known)
            {
                ours = true;
            }
        }
        if (ours)
        {
            hit = 1;
        }
        if (elen == 0x10)
        {
            const std::uint8_t* u = e + ehdr;

            logLine("sdp: searched uuid %02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x (%s)",
                    u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7], u[8], u[9], u[10], u[11], u[12], u[13], u[14],
                    u[15], ours ? "ours" : "not ours");
        }
        else
        {
            logLine("sdp: searched uuid %#06x (%s)", static_cast<unsigned>(uuid), ours ? "ours" : "not ours");
        }

        off += elen + ehdr;
        if (off >= end)
        {
            return hit;
        }
    }
    return hit;
}

// Builds the attribute list for the requested attribute ID ranges; the
// length of the list, or -1.
int buildAttributes(const std::uint8_t* ranges, std::uint32_t len, int* consumed, std::uint8_t* out, int outcap)
{
    std::uint32_t plen = 0;
    const std::uint32_t hdr = static_cast<std::uint32_t>(seqHeader(ranges, len, &plen));
    int pos = 3;

    if (hdr == 0)
    {
        return -1;
    }
    const std::uint32_t end = hdr + plen;

    *consumed = static_cast<int>(end);

    for (const Attribute& attr : kRecord)
    {
        const std::uint8_t* r = ranges + hdr;
        bool match = false;

        if (static_cast<int>(plen) < 1)
        {
            continue;
        }
        while (r < ranges + end)
        {
            std::uint32_t elen = 0, lo, hi;
            const std::uint32_t ehdr =
                static_cast<std::uint32_t>(deHeader(r, end - static_cast<std::uint32_t>(r - ranges), &elen));

            if (ehdr == 0 || (r[0] >> 3) != 1)
            {
                return -1; // must be an unsigned int
            }

            if (elen == 4)
            { // lo..hi range
                lo = getBe16(r + ehdr);
                hi = getBe16(r + ehdr + 2);
                if (lo <= attr.id && attr.id <= hi)
                {
                    match = true;
                }
            }
            else if (elen == 2)
            { // single attribute
                lo = getBe16(r + ehdr);
                if (lo == attr.id)
                {
                    match = true;
                }
            }
            r += elen + ehdr;
        }
        if (match)
        {
            const int need = static_cast<int>(attr.length) + 3 + pos;

            if (outcap < need)
            {
                return -1;
            }
            out[pos] = 0x09; // uint32 attribute id
            putBe16(out + pos + 1, attr.id);
            std::memcpy(out + pos + 3, attr.blob, attr.length);
            pos = need;
        }
    }
    out[0] = 0x36; // data element sequence, 16-bit
    putBe16(out + 1, pos - 3);
    return pos;
}

constexpr std::uint8_t kInvalidRecordHandle = 0x02;
constexpr std::uint8_t kInvalidContinuation = 0x03;

int errorResponse(std::uint8_t* out, std::uint8_t tid1, std::uint8_t tid2, std::uint8_t code = kInvalidContinuation)
{
    out[0] = 0x01; // error response
    out[1] = tid1;
    out[2] = tid2;
    putBe16(out + 3, 2); // parameter length
    putBe16(out + 5, code);
    return 7;
}

} // namespace

int SdpServer::handle(const std::uint8_t* req, std::uint32_t reqlen, std::uint8_t* out, int outcap)
{
    if (static_cast<int>(reqlen) < 5)
    {
        return 0;
    }

    const std::uint8_t pdu = req[0];
    const std::uint8_t tid1 = req[1], tid2 = req[2];
    const std::uint32_t plen = getBe16(req + 3);
    int consumed = 0;
    int r;

    if (outcap < 64)
    {
        return 0;
    }
    if (reqlen < plen + 5)
    {
        return errorResponse(out, tid1, tid2);
    }

    switch (pdu)
    {
    case 0x02: { // service search
        r = patternMatches(req + 5, plen, &consumed);
        if (r < 0)
        {
            return errorResponse(out, tid1, tid2);
        }
        logLine("sdp: service search -> %d record(s)", r);
        out[0] = 0x03;
        out[1] = tid1;
        out[2] = tid2;
        putBe16(out + 5, r); // total record count
        putBe16(out + 7, r); // current record count
        if (r != 0)
        {
            out[9] = 0x00;
            out[10] = 0x01;
            out[11] = 0x00;
            out[12] = 0x01;
            out[13] = 0;
            out[3] = 0;
            out[4] = 13 - 4;
            return 14;
        }
        out[9] = 0;
        out[3] = 0;
        out[4] = 9 - 4;
        return 10;
    }
    case 0x04: { // service attribute
        if (plen < 6)
        {
            return errorResponse(out, tid1, tid2);
        }
        const std::uint32_t handle = getBe32(req + 5);

        if (handle != 0x10001)
        {
            return errorResponse(out, tid1, tid2, kInvalidRecordHandle);
        }
        r = buildAttributes(req + 11, plen - 6, &consumed, out + 7, outcap - 8);
        if (r < 0)
        {
            return errorResponse(out, tid1, tid2);
        }
        logLine("sdp: attribute request -> %d bytes", r);
        out[0] = 0x05;
        out[1] = tid1;
        out[2] = tid2;
        const std::uint32_t outlen = static_cast<std::uint32_t>(r);

        putBe16(out + 5, outlen);
        out[outlen + 7] = 0; // no continuation state
        putBe16(out + 3, outlen + 3);
        return static_cast<int>(outlen + 8);
    }
    case 0x06: { // service search attribute
        r = patternMatches(req + 5, plen, &consumed);
        if (r < 0 || consumed + 2 > static_cast<int>(plen))
        {
            return errorResponse(out, tid1, tid2);
        }
        if (r == 0)
        {
            out[7] = 0x35; // empty attribute list
            out[8] = 0;
            r = 2;
            logLine("sdp: search+attributes -> %d record(s), %d bytes", 0, r);
            out[0] = 0x07;
            out[1] = tid1;
            out[2] = tid2;
            putBe16(out + 5, r);
            out[r + 7] = 0;
            putBe16(out + 3, r + 3);
            return r + 8;
        }
        int dummy = 0;
        const std::int32_t outlen = buildAttributes(
            req + 5 + consumed + 2, plen - static_cast<std::uint32_t>(consumed) - 2, &dummy, out + 10, outcap - 11);

        if (outlen < 0)
        {
            return errorResponse(out, tid1, tid2);
        }
        out[7] = 0x36;
        putBe16(out + 8, outlen);
        logLine("sdp: search+attributes -> %d record(s), %d bytes", r, outlen + 3);
        out[0] = 0x07;
        out[1] = tid1;
        out[2] = tid2;
        putBe16(out + 5, outlen + 3);
        out[outlen + 3 + 7] = 0;
        putBe16(out + 3, outlen + 3 + 3);
        return outlen + 3 + 8;
    }
    default:
        logLine("sdp: unhandled PDU %#04x", pdu);
        break;
    }
    return errorResponse(out, tid1, tid2);
}

} // namespace bluev
