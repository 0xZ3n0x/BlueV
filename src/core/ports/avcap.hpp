// SPDX-License-Identifier: GPL-3.0-or-later
// The console's audio capture library (libSceAvcap2) as the capture uses it.
// The real one calls the library; a test plays it and answers with the
// numbers the library would. Every call returns the library's own result: a
// negative number is an error.
#pragma once

#include <cstdint>

namespace bluev
{

class IAvcap2
{
  public:
    virtual ~IAvcap2() = default;

    virtual int initialize() = 0;
    virtual int terminate() = 0;
    // Opens the capture of the whole mix, stereo; `handle` is set on success.
    virtual int openAudio(void** handle) = 0;
    virtual int close(void* handle) = 0;
    virtual int start(void* handle) = 0;
    virtual int stop(void* handle) = 0;
    // Reads one record into `buffer`: the bytes read, or an error number.
    virtual int readAudio(void* handle, float* buffer, std::uint32_t maxBytes) = 0;
};

} // namespace bluev
