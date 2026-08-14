---
name: elfuse-debug
description: Localizing a guest that misbehaves under elfuse - crash reports, the verbose and startup-trace switches, the syscall histogram, the TLBI bisect switch, the GDB stub, and the qemu differential. Use when a guest faults, hangs, gets a wrong errno, dies only sometimes, or starts too slowly, and you do not yet know whether the fault is host-side or guest-side.
---

# Localizing an elfuse failure

Every other skill in this repo tells you which rule you broke. This one tells
you how to find out. The question that orders all of it is the same one every
time: did the host hand the guest something wrong, or did the guest execute
something the guest ABI does not support?

Answer that before editing anything. The most expensive mistake here is
patching a syscall whose real fault is a stale TLB entry or a return path that
rebuilt EL0 state incorrectly, because the syscall is where the damage becomes
visible and not where it was caused.

## Read the crash report first

A fatal error prints a structured report from `crash_report()`
(`src/debug/crashreport.c`) with the register dump and the memory layout. Its
classification is the first bisection, and it is free:

| Type | What it means for you |
|------|-----------------------|
| `CRASH_BAD_EXCEPTION` | The guest took an exception at EL1 (HVC #2). Guest-side: permissions, a bad mapping, or a vector rule. Start at `elfuse-guest-abi`. |
| `CRASH_UNEXPECTED_HVC` / `CRASH_UNEXPECTED_EC` | The shim and the host dispatcher disagree about the protocol. Usually a half-landed HVC change. |
| `CRASH_HV_CHECK` | Hypervisor.framework refused a call. Host-side: a mapping or permission elfuse asked for is not one HVF allows. |
| `CRASH_ELR_ZERO` | Register state after exec is not what the host wrote. A return-to-EL0 path problem, not a loader problem. |
| `CRASH_TIMEOUT` | One `hv_vcpu_run()` iteration exceeded the `--timeout` watchdog. |

`CRASH_TIMEOUT` is the one that gets misread. The watchdog bounds a single run
iteration, not total runtime, so a CPU-bound guest can trip it legitimately and
`--timeout 0` is the right answer there. A guest that is actually stuck (futex,
wait, poll) trips it too, and raising the timeout only makes it take longer to
say so.

## By symptom

Faults at or near the first guest instruction.
Bring-up, not your syscall. `ELFUSE_STARTUP_TRACE=steps` gives per-step
wall-time for VM bring-up so you can see which step is the last one to
complete. The usual causes are all in `elfuse-guest-abi`: the initial stack,
enabling the MMU from the wrong side, or sysreg bits that HVF does not
default the way the architecture does.

Dies at a specific syscall.
`-v` / `--verbose` turns on syscall-level and loader diagnostics. If the
syscall is new, suspect its `dispatch.tbl` entry before its implementation: a
fourth argument that behaves as 0 or NULL on every call is the signature of a
missing `needs_extra_regs`. See `elfuse-syscall`.

Works under `-v`, fails without it.
That is the same bug, and the verbose flag is what hides it. The dispatcher
fetches X3-X5 when the entry asks for them or when verbose is on, so a missing
`needs_extra_regs` reads real arguments under `-v` and deterministic zeros
without it. Any behavioral difference between a verbose and a quiet run is
this until proven otherwise; do not debug it as a timing problem.

Wrong result, no crash.
Do not reason about what Linux would do. Run the same binary under the
`qemu-aarch64` lane, which boots a real kernel and is the ground truth in this
tree. A TIMEOUT there is emulation speed and means nothing.

Works almost always, fails under load or after an mmap/mprotect.
Suspect a stale TLB before suspecting the syscall. `ELFUSE_DISABLE_TLBI_RANGE=1`
forces the broadcast fallback (`src/core/guest.c`); if the failure disappears,
the selective or range request the host computed was too small, and the fix is
in the accumulator, not in the caller. The same shape of bug appears when two
vCPUs are involved, which is why the accumulator slot is per-vCPU.

A handler runs with the wrong PC, SP, or LR.
The host rebuilt EL0 state and the shim restored a stale frame over it, or the
reverse. See "Paths that rebuild EL0 state" in `elfuse-guest-abi`. Nothing in
the syscall implementation can cause or fix this.

Dynamic guest starts too slowly.
`ELFUSE_STARTUP_TRACE=syscalls` records a per-syscall histogram (count, total,
max latency) through the linker's syscall storm and freezes at the first
successful `execve`, so the dump is the startup picture rather than steady
state. `=all` enables it alongside the step tracer. `ELFUSE_SHIM_STATS` dumps
the shim's counter table at exit, attributing every fast-path bail, which is
how you tell a syscall that is slow from one that is not being served inline
at all.

Terminal or pty behavior.
`ELFUSE_PTY_LOG=<file>` mirrors the pty layer's diagnostics to a file.

## Stepping the guest

```
build/elfuse --gdb 1234 --gdb-stop-on-entry ./binary
aarch64-linux-gnu-gdb -ex "target remote :1234" ./binary
```

All-stop, hardware breakpoints and watchpoints, full register and memory
access. Two properties are not negotiable and will confuse you if you forget
them: registers come from a snapshot, because HVF requires register access on
the owning thread, and `--gdb` is rejected for x86_64 guests because the stub
would serve the translated aarch64 view rather than the state the guest thinks
it has.

## Before calling it an elfuse bug

Reproduce it under `qemu-aarch64`. A divergence from that lane is an elfuse
bug worth reporting with the crash report attached; identical behavior in both
means the guest is doing something the guest itself got wrong.

## Authoritative sources

This skill is a working summary. These are tracked and survive a fresh clone,
so prefer them when the two disagree:

- `docs/usage.md` - the full option list and the exact semantics of
  `--timeout`, `--fakeroot`, and `ELFUSE_FAKEROOT_EXEC`.
- `docs/internals.md`, section "GDB Stub" - the snapshot protocol and the
  `src/debug/` split.
- The header comments in `src/core/startup-trace.h`, `src/debug/syscall-hist.h`,
  and `src/debug/crashreport.h` - each states its env var's accepted values and
  what it costs when disabled.
