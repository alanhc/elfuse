/*
 * mach-o/dyld.h, for the analyzer only
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * core/bootstrap.c calls _NSGetExecutablePath to find elfuse's own binary,
 * which is how a re-exec locates the image to spawn. One declaration is the
 * whole of what the file needs from this header.
 *
 * Same placement rule as the Hypervisor stub: outside src/, reachable only
 * through FRAMAC_STUB_DIR in mk/verify.mk, never on a compile's include path.
 */

#pragma once

#include <stdint.h>

int _NSGetExecutablePath(char *buf, uint32_t *bufsize);
