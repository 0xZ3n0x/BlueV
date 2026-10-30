// SPDX-License-Identifier: GPL-3.0-or-later
// libSceAvcap2, the console's audio capture (what Remote Play uses). The
// library is loaded at run time and the entry points are resolved by NID, so
// the payload stays self-contained.
#pragma once

#include <cstdint>

#include "core/ports/avcap.hpp"

namespace bluev
{

class AvcapLibrary final : public IAvcap2
{
  public:
    // Loads the library and finds its entry points; false, with the reason in
    // the log, when that does not work.
    bool load();

    int initialize() override;
    int terminate() override;
    int openAudio(void** handle) override;
    int close(void* handle) override;
    int start(void* handle) override;
    int stop(void* handle) override;
    int readAudio(void* handle, float* buffer, std::uint32_t maxBytes) override;

  private:
    int (*m_initialize)() = nullptr;
    int (*m_terminate)() = nullptr;
    int (*m_openAudio)(void** handle, void* param, std::uint64_t flags) = nullptr;
    int (*m_close)(void* handle) = nullptr;
    int (*m_start)(void* handle) = nullptr;
    int (*m_stop)(void* handle) = nullptr;
    int (*m_readAudio)(void* handle, void* rec, std::uint32_t max, void* meta, std::uint32_t flag) = nullptr;
};

} // namespace bluev
