// SPDX-License-Identifier: GPL-3.0-or-later
// Finding a running copy of this payload the way elfldr and friends do: look
// it up by name in the kernel's process list, instead of probing it over HTTP.
// Works whether or not its web server has come up yet. The process names
// itself with thr_set_name (see main).
#pragma once

#include <sys/types.h>

namespace bluev
{

// Looks for another running process with this name and sends it SIGTERM.
// Returns its pid, or -1 when there is none (including when the only one is
// us). The kernel refuses both the process list and a signal to another
// process to a payload that elfldr injected without privilege, so the
// authid ftpsrv asks for itself is taken for the lookup and the signal and
// put back before returning.
pid_t askOtherInstanceToQuit(const char* name);

} // namespace bluev
