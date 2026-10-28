// SPDX-License-Identifier: GPL-3.0-or-later
/* Build shim for the BlueZ libsbc (third_party/sbc): the settings its configure
 * script would generate. Used by both the PS5 build and the host tests
 * (BLUEV_HOST_BUILD). */
#ifndef SBC_CONFIG_H
#define SBC_CONFIG_H

#define VERSION "2.2"

#include <stddef.h>
#include <stdint.h>

/* libsbc only reads __BYTE_ORDER. Take it from the system header when there is
 * one (glibc, BSD), else from the compiler (macOS). */
#if __has_include(<endian.h>)
#include <endian.h>
#elif __has_include(<sys/endian.h>)
#include <sys/endian.h>
#endif
#ifndef __BYTE_ORDER
#define __BYTE_ORDER __BYTE_ORDER__
#endif
#ifndef __LITTLE_ENDIAN
#define __LITTLE_ENDIAN __ORDER_LITTLE_ENDIAN__
#endif
#ifndef __BIG_ENDIAN
#define __BIG_ENDIAN __ORDER_BIG_ENDIAN__
#endif

#ifdef BLUEV_HOST_BUILD
/* The host tests use libsbc's plain C encoder on every CPU: defining the
 * include guards of its SIMD headers leaves them empty. The console build
 * keeps MMX and SSE. */
#define __SBC_PRIMITIVES_MMX_H
#define __SBC_PRIMITIVES_SSE_H
#define __SBC_PRIMITIVES_NEON_H
#define __SBC_PRIMITIVES_ARMV6_H
#define __SBC_PRIMITIVES_IWMMXT_H
#endif

/* The target CPU always has MMX and SSE4.2, so libsbc does not probe for them
 * at run time. */
#define __builtin_cpu_init() ((void)0)
#define __builtin_cpu_supports(feature) 1

#endif
