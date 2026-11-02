// SPDX-License-Identifier: GPL-3.0-or-later
// A USB device as the HCI transport uses it: a table of asynchronous
// transfers on its endpoints, each with a buffer of its own, plus control
// requests. The real one is the kernel's ugen driver; a test plays the kernel.
//
// A transfer is opened once, then started, and later reported as complete
// (with its status and how much arrived); a started transfer owns its buffer
// until it is complete or stopped.
#pragma once

#include <cstdint>

namespace bluev
{

constexpr int kUsbTransfers = 65;        // the size of the transfer table
constexpr int kUsbTransferBytes = 0x44c; // the buffer of every transfer

struct UsbTransferResult
{
    int status = 0;           // 0 = done, anything else an error
    std::uint32_t frames = 0; // frames that were transferred; 0 = nothing came
    std::uint32_t length = 0; // bytes in the buffer
};

class IUsbDevice
{
  public:
    virtual ~IUsbDevice() = default;

    // Opens the device node (a name under /dev) and sets up a table of
    // `transfers` transfers. The transfer with index `forceShort` is written
    // with the short-packet flag (the one for bulk out). False, with the
    // reason in the log, when the node cannot be opened or set up.
    virtual bool open(const char* node, int transfers, int forceShort) = 0;
    // Takes the table down and closes the node. The transfers still running
    // must have been stopped first.
    virtual void close() = 0;

    // Gives transfer `index` an endpoint address and a buffer size.
    virtual bool openTransfer(int index, std::uint8_t endpoint, std::uint32_t length) = 0;
    // The buffer of transfer `index`, kUsbTransferBytes long.
    virtual std::uint8_t* buffer(int index) = 0;
    // Starts transfer `index` with `length` bytes of its buffer.
    virtual bool start(int index, std::uint32_t length) = 0;
    virtual bool stop(int index) = 0;
    // The index of a transfer that has finished, or -1 when none has.
    virtual int complete() = 0;
    virtual UsbTransferResult result(int index) = 0;

    // A control request to the class of `interface`: `data` is the HCI
    // command packet (opcode, length, parameters).
    virtual bool control(std::uint8_t interface, const std::uint8_t* data, std::uint16_t length) = 0;

    // Lets about `microseconds` pass; the only place a transport waits.
    virtual void wait(std::uint32_t microseconds) = 0;
    // errno of the call that failed last, for the log.
    virtual int lastError() const = 0;
};

} // namespace bluev
