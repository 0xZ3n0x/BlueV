// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/ps5/console_model.hpp"

#include "platform/ps5/ps5_api.hpp"

namespace bluev
{

std::string consoleModelName()
{
    char model[1000] = "";

    if (sceKernelGetHwModelName(model) != 0)
    {
        model[0] = 0;
    }
    return model;
}

} // namespace bluev
