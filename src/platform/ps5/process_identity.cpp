// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/ps5/process_identity.hpp"

#include <csignal>
#include <cstdint>
#include <cstring>
#include <vector>

#include <ps5/kernel.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#include <unistd.h>

namespace bluev
{

namespace
{
// What ftpsrv asks for itself (see the project notes): broad enough to also
// unlock sysctl(KERN_PROC_PROC) visibility and kill() across processes for
// an elfldr-injected, otherwise unprivileged process.
constexpr std::uint64_t kAuthId = 0x4801000000000013ULL;

pid_t findOtherInstance(const char* name)
{
    int mib[3] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC};
    std::size_t size = 0;

    if (sysctl(mib, 3, nullptr, &size, nullptr, 0) != 0 || size == 0)
    {
        return -1;
    }
    // another process can appear between the sizing call and the real one
    size += size / 4 + sizeof(struct kinfo_proc);
    std::vector<unsigned char> buffer(size);
    if (sysctl(mib, 3, buffer.data(), &size, nullptr, 0) != 0)
    {
        return -1;
    }

    const pid_t self = getpid();
    std::size_t offset = 0;

    // Records are not a fixed-size array: each carries its own size
    // (ki_structsize), which can differ between kernel builds, so step
    // through the buffer by that instead of sizeof(kinfo_proc) — walking it
    // as a fixed-stride array walks off into the next record's fields once
    // a single entry's real size differs from what was compiled here.
    while (offset + sizeof(int) <= size)
    {
        const auto* proc = reinterpret_cast<const struct kinfo_proc*>(buffer.data() + offset);
        const int entrySize = proc->ki_structsize;

        if (entrySize <= 0 || offset + static_cast<std::size_t>(entrySize) > size)
        {
            break;
        }
        // elfldr names the injected process after the uploaded ELF itself
        // (ki_comm); thr_set_name()'s own ki_tdname does not reflect it on
        // this kernel, so ki_comm is what is compared against here.
        if (proc->ki_pid != self && std::strcmp(proc->ki_comm, name) == 0)
        {
            return proc->ki_pid;
        }
        offset += static_cast<std::size_t>(entrySize);
    }
    return -1;
}

} // namespace

pid_t askOtherInstanceToQuit(const char* name)
{
    const std::uint64_t original = kernel_get_ucred_authid(getpid());

    kernel_set_ucred_authid(getpid(), kAuthId);
    const pid_t other = findOtherInstance(name);

    if (other >= 0)
    {
        kill(other, SIGTERM);
    }
    kernel_set_ucred_authid(getpid(), original);
    return other;
}

} // namespace bluev
