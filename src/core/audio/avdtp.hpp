// SPDX-License-Identifier: GPL-3.0-or-later
// AVDTP signalling messages: building and reading the few we use.
#pragma once

#include <cstddef>
#include <cstdint>

namespace bluev::avdtp
{

enum Signal : std::uint8_t
{
    kDiscover = 1,
    kGetCapabilities = 2,
    kSetConfiguration = 3,
    kOpen = 6,
    kStart = 7,
    kClose = 8,
};

enum MessageType : std::uint8_t
{
    kCommand = 0,
    kAccept = 2,
    kReject = 3
};

struct Header
{
    std::uint8_t label = 0; // transaction label, 0..15
    std::uint8_t type = 0;  // MessageType
    std::uint8_t signal = 0;
};

// False when the frame is too short or a fragment of a longer message.
bool parseHeader(const std::uint8_t* frame, int length, Header& out);

// A command: label in the high nibble, single packet, then the signal.
std::size_t buildCommand(std::uint8_t label, std::uint8_t signal, const std::uint8_t* params, int length,
                         std::uint8_t* out);
std::size_t buildAccept(std::uint8_t label, std::uint8_t signal, std::uint8_t* out);
std::size_t buildReject(std::uint8_t label, std::uint8_t signal, std::uint8_t error, std::uint8_t* out);
// Answer to Discover: one SBC source endpoint, seid 1, not in use.
std::size_t buildDiscoverAccept(std::uint8_t label, std::uint8_t* out);
// Answer to Get Capabilities: media transport and SBC.
std::size_t buildCapabilitiesAccept(std::uint8_t label, std::uint8_t* out);

// What a Set Configuration command carries.
struct PeerConfig
{
    std::uint8_t seid = 0; // the device's own endpoint, as the byte it sent (seid << 2)
    bool haveSbc = false;
    std::uint32_t caps = 0; // the SBC element, see chooseA2dpParams
};
bool parseSetConfiguration(const std::uint8_t* frame, int length, PeerConfig& out);

// One endpoint of a Discover response.
struct Endpoint
{
    std::uint8_t seid = 0;
    bool isSink = false;
};
// Reads up to `max` endpoints from a Discover response; returns how many.
int parseEndpoints(const std::uint8_t* response, int length, Endpoint* out, int max);

// The SBC element of a Get Capabilities response; false when there is none.
bool findSbcCapabilities(const std::uint8_t* response, int length, std::uint32_t& caps);

} // namespace bluev::avdtp
