// SPDX-License-Identifier: GPL-3.0-or-later
// How a newer copy of this payload tells an older one to step aside: not
// over HTTP (see platform/ps5/process_identity.hpp for how it is found),
// but with SIGTERM — the conventional "please end" signal, so a plain
// `kill`/`pkill` against this process name also takes the peaceful path
// below instead of its default disposition (terminate at once). Works even
// before the older copy's web server has come up.
#pragma once

namespace bluev
{

// Arms the SIGTERM handler. Call once, early, before anything could come
// looking for this process by name.
void installQuitSignalHandling();

// True once, the first time it is called after a SIGTERM landed since the
// last call. Safe to poll from the service loop.
bool takeQuitRequestedFlag();

} // namespace bluev
