// SPDX-License-Identifier: GPL-3.0-or-later
/* Build shim for the BlueZ libsbc (third_party/sbc): the settings its configure
 * script would generate. Used by both the PS5 build and the host tests
 * (BLUEV_HOST_BUILD). */
#ifndef SBC_CONFIG_H
#define SBC_CONFIG_H

#define VERSION "2.2"

#include <stddef.h>
#include <stdint.h>

#ifdef BLUEV_HOST_BUILD
#include <endian.h>
#endif
#ifndef __BYTE_ORDER
#include <sys/endian.h>
#ifndef __BYTE_ORDER
#define __BYTE_ORDER _BYTE_ORDER
#endif
#endif
#ifndef __LITTLE_ENDIAN
#define __LITTLE_ENDIAN 1234
#endif
#ifndef __BIG_ENDIAN
#define __BIG_ENDIAN 4321
#endif

/* The SIMD encoder paths are switched on by libsbc itself on x86-64. The
 * target CPU always has MMX and SSE4.2, so libsbc does not probe for them at
 * run time. */
#define __builtin_cpu_init() ((void)0)
#define __builtin_cpu_supports(feature) 1

#endif
