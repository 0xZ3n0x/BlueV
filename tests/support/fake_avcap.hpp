// SPDX-License-Identifier: GPL-3.0-or-later
// libSceAvcap2 as a test plays it: every call is noted and answers with the
// number the test set.
#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "core/ports/avcap.hpp"

namespace bluev::testing
{

class FakeAvcap final : public IAvcap2
{
  public:
    // ---- what it answers ----
    int initializeResult = 0;
    int openResult = 0;
    bool openGivesNoHandle = false; // OpenAudio says 0 but leaves the handle empty
    int startResult = 0;
    int readResult = 16;   // used when `reads` is empty
    std::deque<int> reads; // answers for the next reads, first in first out
    float fill = 0.5f;     // what a successful read writes into the buffer

    // ---- what it saw ----
    std::vector<std::string> calls; // "initialize", "open", "start", "stop", "close", "terminate"
    void* lastReadHandle = nullptr;
    std::uint32_t lastReadMax = 0;
    float* lastReadBuffer = nullptr;
    int handleToken = 0; // its address is the handle we give out

    int initialize() override
    {
        calls.push_back("initialize");
        return initializeResult;
    }
    int terminate() override
    {
        calls.push_back("terminate");
        return 0;
    }
    int openAudio(void** handle) override
    {
        calls.push_back("open");
        if (openResult >= 0 && !openGivesNoHandle)
        {
            *handle = &handleToken;
        }
        return openResult;
    }
    int close(void* handle) override
    {
        calls.push_back(handle == &handleToken ? "close" : "close?");
        return 0;
    }
    int start(void* handle) override
    {
        calls.push_back(handle == &handleToken ? "start" : "start?");
        return startResult;
    }
    int stop(void* handle) override
    {
        calls.push_back(handle == &handleToken ? "stop" : "stop?");
        return 0;
    }
    int readAudio(void* handle, float* buffer, std::uint32_t maxBytes) override
    {
        lastReadHandle = handle;
        lastReadBuffer = buffer;
        lastReadMax = maxBytes;
        int r = readResult;

        if (!reads.empty())
        {
            r = reads.front();
            reads.pop_front();
        }
        if (r > 0)
        {
            buffer[0] = fill;
        }
        return r;
    }

    // how many times `call` happened
    int count(const std::string& call) const
    {
        int n = 0;

        for (const std::string& c : calls)
        {
            n += c == call ? 1 : 0;
        }
        return n;
    }
};

} // namespace bluev::testing
