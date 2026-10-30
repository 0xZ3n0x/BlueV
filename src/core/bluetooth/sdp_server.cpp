// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/bluetooth/sdp_server.hpp"

#include <cstddef>
#include <cstring>
#include <iterator>

#include "core/bytes.hpp"
#include "core/ports/log.hpp"

namespace bluev
{

namespace
{

// DataElement size header for the fixed sizes 1/2/4/8/16
constexpr int kFixedSize[5] = {1, 2, 4, 8, 16};

constexpr std::uint32_t kPublicBrowseRoot = 0x1002;

// one attribute of a service record
struct Attribute
{
    std::uint32_t id;
    const std::uint8_t* blob;
    std::uint32_t length;
};

#define BLOB(text) reinterpret_cast<const std::uint8_t*>(text)

// the A2DP audio source
const std::uint32_t kA2dpUuids[] = {0x110a, 0x110d, 0x0019, 0x0100, kPublicBrowseRoot};
const Attribute kA2dpAttributes[] = {
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

// the AV remote control controller (AVRCP 1.4): we only send the volume
const std::uint32_t kAvrcpUuids[] = {0x110e, 0x0017, 0x0100, kPublicBrowseRoot};
const Attribute kAvrcpAttributes[] = {
    {0x000, BLOB("\x0a\x00\x01\x00\x02"), 5}, // service record handle 0x00010002
    {0x001, BLOB("\x35\x03\x19\x11\x0e"), 5}, // class: remote control
    {0x004,
     BLOB("\x35\x10\x35\x06\x19\x01\x00\x09\x00\x17" // protocols: L2CAP on psm 0x17,
          "\x35\x06\x19\x00\x17\x09\x01\x04"),
     18},                                                          // then AVCTP 1.4
    {0x005, BLOB("\x35\x03\x19\x10\x02"), 5},                      // browse group: public root
    {0x009, BLOB("\x35\x08\x35\x06\x19\x11\x0e\x09\x01\x04"), 10}, // profile: AVRCP 1.4
    {0x100,
     BLOB("\x25\x05"
          "BlueV"),
     7},                              // service name
    {0x311, BLOB("\x09\x00\x01"), 3}, // supported features: category 1
};
#undef BLOB

// A service record we advertise. A search matches it when the pattern names
// one of its UUIDs.
struct Record
{
    std::uint32_t handle;
    const std::uint32_t* uuids;
    std::size_t uuidCount;
    const Attribute* attributes;
    std::size_t attributeCount;
};

const Record kRecords[] = {
    {0x10001, kA2dpUuids, std::size(kA2dpUuids), kA2dpAttributes, std::size(kA2dpAttributes)},
    {0x10002, kAvrcpUuids, std::size(kAvrcpUuids), kAvrcpAttributes, std::size(kAvrcpAttributes)},
};
constexpr unsigned kRecordCount = std::size(kRecords);

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

// Which of our records does the search pattern name? Bit i of *mask is set
// when kRecords[i] has one of its UUIDs. False when the pattern is malformed.
bool patternMatches(const std::uint8_t* pat, std::uint32_t len, int* consumed, unsigned* mask)
{
    std::uint32_t plen = 0;
    const std::uint32_t hdr = static_cast<std::uint32_t>(seqHeader(pat, len, &plen));

    *mask = 0;
    if (hdr == 0)
    {
        return false;
    }
    const std::uint32_t end = hdr + plen;

    *consumed = static_cast<int>(end);

    std::uint32_t off = hdr;
    while (off < end)
    {
        const std::uint8_t* e = pat + off;
        std::uint32_t elen = 0, uuid = 0;
        const std::uint32_t ehdr = static_cast<std::uint32_t>(deHeader(e, end - off, &elen));

        if (ehdr == 0 || (e[0] & 0xf8) != 0x18)
        {
            return false;
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

        unsigned named = 0;

        for (unsigned i = 0; i < kRecordCount; i++)
        {
            for (std::size_t k = 0; k < kRecords[i].uuidCount; k++)
            {
                if (kRecords[i].uuids[k] == uuid)
                {
                    named |= 1u << i;
                }
            }
        }
        *mask |= named;
        if (elen == 0x10)
        {
            const std::uint8_t* u = e + ehdr;

            logLine("sdp: searched uuid %02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x (%s)",
                    u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7], u[8], u[9], u[10], u[11], u[12], u[13], u[14],
                    u[15], named != 0 ? "ours" : "not ours");
        }
        else
        {
            logLine("sdp: searched uuid %#06x (%s)", static_cast<unsigned>(uuid), named != 0 ? "ours" : "not ours");
        }

        off += elen + ehdr;
    }
    return true;
}

// Builds the attribute list of `record` for the requested attribute ID
// ranges; the length of the list, or -1.
int buildAttributes(const Record& record, const std::uint8_t* ranges, std::uint32_t len, int* consumed,
                    std::uint8_t* out, int outcap)
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

    for (std::size_t n = 0; n < record.attributeCount; n++)
    {
        const Attribute& attr = record.attributes[n];
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

// the PDU id and the transaction id (copied from the request) that start every response
void beginResponse(std::uint8_t* out, std::uint8_t pdu, const std::uint8_t* req)
{
    out[0] = pdu;
    out[1] = req[1];
    out[2] = req[2];
}

int errorResponse(std::uint8_t* out, const std::uint8_t* req, std::uint8_t code = kInvalidContinuation)
{
    beginResponse(out, 0x01, req); // error response
    putBe16(out + 3, 2);           // parameter length
    putBe16(out + 5, code);
    return 7;
}

// how many records the mask names
int matchedRecords(unsigned mask)
{
    int n = 0;

    for (unsigned i = 0; i < kRecordCount; i++)
    {
        n += (mask >> i) & 1;
    }
    return n;
}

// Service Search: the handles of the records the pattern names.
int serviceSearch(const std::uint8_t* req, std::uint32_t plen, std::uint8_t* out)
{
    int consumed = 0;
    unsigned mask = 0;

    if (!patternMatches(req + 5, plen, &consumed, &mask))
    {
        return errorResponse(out, req);
    }
    const int r = matchedRecords(mask);
    int pos = 9;

    logLine("sdp: service search -> %d record(s)", r);
    beginResponse(out, 0x03, req);
    putBe16(out + 5, r); // total record count
    putBe16(out + 7, r); // current record count
    for (unsigned i = 0; i < kRecordCount; i++)
    {
        if ((mask >> i) & 1)
        {
            putBe32(out + pos, kRecords[i].handle);
            pos += 4;
        }
    }
    out[pos] = 0; // no continuation state
    putBe16(out + 3, pos - 4);
    return pos + 1;
}

// Service Attribute: the attributes asked for, of the record with that handle.
int serviceAttribute(const std::uint8_t* req, std::uint32_t plen, std::uint8_t* out, int outcap)
{
    int consumed = 0;
    const Record* record = nullptr;

    if (plen < 6)
    {
        return errorResponse(out, req);
    }
    for (const Record& candidate : kRecords)
    {
        if (candidate.handle == getBe32(req + 5))
        {
            record = &candidate;
        }
    }
    if (record == nullptr)
    {
        return errorResponse(out, req, kInvalidRecordHandle);
    }
    const int r = buildAttributes(*record, req + 11, plen - 6, &consumed, out + 7, outcap - 8);

    if (r < 0)
    {
        return errorResponse(out, req);
    }
    logLine("sdp: attribute request -> %d bytes", r);
    beginResponse(out, 0x05, req);
    const std::uint32_t outlen = static_cast<std::uint32_t>(r);

    putBe16(out + 5, outlen);
    out[outlen + 7] = 0; // no continuation state
    putBe16(out + 3, outlen + 3);
    return static_cast<int>(outlen + 8);
}

// Service Search Attribute: the attributes of each record the pattern names,
// in one list; an empty list when it names none.
int serviceSearchAttribute(const std::uint8_t* req, std::uint32_t plen, std::uint8_t* out, int outcap)
{
    int consumed = 0;
    unsigned mask = 0;

    if (!patternMatches(req + 5, plen, &consumed, &mask) || consumed + 2 > static_cast<int>(plen))
    {
        return errorResponse(out, req);
    }
    const int r = matchedRecords(mask);
    std::int32_t outlen = 0; // the attribute lists, one after the other

    for (unsigned i = 0; i < kRecordCount; i++)
    {
        int ignored = 0;

        if (!((mask >> i) & 1))
        {
            continue;
        }
        const std::int32_t one =
            buildAttributes(kRecords[i], req + 5 + consumed + 2, plen - static_cast<std::uint32_t>(consumed) - 2,
                            &ignored, out + 10 + outlen, outcap - 11 - outlen);

        if (one < 0)
        {
            return errorResponse(out, req);
        }
        outlen += one;
    }
    out[7] = 0x36; // the list of attribute lists
    putBe16(out + 8, outlen);
    logLine("sdp: search+attributes -> %d record(s), %d bytes", r, outlen + 3);
    beginResponse(out, 0x07, req);
    putBe16(out + 5, outlen + 3);
    out[outlen + 3 + 7] = 0;
    putBe16(out + 3, outlen + 3 + 3);
    return outlen + 3 + 8;
}

} // namespace

int handleSdpRequest(const std::uint8_t* req, std::uint32_t reqlen, std::uint8_t* out, int outcap)
{
    if (static_cast<int>(reqlen) < 5 || outcap < 64)
    {
        return 0;
    }

    const std::uint32_t plen = getBe16(req + 3);

    if (reqlen < plen + 5)
    {
        return errorResponse(out, req);
    }
    switch (req[0])
    {
    case 0x02:
        return serviceSearch(req, plen, out);
    case 0x04:
        return serviceAttribute(req, plen, out, outcap);
    case 0x06:
        return serviceSearchAttribute(req, plen, out, outcap);
    default:
        logLine("sdp: unhandled PDU %#04x", req[0]);
        return errorResponse(out, req);
    }
}

} // namespace bluev
