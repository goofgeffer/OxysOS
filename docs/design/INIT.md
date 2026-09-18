<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# `init`, the Supervision Beneath the Desktop, and the Stopping of the Machine

**Phase**: 9, sub-task 9.3, of [`../project/PLAN.md`](../project/PLAN.md).
Section 1 is what this sub-task is; Section 2 is `init` itself — the first user
process, what it supervises, and the loop it is; Section 3 is the reparenting of
orphans, which is the kernel's half; Section 4 is the stopping of the machine,
the one call reserved to a single process; Section 5 is the boot screen and the
power screen, which are the two pages the kernel draws for itself; Section 6 is
the verification; Section 7 the limitations.

**Authority**: `PROJECT_GUIDELINES.md`, Sections 2, 3 and 6.

**Implementation**: [`../../userland/init/main.c`](../../userland/init/main.c) is
`init`; [`../../userland/shutdown/main.c`](../../userland/shutdown/main.c) is how
a person asks it. The kernel's half is `ProcessSetInit`,
`ProcessAdoptOrphansOf`, `ProcessPause` and `ThreadLaunch` in
[`../../kernel/proc/process.c`](../../kernel/proc/process.c), `KernelPower`,
`KernelBootScreen` and `KernelDesktopEntry` in
[`../../kernel/kernel.c`](../../kernel/kernel.c), and the two calls in
[`../../kernel/abi/oxys/syscall_abi.h`](../../kernel/abi/oxys/syscall_abi.h)
dispatched by
[`../../kernel/arch/x86_64/syscall/syscall.c`](../../kernel/arch/x86_64/syscall/syscall.c).
Asserted by [`../../kernel/test/proc/init.c`](../../kernel/test/proc/init.c),
which runs [`../../userland/init-check/main.c`](../../userland/init-check/main.c).

**Specifications**: IEEE Std 1003.1-2017 — `waitpid()`, `pause()`, `fork()`,
`execve()`, `kill()`, and the reparenting of a child whose parent has ended;
IBM Personal Computer AT technical reference — the keyboard controller's output
port, whose bit 0 is the system reset line, and the command that pulses it.

## 1. What this sub-task is

Phase 9 has had, since 9.2, a window manager with a program drawing upon it. It
has had no answer to three questions a system is expected to answer: **who
starts the programs that ought to be running**, **who collects a process whose
parent has ended**, and **how the machine is stopped**. This sub-task answers
all three with one process and two calls.

**What it adds**: `init`, the first user process, which starts the desktop and
starts it again when it ends, and which collects every orphan; the reparenting
in the kernel that gives it those orphans; `power`, the one call reserved to
`init` alone, which halts the machine or restarts it; `pause`, which is what
`init` waits in when it has nothing to collect; and `shutdown`, by which a
person asks. It also adds the two pages the kernel draws for itself — the boot
screen and the power screen — because the moments before a desktop and after it
are the two a person sees with no program running to draw them.

**What it did not add** was a service configuration: what `init` started was
written into `init`. Sub-task 9.4 supplied the format and the `/etc` hierarchy,
and the list of what to run is `/etc/system.conf` since —
[`CONFIG.md`](CONFIG.md), Section 4.1. Section 2.2 below describes the
supervision as it now stands.

## 2. `init`

### 2.1 It is the first user process, and the kernel starts only it

Before this sub-task the entry point started two programs: the shell, by a call
it waited in, and — since 9.2 — the window demonstration, launched beside it.
It now launches **one**, `/bin/init`, and tells the kernel which process that is
(`ProcessSetInit`), which is what makes `power` reservable. `init` starts the
desktop itself.

The shell stays the kernel's, and that is a limitation written down rather than
a design: the shell reads the terminal the kernel assembles, and is started in a
loop the entry point holds. Sub-task 9.6 puts a terminal emulator upon the
desktop, and the shell within it; at that point what starts a shell is `init`'s
question too. Section 7, limitation 2.

### 2.2 What it supervises, and why supervision is the point

`init` starts each service of `/etc/system.conf` — since sub-task 9.4; it was
`/bin/windows`, written here, before — and **starts it again whenever it ends**,
because a person closed its last window or because it faulted. That single
behaviour is the whole of what `init` adds over a program the kernel could have
launched itself: a desktop a person closed and could not get back is a desktop
that is gone, and a machine whose desktop died in a fault with nothing to notice
is a machine showing a bare ground for ever.

Two things bound it, both of them 9.4's and both recorded in
[`CONFIG.md`](CONFIG.md), Section 4.1. A service marked `needs = display` is not
started at all where the window manager does not have the screen — `init` asks
`window_screen`, which reports `ENOTSUP` upon the two entries that give the
shell the screen, and a service that needed one and was started there would be
refused, exit, and be started again for ever. And a service that ends five times
in a row is given up on, which is what stops a program that cannot run at all
from being restarted for the machine's whole life.

### 2.3 The loop, and why the handlers only set a flag

```
for (;;)
{
    if a shutdown was asked → stop every service, collect them, power()
    ended = waitpid(-1)
    if ended is a service     → start it again, or give up upon it
    if ended < 0 and ECHILD   → pause()
    otherwise                 → an orphan was collected; go round
}
```

A shutdown may arrive while `init` is asleep in `waitpid` or in `pause`. The
signal wakes it, the call reports `EINTR`, and the handler has run. **The
handler records which shutdown was asked and returns**; the loop performs it.
A handler that called `power` itself would stop the machine from inside a
signal frame with the desktop still drawing, which is the disorder this process
exists to prevent — the point of an orderly shutdown being that the things
below are stopped first and in a known order.

The shutdown is acted upon at the **top** of the loop, before the wait, so that
a request which arrived while the last wait slept is not left until the next
child ends — which upon a quiet desktop could be never.

### 2.4 `pause`, and why it had to exist

`init` on an entry with no desktop has no children, and `waitpid` reports
`ECHILD` at once. A loop that went round on that would take a processor for the
machine's whole life. There is no `sleep`, the kernel keeping no timer a program
may set ([`LIBC.md`](LIBC.md), Section 13), so the honest primitive is POSIX's
`pause`: suspend until a signal, and report `EINTR`.

It is four lines in the kernel over machinery that already existed — the wait
channel of [`SCHEDULER.md`](SCHEDULER.md), Section 9, and the wake a signal
already performs — and it follows the same discipline as every other sleep here:
the condition is tested and the sleep entered within one masked section, so that
a signal cannot fall between the two. A wake that was not a signal — an orphan
given to `init` — returns `EINTR` too and sends the loop round to `waitpid`,
which is where it should be.

## 3. The orphans, which are the kernel's half

**A process that ends gives its children to `init`**, in `ThreadTerminateCurrent`,
beside the release of its descriptors and its windows and for the same reason
those are released there: what a process that has ended holds is given back at
the ending and not at the collecting.
[`PROCESS.md`](PROCESS.md), Section 19, limitation 15, had recorded this as
owed — *"an orphan is nobody's"* — since sub-task 8.7, when a shell exiting with
a background job running first produced one.

Three things the adoption does, and each is a decision:

1. **Nothing is adopted where there is no `init`.** `ProcessAdoptOrphansOf`
   returns at once where none is set or where the one set has ended, and the
   children stay their parent's. An adoption that reparented regardless would
   send orphans to a process that will never wait, which is worse than leaving
   them: they would no longer be visible as the parent's.
2. **`init` does not adopt its own children**, which would loop a parent to
   itself and make a `waitpid` that never ends.
3. **`init` is woken where an adopted child had already ended.** A child that
   ended before its parent did is waiting for a collector; the parent is gone,
   and `init` is now it. Without the wake, `init` would collect that orphan only
   at its next ending of its own — upon a quiet desktop, never — and the slot
   would be held for as long. The wake is `SchedulerWake` and a `SIGCHLD`,
   which is what any parent gets.

## 4. Stopping the machine

### 4.1 `power` is reserved to `init`

`power(action)` halts the machine or restarts it, and **only `init` may call
it**; every other process is refused with `EPERM`. The authority is enforced in
the dispatch, at the boundary, because it is a property of the caller and not of
the action — `KernelPower` does the work and knows nothing of who asked.

The reservation is not a formality. Without it any program could stop the
machine, and Section 6.1 records what that looks like when it is removed: the
boot does not finish, because the self-test's own program stops the machine
half-way through it.

So `shutdown` **asks** rather than acts. It finds `init` by walking the process
table for the process of that name — `procinfo`, exactly as `ps` walks it, and
no new call for it — and sends `SIGTERM` to halt or `SIGINT` to restart. The
terminal's control-C cannot reach `init` by accident: `init` is in no foreground
group.

### 4.2 The halt, and the restart

A **halt** stops every other processor, as a panic does and for the same reason
— nothing should go on drawing over the page or writing to a device — suspends
the compositor, draws the power screen straight upon the framebuffer, and halts.

A **restart** pulses the reset line. The IBM Personal Computer AT technical
reference assigns bit 0 of the keyboard controller's output port to system
reset, and command `0xFE`, written to port `0x64`, pulses it low for a few
microseconds; the processor restarts from its reset vector. The command is
written only once the status register's input-buffer-full flag is clear, so that
it is not lost behind a byte the controller has not yet taken, and the wait is
bounded — a machine with no controller falls through to the halt rather than
spinning. The dangerous form of the same thing, writing `0xD1` and then a byte
with bit 0 clear, holds the processor in reset instead of pulsing it and is not
used.

Neither returns. `power` returns only `EINVAL`, for an action that is neither,
having done nothing — so that `init` learns of a mistake rather than stopping a
machine upon one.

## 5. The two pages the kernel draws for itself

### 5.1 The boot screen

**Upon the default entry only**, the screen shows a mark, a wordmark and a line
of text from the moment there is a back buffer to draw them into until the
desktop composes over them.

What it replaces is worth stating, because it is what made the request: that
entry's screen showed the one-line version banner and then nothing at all for
the length of the boot — the display falls quiet so that four hundred lines of
self-test verdicts do not scroll past a person who asked for a desktop, and what
quiet left was a banner and a black screen. The banner is now quiet too upon
that entry (`KernelDesktopEntry`, asked before the banner is printed), the
serial line carrying it as it carries everything, and the boot screen stands in
its place.

It is drawn **as soon as the compositor exists**, not at the end of the boot,
and that is what makes it the whole of what a person sees rather than a flash
before the desktop. Nothing overwrites it: the display is quiet upon that entry,
so nothing the kernel prints reaches the screen, and the graphics self-tests
that present do so with the boot screen in the buffer they present.

The entries that give the shell the screen do not draw it. A boot log or a
prompt is what belongs upon a screen the shell is about to take, and a splash
that hid a diagnostic entry's log would defeat the entry.

### 5.2 The power screen

The counterpart, drawn by `KernelPower` straight upon the framebuffer with the
compositor suspended — the fault screen's arrangement, for the fault screen's
reason: the machine is stopping, and the back buffer holds a desktop that is no
longer what should be shown.

Both pages are the same few elements — a ring of discs about a ringed disc, a
wordmark, one line of grey text — and both are drawn with the primitives and the
one face this system has.
[`../project/INSPIRATIONS.md`](../project/INSPIRATIONS.md), Section 3, asks that
the character be carried by geometry; a ring of circles is that, and it needs no
artwork this project does not have.

## 6. Verification

Two halves, as the signal test has. The adoption is kernel state — a parent's
children becoming `init`'s — and is asserted upon fixture processes composed in
the table and never run, because the machinery is a field reassigned and a wake.
The two calls need a privilege transition, `power` being reserved by the
dispatch and `pause` sleeping a real thread, and are asserted by `init-check`
running at privilege level 3.

| Property asserted | The silent failure it would catch |
| ----------------- | --------------------------------- |
| A child is created carrying its parent's identifier | A parent named wrongly, after which no orphan of it could ever be found |
| With no `init` set, nothing is adopted and the children stay their parent's | Orphans sent to a process that does not exist and will never wait, and no longer visible as the parent's |
| With `init` set, both of a parent's children are reparented to it | An orphan left in the table with nobody to collect it — the defect of `PROCESS.md`, Section 19, limitation 15. **Observed**, Section 6.1 |
| `init` does not adopt its own children | A parent looped to itself, and a `waitpid` that never ends |
| `power` from a process that is not `init` is `EPERM`, for every action including one that is not an action | **Any program could stop the machine.** **Observed**, Section 6.1, and it is the starkest of these |
| `pause` reports `EINTR` when a signal arrives, and the signal is delivered | A `pause` that returned at once — an `init` spinning for the machine's life — or one that slept through the signal and never woke |
| `init-check` left no open file and no child, and ended by its own choice | A test that passed because the program never ran |

The supervision and the shutdown themselves are judged by operating them, which
is [`../project/TESTING-GRAPHICS.md`](../project/TESTING-GRAPHICS.md), Section 9,
and the runs of [`../project/TESTING-RECORD.md`](../project/TESTING-RECORD.md).

### 6.1 The damage applied, and what the tests said

**The adoption that does not reparent** — the assignment removed, the count kept:

```
init: asserting the adoption of orphans, then running init-check.
  an orphan was not reparented to init
init-check: 0 assertion(s) failed.
init self-test FAILED.
```

**The authority removed from `power`** — every caller treated as `init`:

```
init: asserting the adoption of orphans, then running init-check.
init-check: power and pause, from privilege level 3.

It is now safe to turn off the machine.
--- End of captured serial output ---
VERIFICATION FAILED: the expected banner was not observed.
```

The boot does not finish. The self-test's own program asked for a halt, and with
the reservation gone it was given one — which is exactly the failure the
reservation prevents, made visible in the only way a machine can make it
visible.

### 6.2 Where this test stands in the sequence, and why that mattered

`KernelVerifyInit` runs **last**, after the shell's sessions. It was placed
before them at first, and Bochs — some hundred times slower than the machine
this is developed upon — failed the shell's job-control session there and
nowhere else, with its third `cat` ended by the wrong signal.

Nothing in either test was wrong. That session delivers two control bytes
through the bootstrap processor's tick and is sensitive to where the tick falls
against the shell's forking, as [`SHELL.md`](SHELL.md), Section 28, records; the
init test runs a program that forks a child which spins and then sleeps in
`pause` to be woken, and running it immediately before was enough to move the
tick. **The same image with the two in the other order is clean**, and the image
of sub-task 9.2 was clean under the same conditions, which is how the cause was
established rather than guessed. The order it now has is also the order the
machine itself has: `init` is the thing started after everything.

It is recorded here because a self-test that perturbs another self-test is a
defect of the test corpus, and one that only a slow machine reveals is one that
would otherwise have been called a flake.

## 7. Limitations

1. ~~**What `init` starts is written into `init`.**~~ **Closed at sub-task
   9.4**: the services are `[service]` blocks in `/etc/system.conf`, and
   [`CONFIG.md`](CONFIG.md), Section 4.1, is what each key means. What `init`
   still has written into it is the fallback it uses where that file cannot be
   read at all, which is deliberate and is recorded there.
2. **The shell is still the kernel's**, started in a loop the entry point holds
   upon the serial line, and is not `init`'s to supervise. Sub-task 9.6 puts a
   terminal emulator upon the desktop; that is when a shell becomes a thing
   `init` starts.
3. ~~**Supervision is a restart and nothing more.**~~ **Bounded at sub-task
   9.4**: a service that ends five times in a row is given up on and `init` says
   so. It is a count and not a rate, a rate needing a clock a program can read
   which this system still does not have, and the count is cleared only by
   `init` stopping the service itself — the one ending that says nothing about
   whether the program works. [`CONFIG.md`](CONFIG.md), Section 4.1.
4. **Nothing is told to stop but the desktop.** An orderly shutdown stops what
   `init` started and then stops the machine; a process that is nobody's child
   is not asked to finish, and a filesystem is not flushed — the ramdisk being
   memory, and a volume the machine carries being mounted read-only unless
   asked otherwise. A shutdown that unmounted is owed to whatever writes.
5. **`init` may be killed.** It ignores nothing and blocks nothing, so
   `kill -9` upon it leaves the machine with no supervisor, no collector and no
   way to stop in order; the kernel notices nothing. Real systems refuse the
   signal to process 1, and this one should when there is a reason beyond
   tidiness to.
6. **A restart depends upon the keyboard controller.** A machine whose 8042
   does not answer halts instead, which is stated where it happens and is the
   safe of the two outcomes; the `0xCF9` reset control register and the ACPI
   reset register are the two that would be tried next.
7. **The boot screen says one thing and never changes.** There is no progress
   in it, nothing being measured; it is a page, not an indicator.
