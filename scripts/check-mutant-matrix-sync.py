#!/usr/bin/env python3
"""Fail when the verify-mutants CI matrix drifts from mk/analysis.mk's targets.

.github/workflows/main.yml's verify-mutants job hand-lists the same proof
target names mk/analysis.mk's VERIFY_<T>_SRC entries define, so the mutation
gate can shard one job per target. The workflow's own comment admits the
failure mode: a target missing from the YAML matrix silently drops that
target's mutation coverage from CI, with no error. The run still reports
green, having simply never mutated that target. Nothing else checks the two
lists stay in sync.

Usage:
    check-mutant-matrix-sync.py
"""

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent


def mk_targets():
    """Proof target names from mk/analysis.mk's VERIFY_<T>_SRC entries."""
    text = (ROOT / "mk" / "analysis.mk").read_text()
    return {
        m.group(1).lower()
        for m in re.finditer(r"^VERIFY_([A-Z]+)_SRC\s*:=", text, re.MULTILINE)
    }


def workflow_matrix_targets():
    """Target names from verify-mutants' strategy.matrix.target list.

    Regex rather than a YAML parser, matching every other check script in
    this tree that reads mk/analysis.mk or main.yml as text: PyYAML is not a
    dependency anywhere else here, and the matrix block's shape (one
    "- name" per line under a fixed "target:" key) does not need a real
    parser to read reliably.
    """
    text = (ROOT / ".github" / "workflows" / "main.yml").read_text()
    m = re.search(
        r"^  verify-mutants:.*?^      matrix:\n        target:\n((?:          - \S+\n)+)",
        text,
        re.MULTILINE | re.DOTALL,
    )
    if not m:
        print(
            "  could not find verify-mutants' matrix.target list in "
            ".github/workflows/main.yml. The job may have been renamed or "
            "restructured; update this script's regex to match",
            file=sys.stderr,
        )
        return None

    targets = re.findall(r"^          - (\S+)$", m.group(1), re.MULTILINE)
    # Kept as a list until duplicates are checked: converting straight to a
    # set here would make two matrix entries for the same target compare as
    # "in sync" with mk/analysis.mk's single entry, silently accepting a
    # hand-edit that doubles that target's mutation-gate job (and CI cost)
    # rather than flagging it.
    seen, dupes = set(), set()
    for t in targets:
        (dupes if t in seen else seen).add(t)
    if dupes:
        print(
            "  duplicate target(s) in verify-mutants' matrix.target list "
            "in .github/workflows/main.yml. Each duplicate runs the same "
            "target's mutation gate as a second, redundant CI job:",
            file=sys.stderr,
        )
        for t in sorted(dupes):
            print(f"    {t}", file=sys.stderr)
        return None
    return seen


def main():
    mk = mk_targets()
    wf = workflow_matrix_targets()
    if wf is None:
        return 2

    missing_from_ci = mk - wf
    extra_in_ci = wf - mk

    if not missing_from_ci and not extra_in_ci:
        print(
            f"  {len(mk)} proof target(s) match between mk/analysis.mk and "
            "the verify-mutants CI matrix"
        )
        return 0

    if missing_from_ci:
        print(
            "  proof target(s) in mk/analysis.mk with no verify-mutants CI "
            "matrix entry, so their mutation coverage silently does not run "
            "in CI:",
            file=sys.stderr,
        )
        for t in sorted(missing_from_ci):
            print(f"    {t}", file=sys.stderr)
    if extra_in_ci:
        print(
            "  verify-mutants CI matrix entries with no matching "
            "VERIFY_<T>_SRC in mk/analysis.mk, likely a stale or "
            "misspelled target:",
            file=sys.stderr,
        )
        for t in sorted(extra_in_ci):
            print(f"    {t}", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
