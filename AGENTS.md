# Working on elfuse

elfuse runs Linux ELF binaries from the macOS shell: each guest runs in a
Hypervisor.framework VM the `elfuse` process owns, and Linux syscalls are
translated to macOS behavior in host-side handlers rather than served by a
kernel. `README.md` states the scope and the limitations, `docs/internals.md`
the design, `docs/testing.md` the build and lane requirements.

This file routes. It restates nothing that is settled elsewhere, because a
rule copied to a second place is a rule that can drift.

## Before writing C

`CONTRIBUTING.md` is the tracked style guide and it wins over everything
here. Read it rather than working from a memory of typical C style: several
of its rules are not the common default.

## The skills

The conventions `CONTRIBUTING.md` does not cover live in `.claude/skills/`,
reachable as `.agents/skills/` as well. Each directory holds one SKILL.md
whose description states when it applies; read the one covering the change
before making it, not after review asks for it.

- `elfuse-conventions`: comments, commit messages, PR text, `docs/`, naming,
  atomics, and the prose register every written surface obeys.
- `elfuse-syscall`: adding or changing a Linux syscall, the translation
  boundary, `src/syscall/dispatch.tbl`, fd classes, lock order.
- `elfuse-guest-abi`: HVC calls, the EL1 shim, page tables, TLBI, and
  anything that changes what the guest observes on return.
- `elfuse-verify`: which lanes an area needs, the test matrix, and the
  Frama-C proof targets.
- `elfuse-security`: the guest as an attacker, and what a handler on the
  trust boundary owes.
- `elfuse-debug`: a guest that faults, hangs, or answers the wrong errno.
- `elfuse-refactor`: behavior-preserving cleanup of code that works.
- `elfuse-skills`: editing these skill files themselves.

## Gates

`make check` builds, runs the tests, and runs the checkers. `make indent`
formats and `make check-format` verifies without rewriting; between them
they settle most review comments before they are written.

Run what the change touched before calling the work done, and say what ran
and what it reported; effort is not a finding. `elfuse-verify` settles which
lanes an area needs, and which of them skip rather than fail when a tool is
missing.

## Prose

Two rules bind before the first sentence. Source comments and commit
messages are ASCII with no markdown syntax, which `make check-ascii` and the
commit-message hook enforce. The em dash (U+2014) is banned on every surface,
including the ones no gate reaches: `docs/`, PR bodies, and review replies. A
spaced double hyphen carries the same register rather than avoiding it.

The rest of the machine register, effort claims and inflation words and
prompt echo among them, is defined once in
`.claude/skills/elfuse-conventions/references/prose-register.md`. A PR thread
is a technical discussion between people; walkthroughs, status tables, and
replies that hand the choice back to the reviewer are rejected on sight.

## Files that are not yours

Untracked working docs at the repo root or under `.claude/` belong to
whoever put them there. Never `git add`, commit, stage, gitignore, or delete
another person's: untracked and visible in `git status` is deliberate.
Anything that has to survive a fresh clone belongs in `CONTRIBUTING.md`,
`docs/`, or a skill.
