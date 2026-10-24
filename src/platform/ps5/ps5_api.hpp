// SPDX-License-Identifier: GPL-3.0-or-later
// Declarations of system functions the console exports that the payload SDK
// headers lack, and the SDK's own dynamic library lookups (kernel_dynlib_*).
// The platform adapters load libraries at run time with these.
#pragma once

#include <cstdint>
#include <sys/types.h>

#include <ps5/kernel.h>

extern "C"
{
    // libkernel (the SDK stub we link is libkernel_sys)
    int sceKernelLoadStartModule(const char* path, int argc, const char* argv, int flags, int* unk, int* unk2);
    int sceKernelGetHwModelName(char* name);
    long sceKernelSendNotificationRequest(int type, void* buf, long size, int unk);
}
