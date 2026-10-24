// SPDX-License-Identifier: GPL-3.0-or-later
// The way HCI packets get to and from a controller. The real one drives a
// USB device; a test plays the controller.
#pragma once

#include <cstdint>
#include <string>

namespace bluev
{

// Which endpoint is which, and the interface the commands go to.
struct HciLayout
{
    std::uint8_t interface = 0;
    std::uint8_t events = 0x81; // interrupt in
    std::uint8_t aclIn = 0x82;  // bulk in
    std::uint8_t aclOut = 0x02; // bulk out
};

class IHciTransport
{
  public:
    virtual ~IHciTransport() = default;

    // Choose the controller before open(): a name under /dev, or nullptr for
    // the console's own chip, and the layout of its endpoints.
    virtual void select(const char* node, const HciLayout& layout) = 0;
    virtual bool open() = 0;
    virtual void close() = 0;

    // Send a command (opcode, then its parameters).
    // False when the command could not be handed to the controller.
    virtual bool command(std::uint16_t opcode, const std::uint8_t* params, std::uint8_t length) = 0;

    // Let the transport work for at most `timeoutMs`; true when a packet is
    // waiting. This is where time passes.
    virtual bool pump(std::uint32_t timeoutMs) = 0;

    // The next whole packet, copied to `out` (at most `max` bytes); the
    // length, or 0 when there is none. An event starts with its code, an ACL
    // packet with the handle.
    virtual int nextEvent(std::uint8_t* out, int max) = 0;
    virtual int nextAcl(std::uint8_t* out, int max) = 0;

    // One ACL packet: HCI ACL header, then the data. False when it was not accepted.
    virtual bool sendAcl(const std::uint8_t* packet, std::uint32_t length) = 0;

    // Find out which endpoint carries the events when the layout is not
    // known: 1 = as selected, 2 = events on the second IN pipe (select the
    // standard layout and open again), 0 = no answer or not supported.
    virtual int detectRoles(std::uint32_t timeoutMs) = 0;

    // Counters for the log after a failure.
    virtual std::string describe(const char* why) = 0;
};

} // namespace bluev
