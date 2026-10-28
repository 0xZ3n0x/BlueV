#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Host tests. usage: tests/run.sh [thread|address|plain] [gtest filter]
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
MODE=${1:-plain}
FILTER=${2:-*}
case "$MODE" in plain) SAN="" ;; *) SAN="$MODE" ;; esac
OUT=${BLUEV_BUILD_DIR:-/tmp/bluev-host-$MODE}
cmake -S "$HERE" -B "$OUT" -G Ninja -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang \
    -DBLUEV_SANITIZE="$SAN" > "$OUT.configure.log" 2>&1 || { cat "$OUT.configure.log"; exit 1; }
cmake --build "$OUT" 2>&1 | grep -E "error|warning: unused|FAILED|undefined" | head -40 || true
cmake --build "$OUT" > "$OUT.build.log" 2>&1 || { tail -40 "$OUT.build.log"; exit 1; }
TSAN_OPTIONS="halt_on_error=0 exitcode=66" "$OUT/tests/bluev_tests" --gtest_filter="$FILTER" 2>&1 | tail -${TAIL:-25}
exit ${PIPESTATUS:-0}
