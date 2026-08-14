#!/usr/bin/env python3
"""Detect plain-char use in proved code with the compiler, not a regex.

Frama-C 31 has no aarch64 machdep, so every target is proved under gcc_x86_64.
That model matches arm64 macOS on pointer width, byte order and alignment, but
not on one property: it makes plain char SIGNED where arm64 makes it unsigned
(see the table in mk/analysis.mk). The risk only exists where proved code reads
a plain char, so knowing exactly which sources do that is the thing worth
automating.

check-acsl-coverage.py answers it with a regex, which cannot see a plain char
reached through a typedef or produced by a macro. This asks the compiler
instead: each proved source is compiled twice, once with -fsigned-char and once
with -funsigned-char, and each proved function's code is compared.

Identical code means that function behaves the same under either signedness, so
the gcc_x86_64 proof describes the real target. Different code means it does
not, and the function is named. That is the invariant itself rather than a
proxy for it, which two earlier attempts at this file were not:

  - Comparing whole objects answers the wrong question. A unit like elf.c is
    full of ordinary code that reads a plain char quite properly, and judging
    the file reports that as though a proved function were at fault.
  - Comparing at -O1 or above lets the optimizer delete a plain-char read whose
    result is unused, so the objects agree while the source Frama-C reasons
    about still contains the read. Measured: a dead widening read is missed at
    -O1 and -O2 and caught at -O0.

At -O0 an explicit (unsigned char) cast emits the same mask under both
settings, so a correctly written read compares equal and only a genuine
dependence stands out. There is deliberately no allowlist: the invariant admits
no exception, and a proved function that depends on plain-char signedness needs
the cast, not a waiver.

Usage:
    check-char-signedness.py [--cc CC]
"""

import argparse
import pathlib
import shlex
import re
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent


def analysis_mk():
    """mk/analysis.mk with line continuations joined."""
    return re.sub(r"\\\n", " ", (ROOT / "mk" / "analysis.mk").read_text())


def proof_sources():
    """{target: (source path, [proved function names])}."""
    text = analysis_mk()
    utils = ""
    m = re.search(r"^VERIFY_UTILS_FCTS\s*:=\s*(.*)$", text, re.MULTILINE)
    if m:
        utils = m.group(1)

    srcs = {
        m.group(1).lower(): m.group(2).strip()
        for m in re.finditer(
            r"^VERIFY_([A-Z0-9_]+)_SRC\s*:=\s*(\S+)", text, re.MULTILINE
        )
    }
    out = {}
    for m in re.finditer(r"^VERIFY_([A-Z0-9_]+)_FCTS\s*:=\s*(.*)$", text, re.MULTILINE):
        target = m.group(1).lower()
        if target == "utils" or target not in srcs:
            continue
        fcts = m.group(2).replace("$(VERIFY_UTILS_FCTS)", utils)
        out[target] = (srcs[target], [f for f in fcts.split() if not f.startswith("$")])
    return out


def probe_source(src, functions):
    """A translation unit that forces the proved functions to be emitted.

    The math headers declare everything static inline, so a unit that merely
    includes one compiles to nothing and would compare equal no matter what the
    code did. Taking each address defeats that: the compiler has to emit a real
    body to point at.
    """
    refs = ",\n    ".join(f"(void *) {f}" for f in functions)
    # Absolute path: the probe lives in a temp directory, so a repo-relative
    # include would not resolve against -Isrc.
    path = ROOT / src
    return f'#include "{path}"\nvoid *const probe[] = {{\n    {refs},\n}};\n'


def disassembly(obj):
    """{symbol: [instructions]} for @obj, addresses stripped.

    Comparing per function rather than per object is what keeps the answer
    about proved code. A translation unit like elf.c holds plenty of ordinary
    code that reads a plain char quite properly, and judging the whole file
    would report that as if a proved function were at fault.
    """
    try:
        out = subprocess.run(
            ["otool", "-tv", str(obj)],
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
        )
    except FileNotFoundError:
        # This binary is load-bearing for the whole gate, so its absence has to
        # read like every other failure here rather than a traceback.
        return None
    if out.returncode != 0:
        return None
    blocks, current = {}, None
    for line in out.stdout.splitlines():
        label = re.match(r"^_([A-Za-z_][A-Za-z_0-9]*):", line)
        if label:
            current = label.group(1)
            blocks[current] = []
        elif current is not None:
            # Drop the address column so a layout shift is not read as a
            # difference in what the code does.
            blocks[current].append(re.sub(r"^[0-9a-f]+\t", "", line))
    return blocks


def compare(cc, src, functions, workdir):
    """Compile @src both ways. Returns (reading, error).

    "reading" lists the proved functions whose code depends on plain-char
    signedness; "error" is set instead when the comparison could not be made at
    all, which must never be mistaken for a clean result.
    """
    unit = workdir / "probe.c"
    unit.write_text(probe_source(src, functions))

    objs = {}
    for sign in ("signed", "unsigned"):
        obj = workdir / f"probe-{sign}.o"
        try:
            proc = subprocess.run(
                cc
                + [
                    # -O0 deliberately. At -O1 and above a plain-char read
                    # whose result is unused is deleted, and the two objects
                    # then compare equal while the source still holds the read
                    # Frama-C reasons about. Measured: a dead widening read is
                    # missed at -O1 and -O2, caught at -O0.
                    "-O0",
                    "-I",
                    str(ROOT / "src"),
                    "-I",
                    str(ROOT / "build"),
                    f"-f{sign}-char",
                    "-c",
                    "-o",
                    str(obj),
                    str(unit),
                ],
                cwd=ROOT,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
            )
        except OSError as exc:
            # cc itself does not exist or is not executable. otool's absence
            # a few lines below already reports this shape of failure through
            # disassembly()'s own try/except; this mirrors it for the compiler
            # rather than letting the exception crash the run.
            return [], f"compiler not runnable: {exc}"
        if proc.returncode != 0:
            return [], f"probe did not compile:\n{proc.stdout.strip()[:400]}"
        objs[sign] = disassembly(obj)
        if objs[sign] is None:
            return [], "otool could not disassemble the probe"

    reading, missing = [], []
    for f in functions:
        a, b = objs["signed"].get(f), objs["unsigned"].get(f)
        if a is None or b is None:
            # Emitted nothing to compare, so this function was not actually
            # examined. Silence here would read as a clean result.
            missing.append(f)
        elif a != b:
            reading.append(f)
    if missing:
        return [], "no code emitted for: " + ", ".join(sorted(missing))
    return reading, None


def main():
    ap = argparse.ArgumentParser()
    # Split rather than exec directly: CC is routinely a wrapper or carries
    # flags ("ccache clang", "cc -DTEST"), and forwarding that as one argv entry
    # would fail to exec while forwarding it unsplit by make would look like
    # stray arguments here.
    ap.add_argument(
        "--cc", default="cc", help="compiler command, flags allowed (default: cc)"
    )
    ap.add_argument("--target", help="check only this proof target")
    args = ap.parse_args()
    cc = shlex.split(args.cc) or ["cc"]

    targets = proof_sources()
    if args.target:
        if args.target not in targets:
            print(
                f"no such target: {args.target!r} "
                f"(known: {', '.join(sorted(targets))})",
                file=sys.stderr,
            )
            return 2
        targets = {args.target: targets[args.target]}
    if not targets:
        print("no proof targets found in mk/analysis.mk", file=sys.stderr)
        return 2

    # Two distinct failure classes, reported with two distinct headers. An
    # infra failure (compiler missing, otool missing, probe would not
    # compile) says nothing about whether any proved code actually depends on
    # signedness, and printing the signedness header over a toolchain problem
    # would mislead a reader into chasing the wrong thing.
    char_failures, infra_failures = [], []
    for target, (src, functions) in sorted(targets.items()):
        with tempfile.TemporaryDirectory() as tmp:
            reading, error = compare(cc, src, functions, pathlib.Path(tmp))
        if error:
            print(f"  ERROR     verify-{target:<9} {error}")
            infra_failures.append((target, error))
            continue
        if reading:
            print(f"  CHAR      verify-{target:<9} {', '.join(reading)}")
            char_failures.append((target, ", ".join(reading)))
        else:
            print(f"  no-char   verify-{target:<9} {len(functions)} function(s)")

    # Both headers print when both kinds of failure occur: an infra failure on
    # one target must never bury a genuine signedness finding on another, and
    # fixing the toolchain must not read as though it also fixed the char
    # dependence.
    if infra_failures:
        print(
            "\n  could not check plain-char signedness for every target.\n"
            "  This is a toolchain problem, not evidence about the proofs:",
            file=sys.stderr,
        )
        for target, reason in infra_failures:
            print(f"    verify-{target}: {reason}", file=sys.stderr)

    if char_failures:
        print(
            "\n  proved code depends on plain-char signedness.\n"
            "  gcc_x86_64 makes plain char signed where arm64 macOS makes it\n"
            "  unsigned, so the proof would not describe the real target. Read\n"
            "  the char through an explicit (unsigned char) or (uint8_t) cast.",
            file=sys.stderr,
        )
        for target, reading in char_failures:
            print(f"    verify-{target}: {reading}", file=sys.stderr)

    if infra_failures or char_failures:
        return 1

    total = sum(len(f) for _s, f in targets.values())
    print(f"\n  {total} proved function(s) behave the same under either signedness")
    return 0


if __name__ == "__main__":
    sys.exit(main())
