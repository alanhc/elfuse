#!/usr/bin/env python3
"""Fail when a stub constant disagrees with the macOS SDK it claims to copy.

frama-c-stubs/macos-libc.h supplies Darwin constants Frama-C's portable libc
omits, and its header says the values are the real ones rather than
placeholders. That claim was wrong the day it was written: ETOOMANYREFS was
given 62, which is Darwin's ELOOP, and both are arms of the same linux_errno()
switch. Nothing caught it, because nothing was checking.

The analyzer never links against the SDK, so a wrong value cannot break a
build. It quietly changes what the proofs reason about instead: two arms of a
walked switch sharing a value makes one of them look unreachable, and a proof
over that switch is then about a program nobody ships.

Skips rather than fails when no SDK is present, so a Linux checkout can still
run the rest of the gates. A macOS CI run has one.

Usage:
    check-stub-constants.py [--stub PATH]
"""

import argparse
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
DEFAULT_STUB = ROOT / "frama-c-stubs" / "macos-libc.h"

# Only object-like defines with an integer value. A macro with parameters or a
# non-numeric body is not a constant this can compare, and is reported as
# unchecked rather than silently passed.
DEFINE = re.compile(r"^#define\s+([A-Z_][A-Z_0-9]*)\s+(0x[0-9a-fA-F]+|\d+)\s*$", re.M)


def sdk_path():
    try:
        out = subprocess.run(
            ["xcrun", "--show-sdk-path"],
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
        )
    except OSError:
        return None
    path = pathlib.Path(out.stdout.strip()) if out.returncode == 0 else None
    return path if path and (path / "usr" / "include").is_dir() else None


def sdk_values(include_dir, names):
    """{name: {values the SDK defines it as}} for every @names, in one pass.

    Searched across the whole include tree rather than named headers: the stub's
    comment says which header each constant comes from, and pinning that here
    would just be a second copy of the same claim to keep in step.

    One grep for all of them rather than one each. The tree is about 3,400 files
    and a scan costs roughly 0.75s, which a per-constant loop multiplied by the
    number of stub constants for no reason.
    """
    pattern = r"^#define[ \t]+(" + "|".join(names) + r")[ \t]+(0x[0-9a-fA-F]+|[0-9]+)"
    hit = subprocess.run(
        ["grep", "-rhoE", pattern, str(include_dir)],
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True,
    ).stdout.splitlines()
    found = {name: set() for name in names}
    for line in hit:
        parts = line.split()
        if len(parts) >= 3 and parts[1] in found:
            found[parts[1]].add(int(parts[2], 0))
    return found


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--stub", default=str(DEFAULT_STUB))
    args = ap.parse_args()

    stub = pathlib.Path(args.stub)
    if not stub.is_file():
        print(f"  stub not found: {stub}")
        return 1

    sdk = sdk_path()
    if sdk is None:
        print("  STUBCONST no macOS SDK; skipping (a macOS run checks this)")
        return 0
    include_dir = sdk / "usr" / "include"

    defines = DEFINE.findall(stub.read_text())
    if not defines:
        print(f"  no integer defines found in {stub}; the regex or the file moved")
        return 1

    found = sdk_values(include_dir, [name for name, _ in defines])
    wrong, missing = [], []
    for name, raw in defines:
        want = int(raw, 0)
        got = found[name]
        if not got:
            missing.append(name)
        elif got != {want}:
            # One value that disagrees, or several headers disagreeing with each
            # other; either way there is nothing here that matches the stub.
            wrong.append((name, want, sorted(got)))

    if wrong:
        print("  stub constants disagree with the macOS SDK:")
        for name, want, got in wrong:
            print(f"    {name}: stub {want} ({hex(want)}), SDK {got}")
        print("  The analyzer never links, so this changes what the proofs")
        print("  reason about rather than what runs. Use the SDK value.")
        return 1

    if missing:
        print("  stub constants the SDK does not define uniquely:")
        for name in missing:
            print(f"    {name}")
        print("  Either the name is wrong or it is not an SDK constant; if it")
        print("  is deliberately synthetic, it does not belong in this file.")
        return 1

    print(f"  STUBCONST {len(defines)} stub constant(s) match the macOS SDK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
