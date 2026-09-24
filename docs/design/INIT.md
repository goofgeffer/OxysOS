<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# `init`, Orphans, and Stopping the Machine

**Phase**: sub-task 9.3 of [`../project/PLAN.md`](../project/PLAN.md).
**Source**: [`../../userland/init/main.c`](../../userland/init/main.c),
[`../../userland/shutdown/main.c`](../../userland/shutdown/main.c); in the kernel
`ProcessSetInit`, `ProcessAdoptOrphansOf`, `ProcessPause` and `ThreadLaunch`
([`../../kernel/proc/process.c`](../../kernel/proc/process.c)), `KernelPower`,
`KernelBootScreen`, `KernelDesktopEntry` and `KernelScreenBackToConsole`
([`../../kernel/kernel.c`](../../kernel/kernel.c)), and the `power` and `pause`
calls.
**Specifications**: IEEE Std 1003.1-2017 (`waitpid()`, `pause()`, `fork()`,
`execve()`, `kill()`, reparenting of orphans); IBM PC/AT Technical Reference (the
8042 output port, bit 0 the reset line, command `0xFE`).

The first user process, which starts and supervises the desktop and collects every
orphan; the kernel's reparenting; the `power` call reserved to `init`; and the two
pages the kernel draws when no program can: the boot screen and the power screen.

## 1. `init`

On the default boot entry the kernel starts one program, `/bin/init`, and records
it with `ProcessSetInit` (which is what makes `power` reservable). The shell on the
serial line remains the kernel's, started from a loop the entry point holds.

**Supervision.** `init` starts each `[service]` of `/etc/system.conf`
([`CONFIG.md`](CONFIG.md)) and **starts it again when it ends**, whether a person
closed it or it faulted: a desktop that died with nothing to notice leaves a bare
screen for ever. Two bounds:

- `needs = display` services are not started where `window_screen` reports
  `ENOTSUP` (the entries that give the shell the screen), or they would be refused
  and restarted endlessly.
- A service ending **five times in a row** is given up on, and `init` says so:
  `init: desktop ended 5 times in a row, last with status 0x7f; it is not started again.`
  The count is cleared only when `init` itself stops the service.

**The loop.**

```
for (;;)
{
    if a shutdown was asked → stop every service, collect them, power()
    ended = waitpid(-1)
    if ended is a service     → start it again, or give up on it
    if ended < 0 and ECHILD   → pause()
    otherwise                 → an orphan was collected; go round
}
```

- **Signal handlers only record which shutdown was asked.** The loop performs it,
  so services are stopped in order rather than from inside a signal frame with the
  desktop still drawing.
- **The shutdown is checked at the top**, before waiting, so a request that arrived
  during the last wait is not left until the next child ends.
- **`pause`** sleeps until a signal and returns `EINTR`. With no children `waitpid`
  returns `ECHILD` at once, and looping on that would occupy a processor for ever.
  It tests its condition and sleeps within one masked section, like every sleep
  here ([`SCHEDULER.md`](SCHEDULER.md)); an orphan handed over also wakes it.

## 2. Orphans

When a process ends, `ThreadTerminateCurrent` gives its children to `init`, beside
releasing its descriptors and windows: what an ended process holds is returned at
its end, not at its collection.

- **No `init`, no adoption.** Children stay their parent's rather than going to a
  process that will never wait.
- **`init` does not adopt its own children**, which would make it its own parent.
- **`init` is woken, with `SIGCHLD`, when an adopted child has already ended**;
  otherwise that zombie would wait for `init`'s next unrelated wake.

## 3. `power`

`power(action)` halts or restarts, and **only `init` may call it**; anyone else gets
`EPERM`. The check is in the dispatch, since authority belongs to the caller;
`KernelPower` does the work. `shutdown` therefore **asks**: it finds `init` by name
through `procinfo` (as `ps` does) and sends `SIGTERM` to halt or `SIGINT` to
restart. `init` is in no foreground group, so Control-C cannot reach it.

- **Before either**, the persistent `/etc` is synced and unmounted
  ([`../storage/PERSIST.md`](../storage/PERSIST.md)).
- **Halt**: stop every other processor (as a panic does), suspend the compositor,
  draw the power screen directly on the framebuffer, halt.
- **Restart**: write `0xFE` to port `0x64`, which pulses the reset line (bit 0 of
  the 8042 output port). The write waits, boundedly, for the input buffer to be
  empty; a machine with no controller falls through to a halt. The alternative
  (`0xD1` then a byte with bit 0 clear) holds the processor in reset instead and is
  not used.
- Neither returns. `power` returns only `EINVAL`, for an unknown action, having
  done nothing.

## 4. The boot and power screens

**The boot screen** is drawn on the default entry as soon as the compositor exists:
the mark of [`../../art/logo.h`](../../art/logo.h), a wordmark and one dim line.
On that entry the display is quiet (the version banner too, decided by
`KernelDesktopEntry` before the banner is printed), and the serial line carries the
whole log, so nothing overwrites the page until the desktop composes over it.
Entries that give the shell the screen do not draw it: a log or a prompt belongs
there.

**The power screen** is the same elements, drawn by `KernelPower` directly on the
framebuffer with the compositor suspended, as the fault screen is: the back buffer
holds a desktop that is no longer what should be shown.

**Giving the screen back.** If `init` or the shell cannot be started, or the shell
ends by a fault, the kernel falls back to its echo loop on the console.
`KernelScreenBackToConsole` runs **before** the explanation is printed: keys go
back to the terminal, the tick stops composing, the pointer is hidden, the display
leaves quiet mode, and the console is cleared. The person sees the reason at the
top of an empty screen. Without it, the reason went only to the serial line and
the console wrote into the frozen boot screen a row at a time. It does nothing when
the console already holds the screen, so each path may call it.

## Verification

`KernelVerifyInit` in [`../../kernel/test/proc/init.c`](../../kernel/test/proc/init.c)
asserts adoption on fixture processes composed in the table and never run, then
runs `init-check` ([`../../userland/init-check/main.c`](../../userland/init-check/main.c))
at privilege level 3 for the two calls.

| Property asserted | The failure it would catch |
| ----------------- | -------------------------- |
| A child records its parent's identifier. | No orphan of that parent ever found. |
| With no `init`, nothing is adopted. | Orphans sent to a process that will never wait. |
| With `init` set, both of a parent's children are reparented to it. | An orphan nobody can collect. |
| `init` does not adopt its own children. | A `waitpid` that never ends. |
| `power` from any other process is `EPERM`, for every action value. | **Any program stopping the machine** (with the check removed, the self-test's own program halts the boot). |
| `pause` returns `EINTR` when a signal arrives, and the signal is delivered. | An `init` that spins, or never wakes. |
| `init-check` left no open file or child, and ended itself. | A pass from a program that never ran. |

The test runs **last**, after the shell's sessions: run before them, on Bochs, its
forking child shifted the tick enough to disturb the job-control session's
timing. Supervision and shutdown themselves are exercised by operating them
([`../project/TESTING-GRAPHICS.md`](../project/TESTING-GRAPHICS.md)).

## Limitations

1. The serial shell is the kernel's, not a service of `init`. Making it one needs a
   terminal that is not the kernel's single one ([`TERMINAL.md`](TERMINAL.md)).
2. Restart supervision is a count of consecutive endings, not a rate.
3. Shutdown stops `init`'s services and releases `/etc`; other processes are not
   asked to finish.
4. `init` can be killed (`kill -9`), leaving no supervisor or collector; the kernel
   does not refuse signals to it.
5. Restart depends on the 8042; `0xCF9` and the ACPI reset register are not tried.
6. The boot screen shows no progress.
7. Giving the screen back ends the desktop even if the window manager and session
   could carry on, because the echo loop needs the console.
