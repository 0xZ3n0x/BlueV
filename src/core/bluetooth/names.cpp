// SPDX-License-Identifier: GPL-3.0-or-later
// Names for the numbers in the log: HCI status codes, opcodes, device classes,
// controller makers and versions.
#include "core/bluetooth/names.hpp"

#include <cstddef>

namespace bluev
{

const char* hciStatusName(unsigned code)
{
    switch (code)
    {
    case 0x00:
        return "success";
    case 0x01:
        return "unknown command";
    case 0x02:
        return "unknown connection";
    case 0x03:
        return "hardware failure";
    case 0x04:
        return "page timeout, the device did not answer";
    case 0x05:
        return "authentication failure";
    case 0x06:
        return "PIN or key missing";
    case 0x07:
        return "out of memory";
    case 0x08:
        return "connection timeout";
    case 0x09:
        return "too many connections";
    case 0x0b:
        return "a connection to this device already exists";
    case 0x0c:
        return "command not allowed now";
    case 0x0d:
        return "rejected, limited resources";
    case 0x0e:
        return "rejected, security reasons";
    case 0x0f:
        return "rejected, unacceptable address";
    case 0x10:
        return "connection accept timeout";
    case 0x11:
        return "unsupported feature or parameter";
    case 0x12:
        return "invalid parameters";
    case 0x13:
        return "the device ended the connection";
    case 0x14:
        return "the device ended it, low resources";
    case 0x15:
        return "the device ended it, power off";
    case 0x16:
        return "ended by us";
    case 0x17:
        return "repeated attempts";
    case 0x18:
        return "pairing not allowed";
    case 0x1a:
        return "unsupported remote feature";
    case 0x1f:
        return "unspecified error";
    case 0x22:
        return "LMP response timeout";
    case 0x23:
        return "LMP error transaction collision";
    case 0x24:
        return "LMP PDU not allowed";
    case 0x25:
        return "encryption mode not acceptable";
    case 0x29:
        return "pairing with a unit key not supported";
    case 0x3d:
        return "connection ended, MIC failure";
    case 0x3e:
        return "connection failed to be established";
    }
    return "unknown reason";
}

const char* hciOpcodeName(std::uint16_t op)
{
    switch (op)
    {
    case 0x0401:
        return "Inquiry";
    case 0x0402:
        return "Inquiry Cancel";
    case 0x0405:
        return "Create Connection";
    case 0x0406:
        return "Disconnect";
    case 0x0408:
        return "Create Connection Cancel";
    case 0x0409:
        return "Accept Connection Request";
    case 0x040a:
        return "Reject Connection Request";
    case 0x040b:
        return "Link Key Request Reply";
    case 0x040c:
        return "Link Key Request Negative Reply";
    case 0x040d:
        return "PIN Code Request Reply";
    case 0x0411:
        return "Authentication Requested";
    case 0x0413:
        return "Set Connection Encryption";
    case 0x0419:
        return "Remote Name Request";
    case 0x042b:
        return "IO Capability Request Reply";
    case 0x042c:
        return "User Confirmation Request Reply";
    case 0x0801:
        return "Hold Mode";
    case 0x0803:
        return "Sniff Mode";
    case 0x0804:
        return "Exit Sniff Mode";
    case 0x0809:
        return "Role Discovery";
    case 0x080b:
        return "Switch Role";
    case 0x080d:
        return "Write Link Policy Settings";
    case 0x080f:
        return "Write Default Link Policy Settings";
    case 0x0c01:
        return "Set Event Mask";
    case 0x0c03:
        return "Reset";
    case 0x0c05:
        return "Set Event Filter";
    case 0x0c13:
        return "Write Local Name";
    case 0x0c17:
        return "Read Page Timeout";
    case 0x0c18:
        return "Write Page Timeout";
    case 0x0c19:
        return "Read Scan Enable";
    case 0x0c1a:
        return "Write Scan Enable";
    case 0x0c1c:
        return "Write Page Scan Activity";
    case 0x0c1e:
        return "Write Inquiry Scan Activity";
    case 0x0c24:
        return "Write Class of Device";
    case 0x0c45:
        return "Write Inquiry Mode";
    case 0x0c47:
        return "Write Page Scan Type";
    case 0x0c52:
        return "Write Extended Inquiry Response";
    case 0x0c56:
        return "Write Simple Pairing Mode";
    case 0x1001:
        return "Read Local Version";
    case 0x1003:
        return "Read Local Features";
    case 0x1005:
        return "Read Buffer Size";
    case 0x1009:
        return "Read BD_ADDR";
    case 0x1405:
        return "Read RSSI";
    }
    return "command";
}

/* the major (and for peripherals the minor) class of a Class of Device */
const char* deviceClassName(std::uint32_t cod)
{
    switch ((cod >> 8) & 0x1f)
    {
    case 0x01:
        return "computer";
    case 0x02:
        return "phone";
    case 0x03:
        return "network access point";
    case 0x04:
        return "audio/video";
    case 0x05:
        switch ((cod >> 2) & 0x0f)
        {
        case 0x02:
            return "gamepad";
        case 0x01:
            return "joystick";
        default:
            break;
        }
        if ((cod >> 6) & 1)
        {
            return "keyboard";
        }
        return "peripheral";
    case 0x06:
        return "imaging";
    case 0x07:
        return "wearable";
    case 0x08:
        return "toy";
    case 0x09:
        return "health";
    }
    return "other";
}

const char* manufacturerName(unsigned id)
{
    switch (id)
    {
    case 0x0002:
        return "Intel";
    case 0x000a:
        return "CSR";
    case 0x000d:
        return "Texas Instruments";
    case 0x000f:
        return "Broadcom";
    case 0x001d:
        return "Qualcomm";
    case 0x0046:
        return "MediaTek";
    case 0x0048:
        return "Marvell";
    case 0x004c:
        return "Apple";
    case 0x005d:
        return "Realtek";
    }
    return "unknown";
}

const char* hciVersionName(unsigned version)
{
    static const char* const names[] = {"1.0b", "1.1", "1.2", "2.0", "2.1", "3.0", "4.0",
                                        "4.1",  "4.2", "5.0", "5.1", "5.2", "5.3", "5.4"};

    return version < sizeof names / sizeof names[0] ? names[version] : "newer than 5.4";
}

} // namespace bluev
