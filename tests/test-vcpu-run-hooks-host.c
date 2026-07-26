/*
 * Native-host compile test for vcpu_run_loop hook API compatibility.
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <assert.h>
#include <stddef.h>
#include <stdio.h>

#include "syscall/proc.h"

typedef int vcpu_run_loop_sig(hv_vcpu_t vcpu,
                              hv_vcpu_exit_t *vexit,
                              guest_t *g,
                              bool verbose,
                              int timeout_sec,
                              int *wait_status_out);

typedef int vcpu_run_loop_with_hooks_sig(hv_vcpu_t vcpu,
                                         hv_vcpu_exit_t *vexit,
                                         guest_t *g,
                                         bool verbose,
                                         int timeout_sec,
                                         int *wait_status_out,
                                         const vcpu_run_hooks_t *hooks);

_Static_assert(__builtin_types_compatible_p(__typeof__(vcpu_run_loop),
                                            vcpu_run_loop_sig),
               "vcpu_run_loop compatibility signature changed");
_Static_assert(
    __builtin_types_compatible_p(__typeof__(vcpu_run_loop_with_hooks),
                                 vcpu_run_loop_with_hooks_sig),
    "vcpu_run_loop_with_hooks signature changed");

static int tick(guest_t *g, void *opaque)
{
    return g != NULL && opaque != NULL;
}

int main(void)
{
    vcpu_run_hooks_t hooks = {
        .tick = tick,
        .opaque = (void *) 0x1,
    };

    if (hooks.tick == NULL || hooks.opaque == NULL) {
        fprintf(stderr, "test-vcpu-run-hooks-host: API wiring failed\n");
        return 1;
    }

    /* Verify tick actually functions with non-NULL and NULL parameters */
    assert(hooks.tick((guest_t *) 0x1234, hooks.opaque) == 1);
    assert(hooks.tick(NULL, hooks.opaque) == 0);
    assert(hooks.tick((guest_t *) 0x1234, NULL) == 0);
    assert(hooks.tick(NULL, NULL) == 0);

    printf("test-vcpu-run-hooks-host: PASS\n");
    return 0;
}
