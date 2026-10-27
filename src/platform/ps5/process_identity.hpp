// SPDX-License-Identifier: GPL-3.0-or-later
// Finding a running copy of this payload the way elfldr and friends do:
// name the process, then look it up in the kernel's process list, instead of
// probing it over HTTP. Works whether or not its web server has come up yet.
#pragma once

#include <cstdint>
#include <sys/types.h>

namespace bluev
{

// Gives the calling thread (and, as the only thread at this point, the
// process) a name another copy of this payload can find later. Call once,
// early, before anything could come looking for it.
void setProcessName(const char* name);

// Without this, the kernel refuses both the process list this payload is
// found in (findOtherInstance) and a signal sent to another process
// (kill()): elfldr injects it with no privilege for either. The same
// authid ftpsrv asks for itself to reach its own device nodes also covers
// this. Needed only around the lookup+signal below; returns the authid we
// had before, so restoreCredentials() can put it back once that is done.
std::uint64_t allowFindingOtherInstances();

// Undoes allowFindingOtherInstances(): sets the authid back to what
// `original` (its return value) was.
void restoreCredentials(std::uint64_t original);

// The pid of another running process with this name, or -1 when there is
// none (including when the only one is us).
pid_t findOtherInstance(const char* name);

} // namespace bluev
