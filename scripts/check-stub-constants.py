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
STUB_DIR = ROOT / "frama-c-stubs"

# Only object-like defines with an integer value. A macro with parameters or a
# non-numeric body is not a constant this can compare, and is reported as
# unchecked rather than silently passed.
DEFINE = re.compile(
    r"^#define\s+([A-Z_][A-Z_0-9]*)\s+(0[xX][0-9a-fA-F]+|\d+)\s*$", re.M
)


def c_int(token):
    """Value of a C integer literal, octal included.

    int(token, 0) is not this function: Python rejects a leading zero, and
    Darwin writes whole families that way. TIOCM_DTR is 0002 in sys/ioccom.h,
    so a gate using int(_, 0) does not report a mismatch on it, it raises
    ValueError and takes the build down with a traceback.
    """
    text = token.lower()
    if text.startswith("0x"):
        return int(text, 16)
    if len(text) > 1 and text.startswith("0"):
        return int(text, 8)
    return int(text, 10)


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


def sdk_mentions(search_dirs, names):
    """Names the SDK mentions at all, however it spells them.

    The value check below can only compare object-like defines. Hypervisor
    declares its constants as enumerators, so a grep for #define finds nothing
    and every one of them would read as a name the SDK does not have, which is
    the report reserved for a typo. Splitting the two questions keeps that
    report meaningful: a name the SDK mentions but does not #define is an
    enumerator this cannot value-check, while a name it never mentions is
    wrong whatever the spelling.
    """
    pattern = r"\b(" + "|".join(names) + r")\b"
    hit = subprocess.run(
        ["grep", "-rhoE", "--include=*.h", pattern, *[str(d) for d in search_dirs]],
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True,
    ).stdout.split()
    return set(hit)


def sdk_values(include_dir, names):
    """{name: {values the SDK defines it as}} for every @names, in one pass.

    Searched across the whole include tree rather than named headers: the stub's
    comment says which header each constant comes from, and pinning that here
    would just be a second copy of the same claim to keep in step.

    One grep for all of them rather than one each. The tree is about 3,400 files
    and a scan costs roughly 0.75s, which a per-constant loop multiplied by the
    number of stub constants for no reason.
    """
    pattern = (
        r"^#define[ \t]+(" + "|".join(names) + r")[ \t]+(0[xX][0-9a-fA-F]+|[0-9]+)"
    )
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
            found[parts[1]].add(c_int(parts[2]))
    return found


def stub_files(explicit):
    """Every header under frama-c-stubs, not just macos-libc.h.

    The constants moved out of that one file when the shadow headers arrived:
    sys/ipc.h and sys/msg.h carry Darwin values for the same reason and with
    the same consequence if one is wrong. Scanning the directory rather than a
    list means a new stub is covered the day it lands instead of the day
    somebody remembers to add it here.
    """
    if explicit:
        path = pathlib.Path(explicit)
        return [path] if path.is_file() else []
    return sorted(p for p in STUB_DIR.rglob("*.h"))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--stub", default=None)
    args = ap.parse_args()

    stubs = stub_files(args.stub)
    if not stubs:
        print(f"  no stub headers found under {STUB_DIR}")
        return 1

    sdk = sdk_path()
    if sdk is None:
        print("  STUBCONST no macOS SDK; skipping (a macOS run checks this)")
        return 0
    include_dir = sdk / "usr" / "include"
    # Only the frameworks the stub tree actually mirrors, named by its own
    # subdirectories: frama-c-stubs/Hypervisor means Hypervisor.framework. The
    # whole Frameworks tree is 6,266 headers and a grep over it costs half a
    # minute for one framework's worth of answers.
    #
    # Resolved rather than globbed, because a framework's Headers is a symlink
    # into Versions/A and grep -r does not descend a symlinked directory. That
    # silently found nothing for two of the Hypervisor names while finding the
    # others, which reads as "the SDK does not have this name" and is the one
    # report this gate must not get wrong.
    fw_root = sdk / "System" / "Library" / "Frameworks"
    frameworks = []
    for sub in sorted(p.name for p in STUB_DIR.iterdir() if p.is_dir()):
        headers = fw_root / f"{sub}.framework" / "Headers"
        if headers.is_dir():
            frameworks.append(headers.resolve())

    defines = []
    origin = {}
    for stub in stubs:
        for name, raw in DEFINE.findall(stub.read_text()):
            defines.append((name, raw))
            origin[name] = stub.relative_to(ROOT)
    if not defines:
        print(f"  no integer defines found under {STUB_DIR}; the regex moved")
        return 1

    names = [name for name, _ in defines]
    found = sdk_values(include_dir, names)

    # Only for the names the value scan came up empty on. Asking it about all of
    # them means grepping a 3,400-file tree for bare words, and one of those
    # words is NAME_MAX: the match list runs to tens of thousands of lines and
    # the gate went from under a second to half a minute for an answer it
    # already had.
    unresolved = [n for n in names if not found[n]]
    mentioned = (
        sdk_mentions([include_dir, *frameworks], unresolved) if unresolved else set()
    )
    wrong, missing, enum_only = [], [], []
    for name, raw in defines:
        want = c_int(raw)
        got = found[name]
        if not got:
            (enum_only if name in mentioned else missing).append(name)
        elif got != {want}:
            # One value that disagrees, or several headers disagreeing with each
            # other; either way there is nothing here that matches the stub.
            wrong.append((name, want, sorted(got)))

    if wrong:
        print("  stub constants disagree with the macOS SDK:")
        for name, want, got in wrong:
            print(f"    {origin[name]}: {name}: stub {want} ({hex(want)}), SDK {got}")
        print("  The analyzer never links, so this changes what the proofs")
        print("  reason about rather than what runs. Use the SDK value.")
        return 1

    if missing:
        print("  stub constants the SDK does not define uniquely:")
        for name in missing:
            print(f"    {origin[name]}: {name}")
        print("  Either the name is wrong or it is not an SDK constant; if it")
        print("  is deliberately synthetic, it does not belong in this file.")
        return 1

    checked = len(defines) - len(enum_only)
    note = ""
    if enum_only:
        note = (
            f"; {len(enum_only)} named by the SDK as enumerators, value not comparable"
        )
    print(
        f"  STUBCONST {checked} stub constant(s) in {len(stubs)} "
        f"header(s) match the macOS SDK{note}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
