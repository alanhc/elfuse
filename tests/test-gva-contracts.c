/*
 * proved/gva.h call-site precondition checks
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * make verify-gva proves the *_args_ok predicates match their requires clauses
 * exactly, and proves each GVA_CONTRACT_ASSERT is derivable from the clause it
 * mirrors. Neither of those catches a check that is wired to a constant rather
 * than to the real argument, because a constant satisfying the predicate is
 * still derivable.
 *
 * This closes that from the other side: each conjunct is violated in turn and
 * the check must actually reject. Runs only under -DELFUSE_CONTRACT_ASSERT (see
 * make check-contracts); the default build compiles the checks out, so there is
 * nothing to observe and the test skips.
 *
 * The violating call is made in a forked child because a failed check aborts.
 */

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

#include "proved/gva.h"

#ifdef ELFUSE_CONTRACT_ASSERT

static int passes, fails;

/* One result line per case. The label width is fixed so the verdict column
 * lines up across both helpers.
 */
static void pass(const char *what)
{
    printf("  %-38s [ OK ]\n", what);
    passes++;
}

static void fail(const char *what, const char *why)
{
    printf("  %-38s [ FAIL ] (%s)\n", what, why);
    fails++;
}

/* Run one out-of-contract call in a child. The check must abort it. */
static void expect_reject(const char *what, void (*violate)(void))
{
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) {
        fail(what, "fork");
        return;
    }
    if (pid == 0) {
        /* The check is expected to abort here, and its message would otherwise
         * read as a failure in the suite log. Drop it; the parent reports the
         * outcome.
         */
        if (!freopen("/dev/null", "w", stderr))
            _exit(2);
        violate();
        _exit(0); /* reached only if the check did not fire */
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        fail(what, "waitpid");
        return;
    }
    if (!WIFSIGNALED(status)) {
        fail(what, "call was accepted");
        return;
    }

    /* SIGABRT specifically, not merely "died on a signal". A mis-wired check
     * would let execution fall through into the arithmetic the precondition
     * guards, and gva % granule with granule == 0 is undefined behavior that
     * can itself terminate the child. Accepting any signal would report that as
     * a rejection when the check never fired at all.
     */
    if (WTERMSIG(status) != SIGABRT) {
        fail(what, "died on a signal other than SIGABRT");
        return;
    }
    pass(what);
}

/* Sinks marked volatile so the calls cannot be optimized away. */
static volatile uint64_t sink_gpa, sink_chunk, sink_clamp;

/* The three leaf cases differ only in the two checked arguments, so base and
 * gva stay 0 and nothing but the violated conjunct varies.
 */
static void leaf_call(uint64_t ipa, uint64_t granule)
{
    uint64_t gpa = 0, chunk = 0;
    gva_leaf_target(ipa, 0, 0, granule, &gpa, &chunk);
    sink_gpa = gpa;
    sink_chunk = chunk;
}

static void leaf_granule_zero(void)
{
    leaf_call(0x1000, 0);
}

static void leaf_granule_too_big(void)
{
    leaf_call(0x1000, GVA_PT_ADDR_MASK + 1);
}

static void leaf_ipa_too_big(void)
{
    leaf_call(GVA_PT_ADDR_MASK + 1, 4096);
}

static void clamp_chunk_zero(void)
{
    sink_clamp = gva_chunk_clamp(0, 0, 4096, 4096, 0);
}

static void clamp_gpa_past_region(void)
{
    sink_clamp = gva_chunk_clamp(1, 4096, 4096, 4096, 0);
}

static void clamp_total_at_limit(void)
{
    sink_clamp = gva_chunk_clamp(1, 0, 4096, 4096, 4096);
}

int main(void)
{
    printf("test-gva-contracts: call-site precondition checks\n");

    /* In-contract calls must survive; a check that rejects everything would
     * otherwise pass every case below.
     */
    uint64_t gpa = 0, chunk = 0;
    if (gva_leaf_target(0x1000, 0x1000, 0, 4096, &gpa, &chunk))
        pass("in-contract gva_leaf_target accepted");
    else
        fail("in-contract gva_leaf_target", "rejected");

    if (gva_chunk_clamp(8, 0, 4096, 4096, 0) == 8)
        pass("in-contract gva_chunk_clamp accepted");
    else
        fail("in-contract gva_chunk_clamp", "wrong result");

    expect_reject("granule == 0 rejected", leaf_granule_zero);
    expect_reject("granule > GVA_PT_ADDR_MASK rejected", leaf_granule_too_big);
    expect_reject("ipa > GVA_PT_ADDR_MASK rejected", leaf_ipa_too_big);
    expect_reject("chunk == 0 rejected", clamp_chunk_zero);
    expect_reject("gpa >= region_end rejected", clamp_gpa_past_region);
    expect_reject("total >= limit rejected", clamp_total_at_limit);

    printf("\ntest-gva-contracts: %d passed, %d failed%s\n", passes, fails,
           fails == 0 ? " - PASS" : " - FAIL");
    return fails > 0 ? 1 : 0;
}

#else /* !ELFUSE_CONTRACT_ASSERT */

int main(void)
{
    printf(
        "test-gva-contracts: SKIP (built without -DELFUSE_CONTRACT_ASSERT; "
        "run make check-contracts)\n");
    return 0;
}

#endif
