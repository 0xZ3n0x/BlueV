// SPDX-License-Identifier: GPL-3.0-or-later
#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "capture_sink.hpp"
#include "core/bluetooth/sdp_server.hpp"

using namespace bluev;

namespace
{

std::vector<std::uint8_t> request(std::uint8_t pdu, const std::vector<std::uint8_t>& params, std::uint8_t tid = 0x12)
{
    std::vector<std::uint8_t> r = {pdu, 0x00, tid, static_cast<std::uint8_t>(params.size() >> 8),
                                   static_cast<std::uint8_t>(params.size())};
    r.insert(r.end(), params.begin(), params.end());
    return r;
}

// pattern: sequence with one 16 bit UUID, maximum record count 2, no continuation
std::vector<std::uint8_t> searchFor(std::uint16_t uuid)
{
    return {0x35, 0x03, 0x19, static_cast<std::uint8_t>(uuid >> 8), static_cast<std::uint8_t>(uuid), 0x00, 0x02, 0x00};
}

int ask(const std::vector<std::uint8_t>& req, std::uint8_t* out, int cap = 512)
{
    return handleSdpRequest(req.data(), static_cast<std::uint32_t>(req.size()), out, cap);
}

} // namespace

TEST(SdpServer, SearchForTheAudioSourceFindsOneRecord)
{
    std::uint8_t out[512];
    bluev::testing::CaptureSink sink;
    const int n = ask(request(0x02, searchFor(0x110a)), out);

    ASSERT_EQ(14, n);
    EXPECT_EQ(0x03, out[0]); // service search response
    EXPECT_EQ(0x12, out[2]); // the transaction id comes back
    EXPECT_EQ(1, out[6]);    // one record
    EXPECT_EQ(0x00, out[9]);
    EXPECT_EQ(0x01, out[10]);
    EXPECT_EQ(0x00, out[11]);
    EXPECT_EQ(0x01, out[12]); // handle 0x00010001
    EXPECT_TRUE(sink.has("service search -> 1 record"));
}

TEST(SdpServer, EveryA2dpRelatedUuidMatches)
{
    std::uint8_t out[512];
    for (std::uint16_t uuid : {0x110a, 0x110d, 0x0019})
    {
        ASSERT_EQ(14, ask(request(0x02, searchFor(uuid)), out)) << std::hex << uuid;
        EXPECT_EQ(1, out[6]);
    }
}

TEST(SdpServer, SearchForTheRemoteControlFindsItsOwnRecord)
{
    std::uint8_t out[512];

    for (std::uint16_t uuid : {0x110e, 0x0017})
    {
        ASSERT_EQ(14, ask(request(0x02, searchFor(uuid)), out)) << std::hex << uuid;
        EXPECT_EQ(1, out[6]);
        EXPECT_EQ(0x02, out[12]); // handle 0x00010002
    }
}

TEST(SdpServer, SearchForWhatBothShareFindsBothRecords)
{
    std::uint8_t out[512];

    for (std::uint16_t uuid : {0x0100, 0x1002})
    {
        ASSERT_EQ(18, ask(request(0x02, searchFor(uuid)), out)) << std::hex << uuid;
        EXPECT_EQ(2, out[6]);
        EXPECT_EQ(0x01, out[12]);
        EXPECT_EQ(0x02, out[16]);
    }
}

TEST(SdpServer, SearchForSomethingElseFindsNothing)
{
    std::uint8_t out[512];
    ASSERT_EQ(10, ask(request(0x02, searchFor(0x1108)), out)); // headset profile
    EXPECT_EQ(0, out[6]);
}

TEST(SdpServer, AttributeRequestReturnsTheRecordWithTheName)
{
    std::uint8_t out[512];
    // handle 0x00010001, maximum 0xffff bytes, attribute range 0x0000..0xffff
    const std::vector<std::uint8_t> params = {0x00, 0x01, 0x00, 0x01, 0xff, 0xff, 0x35,
                                              0x05, 0x0a, 0x00, 0x00, 0xff, 0xff, 0x00};
    const int n = ask(request(0x04, params), out);

    ASSERT_GT(n, 20);
    EXPECT_EQ(0x05, out[0]); // attribute response
    EXPECT_EQ(0x36, out[7]); // an attribute list sequence
    const std::string body(reinterpret_cast<char*>(out), static_cast<std::size_t>(n));
    EXPECT_NE(std::string::npos, body.find("BlueV"));                                // the service name
    EXPECT_NE(std::string::npos, body.find(std::string("\x35\x03\x19\x11\x0a", 5))); // class: audio source
}

TEST(SdpServer, UnknownRecordHandleIsAnError)
{
    std::uint8_t out[512];
    const std::vector<std::uint8_t> params = {0x00, 0x01, 0x00, 0x99, 0xff, 0xff, 0x35, 0x00, 0x00};
    ASSERT_EQ(7, ask(request(0x04, params), out));
    EXPECT_EQ(0x01, out[0]); // error response
    EXPECT_EQ(0x02, out[6]); // invalid service record handle
}

TEST(SdpServer, SearchAttributeAnswersOneRecordOrAnEmptyList)
{
    std::uint8_t out[512];
    std::vector<std::uint8_t> params = searchFor(0x110a);
    params.pop_back(); // maximum bytes: 0x00 0x02 -> make it two bytes
    params = {0x35, 0x03, 0x19, 0x11, 0x0a, 0xff, 0xff, 0x35, 0x05, 0x0a, 0x00, 0x00, 0xff, 0xff, 0x00};
    const int found = ask(request(0x06, params), out);

    ASSERT_GT(found, 20);
    EXPECT_EQ(0x07, out[0]);

    params[3] = 0x11; // 0x1108 does not match
    params[4] = 0x08;
    const int none = ask(request(0x06, params), out);
    ASSERT_EQ(11, none);
    EXPECT_EQ(0x36, out[7]); // an empty list
    EXPECT_EQ(0, out[9]);
    EXPECT_EQ(0, out[10]);
}

TEST(SdpServer, SearchAttributeForTheRemoteControlReturnsItsRecord)
{
    std::uint8_t out[512];
    const std::vector<std::uint8_t> params = {0x35, 0x03, 0x19, 0x11, 0x0e, 0xff, 0xff, 0x35,
                                              0x05, 0x0a, 0x00, 0x00, 0xff, 0xff, 0x00};
    const int n = ask(request(0x06, params), out);

    ASSERT_GT(n, 20);
    EXPECT_EQ(0x07, out[0]);
    const std::string body(reinterpret_cast<char*>(out), static_cast<std::size_t>(n));
    EXPECT_NE(std::string::npos, body.find(std::string("\x35\x03\x19\x11\x0e", 5))); // class: remote control
    EXPECT_NE(std::string::npos, body.find(std::string("\x09\x00\x17", 3)));         // l2cap psm 0x17
    EXPECT_EQ(std::string::npos, body.find(std::string("\x35\x03\x19\x11\x0a", 5))); // not the audio source
}

TEST(SdpServer, SearchAttributeForBothRecordsListsBoth)
{
    std::uint8_t out[512];
    const std::vector<std::uint8_t> params = {0x35, 0x03, 0x19, 0x10, 0x02, 0xff, 0xff, 0x35,
                                              0x05, 0x0a, 0x00, 0x00, 0xff, 0xff, 0x00};
    const int n = ask(request(0x06, params), out);

    ASSERT_GT(n, 40);
    const std::string body(reinterpret_cast<char*>(out), static_cast<std::size_t>(n));
    EXPECT_NE(std::string::npos, body.find(std::string("\x35\x03\x19\x11\x0a", 5)));
    EXPECT_NE(std::string::npos, body.find(std::string("\x35\x03\x19\x11\x0e", 5)));
    EXPECT_EQ(static_cast<int>(out[8] << 8 | out[9]) + 10,
              n - 1); // the outer list covers both, then the continuation byte
}

TEST(SdpServer, ShortAndMalformedRequests)
{
    std::uint8_t out[512];
    EXPECT_EQ(0, handleSdpRequest(reinterpret_cast<const std::uint8_t*>("\x02\x00"), 2, out, 512));
    EXPECT_EQ(0, ask(request(0x02, searchFor(0x110a)), out, 32)); // no room for an answer

    // the length field promises more than there is
    std::vector<std::uint8_t> lie = {0x02, 0x00, 0x01, 0x00, 0x20, 0x35};
    ASSERT_EQ(7, ask(lie, out));
    EXPECT_EQ(0x01, out[0]);
    EXPECT_EQ(0x03, out[6]); // invalid continuation state

    // a pattern that is not a sequence of UUIDs
    ASSERT_EQ(7, ask(request(0x02, {0x09, 0x00, 0x01, 0x00, 0x02, 0x00}), out));
    EXPECT_EQ(0x01, out[0]);
}

TEST(SdpServer, UnknownPduIsAnError)
{
    std::uint8_t out[512];
    bluev::testing::CaptureSink sink;
    ASSERT_EQ(7, ask(request(0x0a, {0x00}), out));
    EXPECT_EQ(0x01, out[0]);
    EXPECT_TRUE(sink.has("unhandled PDU 0x0a"));
}
