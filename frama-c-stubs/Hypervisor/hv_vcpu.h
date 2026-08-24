/*
 * Hypervisor/hv_vcpu.h, for the analyzer only
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * A real SDK splits the framework across several headers and has
 * Hypervisor/Hypervisor.h pull hv_vcpu.h in, so a source may include either
 * spelling and get the same declarations. Three of ours name hv_vcpu.h directly
 * (core/bootstrap.h, core/bootstrap.c, core/launch.c), and stopped on "file not
 * found" even though the sibling stub already declared every vCPU call they
 * use.
 *
 * So this carries no declarations of its own. Putting them here instead would
 * split one closed set across two files and let the two drift, which is the
 * failure the sibling's own header comment is written against. It exists to
 * make the second spelling resolve.
 *
 * Same placement rule as the sibling: outside src/, reachable only through
 * FRAMAC_STUB_DIR in mk/verify.mk, never on a compile's include path.
 */

#pragma once

#include <Hypervisor/Hypervisor.h>
