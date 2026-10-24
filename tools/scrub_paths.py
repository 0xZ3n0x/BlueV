#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Removes the paths of the machine that built a binary from it.

usage: scrub_paths.py file.elf

Some of the SDK's prebuilt libraries (libc++abi, libunwind) carry the path of the
directory they were built in inside their assertion messages, and a build can
leave the user's home directory behind. Each such path is overwritten with a
neutral text of the same length, so nothing in the file moves. The script
fails when something of this machine is still in the file afterwards: the home
directory, the user name between slashes, or the directory of this repository.
"""
import getpass
import os
import re
import sys

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# a directory prefix, up to and including its last slash
PATTERNS = [
    rb"/tmp/[A-Za-z0-9_.-]+/tmp\.[A-Za-z0-9]+/",
    rb"/home/[^/\0\s]+/",
    rb"/Users/[^/\0\s]+/",
]


def neutral(length):
    # "/build" plus filler, same length, still a directory prefix
    return (b"/build" + b"_" * (length - 7) + b"/")[:length]


def forbidden():
    names = {os.path.expanduser("~"), HERE}
    user = getpass.getuser()
    found = [n.encode() for n in names if n and n != "/"]
    found.append(b"/" + user.encode() + b"/")
    found.append(b"/tmp/")
    return found


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    path = sys.argv[1]
    data = open(path, "rb").read()
    count = 0

    for pattern in PATTERNS:
        data, replaced = re.subn(pattern, lambda match: neutral(len(match.group(0))), data)
        count += replaced
    open(path, "wb").write(data)
    print(f"{path}: {count} path(s) replaced")

    left = [f.decode() for f in forbidden() if f in data]
    if left:
        sys.exit(f"{path}: still contains {left}")


if __name__ == "__main__":
    main()
