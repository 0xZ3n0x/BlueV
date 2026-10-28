// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/bluetooth/usb_layout.hpp"

namespace bluev
{

namespace
{

constexpr std::uint8_t kInterfaceType = 4;
constexpr std::uint8_t kEndpointType = 5;
constexpr int kInterfaceLength = 9;
constexpr int kEndpointLength = 7;

// the endpoints found so far in the interface that is being read
struct Endpoints
{
    int events = -1;     // first interrupt in
    int aclIn = -1;      // first bulk in
    int aclOut = -1;     // first bulk out
    int sameNumber = -1; // the bulk out with the number of the bulk in

    bool complete() const
    {
        return events >= 0 && aclIn >= 0 && (sameNumber >= 0 || aclOut >= 0);
    }
};

} // namespace

bool findHciLayout(const std::uint8_t* config, int length, HciLayout& out)
{
    int interfaceNumber = -1; // of the Bluetooth interface being read, -1 = none
    int bulkOuts[8];
    int bulkOutCount = 0;
    Endpoints endpoints;
    bool matched = false;

    // Closes the interface that was being read: true when it was the Bluetooth
    // one and had everything.
    const auto finish = [&]() {
        if (interfaceNumber < 0)
        {
            return false;
        }
        for (int i = 0; i < bulkOutCount && endpoints.aclIn >= 0; i++)
        {
            if ((bulkOuts[i] & 0x0f) == (endpoints.aclIn & 0x0f))
            {
                endpoints.sameNumber = bulkOuts[i];
                break;
            }
        }
        if (!endpoints.complete())
        {
            return false;
        }
        out.interface = static_cast<std::uint8_t>(interfaceNumber);
        out.events = static_cast<std::uint8_t>(endpoints.events);
        out.aclIn = static_cast<std::uint8_t>(endpoints.aclIn);
        out.aclOut = static_cast<std::uint8_t>(endpoints.sameNumber >= 0 ? endpoints.sameNumber : endpoints.aclOut);
        return true;
    };

    for (int offset = 0; offset + 2 <= length;)
    {
        const int size = config[offset];
        const std::uint8_t type = config[offset + 1];

        if (size < 2 || offset + size > length)
        {
            break; // malformed or cut short: what was read so far is all there is
        }
        if (type == kInterfaceType && size >= kInterfaceLength)
        {
            if (finish())
            {
                matched = true;
                break;
            }
            const bool bluetooth = config[offset + 3] == 0 && config[offset + 5] == 0xe0 && config[offset + 6] == 1 &&
                                   config[offset + 7] == 1;

            interfaceNumber = bluetooth ? config[offset + 2] : -1;
            endpoints = Endpoints{};
            bulkOutCount = 0;
        }
        else if (type == kEndpointType && size >= kEndpointLength && interfaceNumber >= 0)
        {
            const int address = config[offset + 2];
            const int kind = config[offset + 3] & 3; // 2 = bulk, 3 = interrupt
            const bool in = (address & 0x80) != 0;

            if (kind == 3 && in && endpoints.events < 0)
            {
                endpoints.events = address;
            }
            else if (kind == 2 && in && endpoints.aclIn < 0)
            {
                endpoints.aclIn = address;
            }
            else if (kind == 2 && !in)
            {
                if (endpoints.aclOut < 0)
                {
                    endpoints.aclOut = address;
                }
                if (bulkOutCount < 8)
                {
                    bulkOuts[bulkOutCount++] = address;
                }
            }
        }
        offset += size;
    }
    return matched || finish();
}

} // namespace bluev
