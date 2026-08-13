/*
 * Path component depth arithmetic: the parts a proof can reach
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Sysroot containment is a counting argument. Resolution walks a guest path
 * component by component, pushing a mark for each name and popping one for each
 * "..", and the guest cannot escape the sysroot precisely because the pop
 * floors at zero the way path_resolution(7) floors "/.." at the root. Three
 * loops in path.c keep that counter, each spelling the floor differently, and
 * one of them uses it to index the mark array that records where each component
 * started.
 *
 * Both directions are one comparison away from a memory error. Drop the floor
 * and "depth--" at zero wraps to SIZE_MAX, so the next pop reads marks at that
 * index and truncates the output buffer at whatever it finds; drop the capacity
 * check and the next push writes marks one past its end. Neither is reachable
 * today, and neither is reachable by construction, which is the distinction
 * this header exists to close on the boundary that keeps a guest inside its
 * sysroot.
 *
 * Split into a header because path.c cannot be given to Frama-C: it includes
 * the macOS dirent and fcntl headers, which the analyzer's libc does not model.
 * This header needs nothing but stdint.h, so make verify-pathdepth proves it
 * directly.
 */

#pragma once

#include <stdint.h>

/* Record one more component, or 0 when the mark array is full.
 *
 * The capacity is a parameter rather than a constant here because the callers
 * size their mark arrays differently. What the proof pins is that a successful
 * push leaves a depth that is a valid index for the NEXT mark write, which is
 * the property the caller would otherwise have to re-derive at the array.
 */
/*@
  requires depth <= cap;
  requires \valid(out_depth);
  assigns *out_depth;
  ensures binary: \result == 0 || \result == 1;
  ensures exact: \result != 0 <==> depth < cap;
  ensures counts_one: \result != 0 ==> *out_depth == depth + 1;
  ensures stays_in_array: \result != 0 ==> *out_depth <= cap;
  ensures no_wrap: \result != 0 ==> *out_depth > depth;
  ensures untouched_on_reject: \result == 0 ==> *out_depth == \old(*out_depth);
 */
static inline int path_depth_push(uint64_t depth,
                                  uint64_t cap,
                                  uint64_t *out_depth)
{
    if (depth >= cap)
        return 0;

    *out_depth = depth + 1;
    return 1;
}

/* Drop one component for a "..", or 0 at the root.
 *
 * Returning 0 rather than saturating is what lets the caller distinguish the
 * two cases path_resolution(7) separates: a ".." that pops a real component,
 * and a ".." at the root that names the root and must not touch the output. The
 * callers that only need the floor ignore the distinction and treat 0 as "leave
 * the depth alone".
 */
/*@
  requires \valid(out_depth);
  assigns *out_depth;
  ensures binary: \result == 0 || \result == 1;
  ensures exact: \result != 0 <==> depth > 0;
  ensures counts_one: \result != 0 ==> *out_depth == depth - 1;
  ensures no_wrap: \result != 0 ==> *out_depth < depth;
  ensures floors_at_root: \result == 0 <==> depth == 0;
  ensures untouched_at_root: \result == 0 ==> *out_depth == \old(*out_depth);
 */
static inline int path_depth_pop(uint64_t depth, uint64_t *out_depth)
{
    if (depth == 0)
        return 0;

    *out_depth = depth - 1;
    return 1;
}
