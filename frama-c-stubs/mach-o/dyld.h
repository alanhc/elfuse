/*
 * mach-o/dyld.h, for the analyzer only
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Two sources call _NSGetExecutablePath to find elfuse's own binary.
 * core/bootstrap.c records it once at startup through proc_set_elfuse_path,
 * which is what /proc/self/exe and the shebang loop answer from.
 * runtime/forkipc.c is the one that spawns it: clone posix_spawns the same
 * binary so the child carries the same entitlement and build. One declaration
 * is the whole of what either file needs from this header.
 *
 * Same placement rule as the Hypervisor stub: outside src/, reachable only
 * through FRAMAC_STUB_DIR in mk/verify.mk, never on a compile's include path.
 */

#pragma once

#include <stdint.h>

int _NSGetExecutablePath(char *buf, uint32_t *bufsize);
