// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/ps5/notification.hpp"

#include <cstdio>
#include <cstring>

#include "platform/ps5/ps5_api.hpp"

namespace bluev
{

namespace
{

// A "notification with user message" request: 45 bytes the system does not
// need, then the UTF-8 text (up to 3075 bytes with its NUL). The same layout
// as the notify_request_t of the SDK's notify_debug sample.
struct NotifyRequest
{
    char unused[45];
    char message[3075];
};

} // namespace

void sendNotification(const std::string& text)
{
    NotifyRequest request;

    std::memset(&request, 0, sizeof request);
    std::snprintf(request.message, sizeof request.message, "%s", text.c_str());
    sceKernelSendNotificationRequest(0, &request, sizeof request, 0);
}

} // namespace bluev
