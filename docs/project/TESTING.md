<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# Testing: the Procedure and the Environments

**Authority**: `PROJECT_GUIDELINES.md`, Section 2, the testing mandate: every
milestone must be bootable and testable in QEMU and VirtualBox, UEFI testing
requires OVMF, and real-hardware compatibility must be considered from the first
ISO build.

**This document is how the machine is tested and where.** What each subsystem's
tests establish is elsewhere, and so is the record of what has actually been run:

| Document | Subject |
| -------- | ------- |
| **`TESTING.md`** (this one) | `make verify` and what it asserts; interactive execution under QEMU; the four further environments — OVMF, VirtualBox, Bochs and the physical machine — and what each is good for that the others are not; debugging with GDB; and the judges this project did not write. |
| [`BUILDS.md`](BUILDS.md) | The numbered register of every image produced, so that an observation made in one of those environments can name the image it was made about. Its record is [`builds.tsv`](builds.tsv), which carries the environments an image was run in as a field that can be queried: `tools/builds.sh query --environment Bochs`. |
| [`TESTING-SYSTEM.md`](TESTING-SYSTEM.md) | The verification of the devices, the storage stack, the privilege apparatus and the concurrency primitives. |
| [`TESTING-GRAPHICS.md`](TESTING-GRAPHICS.md) | The verification of the framebuffer, the primitives, the font and console, the optimisation, the fault screens, the compositor and the pointer. |
| [`TESTING-RECORD.md`](TESTING-RECORD.md) | The dated record of every test performed, with its outcome. |

**Why four.** This was one document of 1,757 lines, of which three quarters were
per-subsystem chapters that nobody reads in sequence and one eighth was a table
of dated results that everybody scrolls past them to reach. The division is the
one [`../design/ARCHITECTURE.md`](../design/ARCHITECTURE.md), Section 2.2, states
for a translation unit that has stopped being readable as one thing, applied to a
document: **along the lines a reader's question falls upon.** *How do I run it,
and where?* is this document. *What does the test of X establish?* is the two
middle ones. *Has it been run, and when?* is the record.

Nothing was reordered within a document, reworded or improved in passing. Every
line of the original stands in exactly one of the four; the sections were
regrouped by subject and renumbered, and every cross-reference the renumbering
broke was rewritten in the same change.

---

## 1. Automated verification

The `verify` target executes the ISO under QEMU without a display, directs the
serial port to a file, and makes **two** assertions upon the captured output.
Both are necessary, and either alone would pass a broken kernel.

**That the kernel reached the end of its initialisation**, asserted by the string
`initialisation complete.` appearing. The assertion is deliberately made upon
that fragment rather than upon the whole line, the line naming the sub-task most
recently completed and therefore changing with every advance of `PLAN.md`. This
catches a machine that faulted, hung or reset on the way there.

**That no boot-time self-test reported a failure**, asserted by the string
`FAILED` appearing nowhere. A self-test that fails states so and allows the
kernel to continue — there being no way to abandon a boot usefully and no harness
to report to — so a kernel whose every assertion failed would still reach the
banner, and the first assertion alone would call that a success. The self-tests
are the substance of this project's testing, and until sub-task 6.1 this target
could not see one fail.

The word is grepped for rather than each test being named, so that a self-test
added in a later phase is covered by this target on the day it is written. The
kernel emits `FAILED` in no other context; every occurrence is a verdict. The
target therefore requires no operator observation and no reading of its output,
and its exit status may be relied upon.

**From sub-task 3.7 the target always runs for the full 25 seconds.** The kernel
no longer halts at the end of initialisation; where a keyboard is present it
enters the echo loop of `docs/devices/KEYBOARD.md`, Section 7.2, and the run is ended by
the `timeout` that bounds it. The assertion is unaffected, the expected string
having been emitted before the loop is entered.

```sh
export PATH="$HOME/opt/cross/bin:$PATH"
make verify
```

The expected output at the completion of Phase 1 was as follows; the banner is
shown as it has read since 2026-09-15, one line where it was three:

```
Oxys-OS x86-64 UNRELEASED, Multiboot2 magic value verified
Multiboot2 information structure at physical address 0x11E4D8.
Multiboot2 information structure total size: 0x5D8 bytes.
Phase 1 initialisation complete.
No further subsystems are implemented. Halting.
VERIFICATION SUCCEEDED: the kernel booted and reported completion.
```

The physical address and the total size of the Multiboot2 information structure
are determined by GRUB and will vary between invocations and between versions of
GRUB. Their exact values are not part of the assertion.

### 1.1 Where the self-tests are

The self-tests are part of the kernel image, there being no harness to run them
in before Phase 7 and no userland to host one. They are implemented in
`kernel/test/`, one file per subsystem, and declared by
`kernel/include/oxys/test/verify.h`; `KernelMain` calls them in the order the
subsystems are initialised, because a test cannot run before the thing it
asserts exists.

`kernel/test/README.md` records the arrangement, the distinction between a
self-test and a diagnostic probe, and the limitations of both.

### 1.2 The assertion that was intermittent, and how it was closed

**`an admitted thread does not record its queue`**, in
`kernel/test/proc/sched.c`, was intermittent for four sub-tasks and is not any
longer. This section is kept rather than deleted, because how a defect of this
kind is found and closed is worth more than the fact that it is closed.

**The diagnosis was made from the code, three sub-tasks before it could be
reproduced at will.** The assertion is made *after* `SchedulerAdmit` has returned
and reads `thread->queued`; the machine has two processors and the fixture's
affinity names both, so the other processor may take the thread off the queue and
begin running it within that window. The race is in the test and not in the
scheduler: a thread that was admitted and then promptly run is a scheduler
working correctly, and the assertion could not tell that from a thread that was
never queued.

**It was seen at three different rates.** Once in about twenty runs during
sub-task 7.5; once in twelve during 7.3, recorded in
[`TESTING-RECORD.md`](TESTING-RECORD.md) with the diagnosis and left for whichever
sub-task next revisited the file; and then **once in three** during 7.7, which
changed nothing in the scheduler and merely shifted the boot's timing. That last
rate is what made it fixable: a change cannot be shown to have fixed a failure
nobody can produce.

**What it is now.** The queue a thread was put upon is asserted always, that field
being written once at admission and not cleared. Whether the thread is still upon
that queue is asserted only where the queue is the bootstrap processor's — the
admission and the read being made with this processor's interrupts masked, and no
other processor being able to take from this one's queue. A thread queued
elsewhere is left to the assertions about the rotation, which are not races.
[`../design/SCHEDULER.md`](../design/SCHEDULER.md), Section 7.3.

**A first attempt was rejected by measurement**, and that is the part worth
keeping. Pinning the fixture to the bootstrap processor made the fields stable
and passed four consecutive runs — and the report then read `the fixture ran upon
1 of 2 processor(s)`, a re-enqueue keeping a thread upon the processor that ran
it. The fix had removed the multi-processor rotation the test exists to
demonstrate, and only the report said so. **A test that reports a failure which is
not one is worse than no test**; a test that passes because it stopped testing
anything is worse than both.

## 2. Interactive execution under QEMU

```sh
make run-qemu
```

The machine type `q35`, the processor model `qemu64` and the two-core
configuration are fixed in the `Makefile` so that the symmetric multi-processing
work of Phase 6 is exercised under a representative configuration from the
earliest opportunity, as `PROJECT_GUIDELINES.md`, Section 2, requires.

The VGA console is expected to present the identification banner in light cyan
upon black, followed by the status lines. The serial output is directed to the
standard output stream of the invoking terminal.

**The default entry is quiet upon the screen, from 2026-09-15.** The boot log —
the self-test verdicts and device reports, some four hundred lines — is carried
by the serial line in every boot, which is what `make verify` and this target's
`-serial stdio` read; but the screen of the default entry shows the banner, one
line saying where the log went, and the shell's prompt. The **`Oxys-OS
(diagnostics)`** entry of the menu shows the log upon the screen as well, and is
the one to boot when a person at the machine is diagnosing a boot. It replaced
the "serial console diagnostics" entry, whose `serial=com1` option had been read
by nothing since Phase 1. `KernelPanic` restores the display whatever the entry,
a machine that has stopped having to say why. `kernel/kernel.c`,
`KernelDisplaySetQuiet`.

### 2.1 Typing at the shell, and driving it without a person

Since sub-task 8.1 the boot ends at a prompt rather than at the echo loop, and
`make run-qemu` leaves the invoking terminal attached to the shell over the
serial line: type at it. A serial terminal sends `CR` for Return and `DEL` for
Backspace, and the line editor accepts both alongside the keyboard's `LF` and
`BS`, so nothing needs configuring.

**The one property of the sub-task that no self-test can reach is that a `read`
of the terminal waits**, the self-test placing its session before the program
starts; so an interactive session is part of the evidence for 8.1 and not a
demonstration, and it can be made without a person. Give QEMU a socket for its
serial port and write the bytes a terminal would send:

```sh
qemu-system-x86_64 -machine q35 -cpu qemu64 -smp cores=2 -m 512M \
    -cdrom build/oxys.iso -display none -no-reboot \
    -chardev socket,id=s,path=/tmp/oxys-serial.sock,server=on,wait=off \
    -serial chardev:s
```

Then, from anything that can connect to a Unix socket, wait for `oxys$ ` and
send, for example, `wrold` `ESC [ H` `ESC [ C` `ESC [ 3 ~` `ESC [ C` `r`
`ESC [ F` `CR` — which is Home, Right, Delete, Right, `r`, End, Return — and
read back `sh: no tokeniser yet, so nothing runs: world`. `ESC [ A` recalls it;
control-D upon an empty line ends the shell and the kernel starts it again.
The rows of [`TESTING-RECORD.md`](TESTING-RECORD.md) for 8.1 were made this
way, and the bytes are the session `kernel/test/libc/line.c` uses, so a
difference between the two runs is a difference between the injected path and
the interrupt-driven one.

**Under VirtualBox the same can be done at the PS/2 keyboard**, which is the
path a person at the machine takes and the one the serial line does not
exercise: `VBoxManage controlvm "Oxys-OS" keyboardputscancode 23 a3 17 97`
presses and releases `h` and `i`, the make code and then the break code of
each, and `e0 4b e0 cb` is the left arrow. Section 4 holds the machine's
configuration and Section 4.1 the serial log the answer is read from.

## 3. Execution under UEFI firmware

```sh
make run-uefi
```

This target invokes QEMU with the OVMF firmware. It is expected to fail at
present, because the ISO carries only the legacy BIOS boot path of GRUB. The
target is provided in advance so that the UEFI work of Phase 12 has an
established point of entry; sub-task 12.7 will render it functional.

## 4. Execution under VirtualBox

```sh
make run-vbox
```

The target destroys any existing machine named `Oxys-OS`, creates a machine with
512 MiB of memory, two processors and legacy BIOS firmware, directs the first
serial port to `build/vbox-serial.log`, attaches the ISO to an IDE controller,
and starts the machine.

**Present status**: there is no `VBoxManage` upon the WSL2 `PATH`, so the target
above fails its own tool check. The Windows binary is nevertheless reachable at
`/mnt/c/Program Files/Oracle/VirtualBox/VBoxManage.exe`, and the test may be
performed by hand through it. Two adjustments are required, both because that
binary is a Windows program and understands no WSL2 path:

```sh
VB="/mnt/c/Program Files/Oracle/VirtualBox/VBoxManage.exe"
mkdir -p /mnt/c/Users/<user>/oxys-vbox
cp build/oxys.iso /mnt/c/Users/<user>/oxys-vbox/oxys.iso

"$VB" createvm  --name "Oxys-OS" --ostype Other_64 --register
"$VB" modifyvm  "Oxys-OS" --memory 512 --cpus 2 --firmware bios
"$VB" storagectl "Oxys-OS" --name "IDE" --add ide
"$VB" storageattach "Oxys-OS" --storagectl "IDE" --port 0 --device 0     --type dvddrive --medium 'C:\Users\<user>\oxys-vbox\oxys.iso'
"$VB" startvm "Oxys-OS" --type headless
```

The medium must be named by a path **the Windows binary can resolve**, and the
same applies to any file the machine is asked to write. That is not the same as
requiring the file to be on the Windows filesystem, and this document said it was
until the claim was tested on 2026-09-12. A WSL2 file is reachable from Windows
by its UNC path, and VirtualBox accepts one:

```sh
"$VB" storageattach "Oxys-OS" --storagectl "IDE" --port 0 --device 0 \
    --type dvddrive --medium '\\wsl.localhost\Ubuntu\home\<user>\oxys-os\build\oxys.iso'
```

So attached, the image booted, reached the banner and reported all 56 assertions
sound, and [`TESTING-RECORD.md`](TESTING-RECORD.md) holds the run. What is *not*
accepted is a WSL2 path written as WSL2 writes it — `/home/<user>/...` or
`/mnt/c/...` — because the binary is a Windows program.

Copying to the Windows filesystem, as above, therefore remains the simpler
recipe and is what the examples use; it is a convenience and not a requirement.
`make run-vbox` names `$(CURDIR)`, which is inside WSL2, and is correct as it
stands — the target fails on the tool check and not on the path.

### 4.1 The serial channel under VirtualBox

**VirtualBox presents a working 16550A and this kernel drives it**, so the
automated assertion of Section 1 can be performed there as it is under QEMU.

Observed at sub-task 7.2, under **VirtualBox 7.2.0r170228** upon the Windows host
of the WSL2 environment, with the machine configured exactly as `make run-vbox`
configures it — `--uart1 0x3F8 4 --uartmode1 file`:

```
Serial self-test: this line was carried by interrupt.
Serial self-test passed.
Serial adapter: base 0x3F8, divisor 1, requested 115200 baud, realised 115200 baud.
Serial adapter: 8 data bits, parity none, 1 stop bit(s), interrupt-driven upon line 4, unmasked.
Serial adapter: transmitted 6927, received 0, interrupts 55, queued 0, waits 0, line errors 0 (last status 0x0), receive overruns 0.
```

294 lines of boot log, 55 assertions reporting passed or sound, no verdict of
`FAILED`, and the adapter driven by interrupt throughout.
The image it came from was the first this project numbered;
[`TESTING-RECORD.md`](TESTING-RECORD.md) holds the run, and the register no
longer holds the row — see [`BUILDS.md`](BUILDS.md).

**This document asserted the opposite until that run**, having said since Phase 4
that VirtualBox had no serial channel at all; [`HISTORY.md`](HISTORY.md) records
the correction. The lesson is worth more than the archaeology and is the reason
this paragraph is here: **a claim about an environment is only as current as the
last run in it**, and nothing in `make lint` can check one. Where a statement here
says an environment cannot do something, the thing to do is try it.

Section 4.2 remains, because reading the screen is still the only way to see what
the *framebuffer* console draws, and because a machine whose serial adapter is
absent — the physical one of Section 5 is such a machine — leaves nothing else.

What can be read besides the serial port is the screen. Until sub-task 6.2 that was the VGA text
console; between sub-tasks 6.2 and 6.4 it was nothing at all, requesting a
framebuffer having put the adapter in a graphics mode with no console upon it;
and from sub-task 6.4 it is the graphical console, which draws the boot log upon
the framebuffer.

The kernel emits more of the log than the screen holds in any of those states —
80 by 60 characters at VirtualBox's 640 by 480 — so the procedure below is needed
to catch a particular line before it scrolls away.

### 4.2 Reading a self-test verdict that has scrolled away

Pause the machine while the line is still upon the screen, and photograph it:

```sh
"$VB" startvm "Oxys-OS" --type headless
sleep 5.9          # the boot menu, then the interval up to the line wanted
"$VB" controlvm "Oxys-OS" pause
"$VB" controlvm "Oxys-OS" screenshotpng 'C:\Users\<user>\oxys-vbox\s.png'
"$VB" controlvm "Oxys-OS" poweroff
```

The interval is found by bisection and jitters by some tenths of a second
between runs, so several attempts may be needed to place a particular line upon
the screen. It is a crude procedure, and until sub-task 7.2 it was the only one
available under VirtualBox; Section 4.1 records what changed. It remains the only
way to see what the framebuffer console draws, which the serial port does not
carry.

## 4A. Execution under Bochs

Bochs is a fifth environment, added at sub-task 7.2. The machine it presents is
[`../../tools/bochsrc.cfg`](../../tools/bochsrc.cfg), which is run from the
repository root:

```sh
bochs -q -f tools/bochsrc.cfg
```

It writes the boot log to `build/bochs-serial.log`, which is then read exactly as
`build/serial.log` is, and Bochs's own log to `build/bochs.log`. Both are in
`build/`, so they are ignored by git and removed by `make clean`.

**Until sub-task 7.5 there was no such file**, and this section said there could
not be one: a configuration naming absolute paths and a build of the emulator
this project does not produce would be a file that failed upon somebody else's
machine. Half of that was solved rather than endured. The two ROM lines name
`$BXSHARE`, which is the variable Bochs itself uses to find its BIOS images —
and which resolves to the configure-time default when it is not set in the
environment, so an ordinary installation needs nothing done to it — and the
medium and the two logs are relative to the repository root. Nothing in the file
belongs to one machine.

**The other half stands, and it is why this is still not a `make` target.** The
emulator must be built with the options below, and a target would run whatever
`bochs` is upon the `PATH` — which upon this machine has three times been a build
that cannot execute long mode at all. A target that silently ran the wrong
emulator would be worse than no target.

**What Bochs is good for that the others are not.** It is an interpreter and not
a virtualiser: every instruction is decoded and checked against the architecture,
and it reports in its own log what it considered wrong — an unsupported pixel
format, a `HLT` executed with interrupts masked, a paging structure with a
reserved bit set. QEMU's translation is faster and says nothing of the kind.
Against that, it is slow: a boot that takes two seconds under QEMU takes some
minutes, so it is a deliberate run and not a regression gate.

**The build matters, and the one this project first met could not run the
kernel at all.** A Bochs configured without `--enable-x86-64` reports a CPU whose
`CPUID` leaf `0x80000001` is zero — no long mode — and the kernel cannot leave
protected mode upon it; one configured without `--enable-smp` refuses
`cpu: count=2`. Both were the case at sub-task 7.2 and both were a property of
the local build rather than of Bochs. **Both were the case again at sub-tasks
7.3, 7.4, 7.6 and 7.7**, the installed binary having reverted to a default build
each time — five occasions now, which is a property of this environment and not
an accident, so **expect to rebuild before a Bochs run rather than discovering
that you must**.

**At sub-task 7.7 the rebuild was not made and the run was not claimed.** The
symptom was the full set — `>>PANIC<< numerical parameter 'n_processors' was set
to 2, which is out of range 1 to 1`, and then, with the count forced to one,
`wrong value for parameter 'model'`, `bochs --help cpu` listing eleven models of
which the highest is `atom_n270`. A build with no long mode cannot run this
kernel at all, so there was not even a reduced run to be had, and
[`STATUS.md`](STATUS.md) and [`TESTING-RECORD.md`](TESTING-RECORD.md) say that
rather than reporting a result. **An environment that was not exercised must be
recorded as not exercised**; a row that says "passed" because something else
passed is the failure this whole document exists to prevent.

The symptom to look for is `bochs --help cpu` listing
nothing above `atom_n270`, every model in
that list being 32-bit, and the run then failing at
`>>PANIC<< numerical parameter 'n_processors' was set to 2` or — with one
processor — silently, the kernel halting where it tries to enter long mode. The
configuration that works:

```sh
./configure --enable-x86-64 --enable-smp --enable-cpu-level=6 \
            --enable-pci --enable-cdrom --enable-long-phy-address --with-nogui
```

**Installing over the system copy needs a password, and the run does not need
the install.** At sub-task 7.6 `make install` could not be run, and the boot was
made against the freshly built binary where it stood:

```sh
BXSHARE=/usr/local/share/bochs ~/src/bochs/bochs -q -f tools/bochsrc.cfg
```

`BXSHARE` names the *installed* share rather than the source tree, because the
source tree holds the VGA BIOS as source and not as the `.bin` the configuration
asks for. That is worth knowing before the half hour the wrong `vgaromimage`
costs, which the note below records.

The `bochsrc` that boots it is [`../../tools/bochsrc.cfg`](../../tools/bochsrc.cfg),
which is in the repository and carries the reasoning for each value beside it.
The CPU model is named explicitly there because it must be: a default-built Bochs
and a 64-bit one do not agree upon what the default is, and the symptom of
leaving it out is a kernel that halts where it enables long mode.

`display_library: nogui` requires `--with-nogui`; a build without it reports
`display library 'nogui' not available` and there is no headless run to be had.
A boot takes some minutes and the captured file is then read exactly as
`build/serial.log` is.

The `BXVGA` messages about an unsupported guest pixel format are the display
stub declining to render a 32-bit framebuffer upon a display library that has
none, and are not the kernel's.

**A `bochsrc` whose `vgaromimage` is wrong produces four failures that look like
kernel regressions**, and it cost half an hour at sub-task 7.5. The symptom is
exactly this set and no other:

```
Display self-test FAILED.
Framebuffer self-test FAILED.        (preceded by "the boot loader described no display")
Compositing self-test FAILED.
Serial self-test FAILED.             ("A sequence did not return unaltered through the loopback")
```

The same image under QEMU and VirtualBox passes every one of them. The cause is
that the VGA BIOS is what implements the VBE calls GRUB uses to honour the
Multiboot2 framebuffer request, so a machine without one presents a text-mode
adapter and no linear framebuffer — and the display, framebuffer and compositing
tests are then asserting against hardware that is not there. The serial failure
is the same configuration error rather than a related one: a `bochsrc` with a
mistaken ROM line usually has more than one.

**The mistake that produced it is worth naming**, because it is easy to repeat: a
`sed` expression written to replace the `romimage:` line also matches
`vgaromimage:`, that string containing the other. Anchor it — `s|^romimage:|…|`.

**The rule to take from it**: a failure that appears under Bochs and under
neither of the other two is a failure to suspect the `bochsrc` for first. This
project's self-tests are hardware-dependent by design, and Bochs is the
environment where the hardware is described by a file a person wrote. Since
sub-task 7.5 that file is [`../../tools/bochsrc.cfg`](../../tools/bochsrc.cfg)
and is in the repository, which is the other reason it is worth having: a
configuration nobody retypes is a configuration nobody mistypes.

## 5. Testing upon physical hardware

The ISO produced by `grub-mkrescue` is a hybrid image and may be written
directly to a USB medium:

```sh
sudo dd if=build/oxys.iso of=/dev/sdX bs=4M status=progress conv=fsync
```

The device name must be confirmed before the command is issued, since an
incorrect name will destroy the contents of the named device.

Physical testing requires a machine offering a legacy BIOS or a compatibility
support module, since the UEFI boot path is not implemented until Phase 12.
Diagnostic output should be captured through a serial adapter where the machine
provides one, and read from the screen where it does not.

**Sub-task 1.12 is closed**, upon the criterion the project owner set on
2026-09-07: **one machine, booted from a USB medium, with the boot log read and
recorded**. The machine and the run are Section 5.1.

### 5.1 The machine the storage work was reported from

One physical machine has run this kernel, and every fault recorded in
[`../storage/DISK.md`](../storage/DISK.md), Sections 2.1 to 2.3, was reported
from it. It is named here once, and the other documents cite this section rather
than restating it.

| | |
| --- | --- |
| Model | HP Laptop 14-dq0052dx |
| Processor | Intel Celeron N4120 — Gemini Lake Refresh, **four cores**, four threads, 1.10 GHz base and 2.60 GHz burst, 14 nm, 6 W |
| Memory | 4 GB DDR4 |
| Storage | **64 GB eMMC**, and no disk of any other kind |
| Graphics | Intel UHD Graphics 600 |
| Display | 14-inch, 1366 × 768 |
| Booted from | A USB drive |

**Three of those lines are why the machine mattered**, and none of them was
chosen for the purpose:

Its storage is an **embedded MultiMediaCard** part, so the machine carries no
mass-storage controller of any class. That is what sub-task 4.8 was added for,
and it is why the report had to learn to name storage outside the mass-storage
class rather than say there was none.

It has **four cores**, where the QEMU configuration this project verifies against
runs two. The application-processor bring-up of sub-task 6.14 has therefore not
yet met a real machine, and when it does it will meet one with more processors
than any test has used — which exercises the reservation of `PER_CPU_MAXIMUM`,
the serial bring-up of [`../design/SMP.md`](../design/SMP.md), Section 7, and the
refusals of its Section 7.1, none of which QEMU's two processors reach.

It is a **UEFI-era machine**, and the kernel has no UEFI boot path until Phase
12, so booting it here went through the firmware's compatibility support module.
That is the one thing about this result which does not generalise: a machine
whose firmware offers no such module cannot boot this kernel at all before
sub-task 12.7.

### 5.2 How the boot log was read, there being no serial channel

**The machine has no serial adapter, and this kernel cannot give it one.** Its
external ports are USB Type-C and Type-A, HDMI, a headphone jack and the power
connector; there is no DE-9 port and no 16550 at `0x3F8` for `SerialInitialise`
to find. A USB-to-serial adapter would not help, there being no USB stack in this
kernel to drive one — and reaching a USB device is a longer road than this
project has taken, as [`../storage/DISK.md`](../storage/DISK.md), Section 2.3,
records.

So the log was read **from the screen**, upon the graphical console of sub-task
6.4. This is the same condition VirtualBox presents and it has the same two
consequences, set out in Section 4.1: the automated assertion of Section 1 cannot
be performed here, that assertion being made upon serial output; and the kernel
emits more of the log than the screen holds, so a particular line must be caught
before it scrolls away by the procedure of Section 4.2.

**That is what closes sub-task 1.12 rather than a serial capture**, and the
criterion was set knowing it. It is also the clearest vindication of sub-task
6.4: between sub-tasks 6.2 and 6.4 this machine would have booted and shown
nothing whatever, the framebuffer request having put the adapter into a graphics
mode with no console upon it. A boot that cannot be read is not a boot that has
been verified.

**What the run established** is that the kernel boots from a USB medium upon real
hardware, reaches the end of its initialisation, and draws its log where a person
can read it. **What it did not establish** is anything about the phases it passed
through on the way: nothing was inspected there beyond the storage report, which
is why Section 3 of [`STATUS.md`](STATUS.md) records those phases as reached and
not as examined. The faults that run did find are Sections 2.1 to 2.3 of
[`../storage/DISK.md`](../storage/DISK.md), and they are the reason sub-tasks 4.7
and 4.8 exist.

## 6. Debugging with GDB

QEMU provides a GDB stub. The kernel is compiled with `-g`, so the DWARF
information in `build/oxys.elf` may be used directly:

```sh
qemu-system-x86_64 -machine q35 -cpu qemu64 -smp cores=2 -m 512M \
    -cdrom build/oxys.iso -s -S &
gdb build/oxys.elf -ex 'target remote localhost:1234'
```

Note that breakpoints upon higher-half symbols cannot be serviced until paging
has been enabled. A breakpoint at `_start`, whose address is physical, is the
correct point at which to begin an examination of the boot sequence.

## 7. Judges this project did not write

Every section above this one describes machinery this project wrote, asserting
against fixtures this project composed. That is a closed loop, and its
characteristic failure is **agreement**: a misreading of a specification is
composed into the fixture, read back by the decoder that shares the misreading,
and every assertion passes. The self-tests then establish self-consistency,
which is a real property and not the one wanted.

The remedy is an independent judge — something built by other people, from the
same specification, sharing none of this project's assumptions. The value of one
is inversely proportional to how much it has in common with the code it judges.

**The case that proves it.** `e2fsck` found the defect in the recorded deletion
time described in [`../storage/VFS.md`](../storage/VFS.md), Section 11.1. `i_dtime`
is overloaded in EXT2 — a deletion time, or the link to the next inode upon the
orphan list, distinguished by magnitude — so the constant 1 recorded for want of
a clock made every freed inode appear orphaned. Every assertion in
`kernel/test/` passed, and the volume was nevertheless wrong. No fixture this
project composed could have caught it, because the fixture would have been
composed with the same misunderstanding.

### 7.1 The judges presently used

| Judge | What it is independent of | Where |
| ----- | ------------------------- | ----- |
| `grub-file --is-x86-multiboot2` | This project's reading of the Multiboot2 header format. Run at every link. | `Makefile`, the `all` target |
| `e2fsck`, `debugfs`, `dumpe2fs` | This project's reading of EXT2. | Sections 7 and 12.1 |
| `clang` | `x86_64-elf-gcc`, and therefore what one toolchain tolerates. | [`TOOLCHAIN.md`](TOOLCHAIN.md), Section 9 |
| Two independent renderings of a specification | A single transcription of a document not publicly distributed. | [`REFERENCES.md`](REFERENCES.md) |
| Real hardware | Every emulator's approximation of a machine. | Sections 10.1 and 10.2 |

Each has already returned something. `grub-file` fails the build outright if the
header is malformed. `e2fsck` found the defect above. `clang` found, upon its
first run, that `kernel/arch/x86_64/cpu/tss.c` named a 32-bit register to an instruction the
architecture defines upon r/m16 — which GNU `as` had accepted, and assembled
correctly, for as long as the file existed. The physical machine of [`TESTING.md`](TESTING.md), Section 5.1
found two, and both changed the design rather than the code.

### 7.2 The judges available and not yet used

Recorded because the argument above applies to them equally, and because a list
of what has not been done is worth more than a resolution to do it.

**Bochs.** QEMU is permissive: it tolerates malformed descriptors, reserved bits
set where the architecture requires them clear, and questionable model-specific
register writes. Bochs is pedantic and logs its objections. For a kernel that has
built a global descriptor table, a task state segment, an interrupt descriptor
table with interrupt stack tables and a `SYSCALL` configuration, that is an
independent referee upon exactly the structures whose errors are least visible —
the processor reads them directly, and its objections are exceptions rather than
return codes.

**Differential testing against Linux for EXT2.** Mount a volume this kernel wrote
and compare its contents; write files with Linux and read them back with this
kernel. Two implementations, one input, the outputs compared. It is the strongest
form of independent judgement available, and the virtual filesystem layer is now
complete enough to submit to it.

**`readelf` and `objdump` upon the composed ELF images.**
`kernel/test/exec/elf.c` assembles images to be wrong on purpose. Two questions
follow: whether `readelf` agrees the *valid* one is well formed, and whether the
loader accepts the output of a real `x86_64-elf-gcc`. It has so far seen only
images this project composed.

**The QEMU monitor** — `info mem`, `info tlb`, `info registers`. QEMU walks the
paging hierarchy with its own code and reports what is mapped. At present the
hierarchy is checked by `PagingTranslate` agreeing with `PagingMapPage`, which
are both this project's and can share a misconception — as they did until
sub-task 6.7, when intermediate entries were found never to have carried the user
bit.

