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

The expected output at the completion of Phase 1 is:

```
Oxys-OS
Version 0.1.0, x86_64, long mode active, higher-half kernel.
Multiboot2 magic value verified.
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
`kernel/include/oxys/verify.h`; `KernelMain` calls them in the order the
subsystems are initialised, because a test cannot run before the thing it
asserts exists.

`kernel/test/README.md` records the arrangement, the distinction between a
self-test and a diagnostic probe, and the limitations of both.

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

The ISO must be staged upon the Windows filesystem and named by a Windows path;
the same applies to any file the machine is asked to write.

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
[`BUILDS.md`](BUILDS.md), build 1, is the image it came from.

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

Bochs is a fifth environment, added at sub-task 7.2. It is not a `make` target:
it requires a configuration file naming absolute paths and a build of the
emulator this project does not produce, and a target that assumed either would be
a target that failed upon somebody else's machine.

```sh
bochs -q -f bochsrc
```

with a `bochsrc` naming the ISO as an ATA CD-ROM, `boot: cdrom`, and
`com1: enabled=1, mode=file, dev=<path>` — after which the captured file is read
exactly as `build/serial.log` is.

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
the local build rather than of Bochs. **Both were the case again at sub-task
7.3**, the installed binary having reverted to a default build: the symptom to
look for is `bochs --help cpu` listing nothing above `atom_n270`, every model in
that list being 32-bit, and the run then failing at
`>>PANIC<< numerical parameter 'n_processors' was set to 2` or — with one
processor — silently, the kernel halting where it tries to enter long mode. The
configuration that works:

```sh
./configure --enable-x86-64 --enable-smp --enable-cpu-level=6 \
            --enable-pci --enable-cdrom --enable-long-phy-address --with-nogui
```

The `bochsrc` that boots it, with the CPU model named explicitly because a
default-built Bochs and a 64-bit one do not agree upon what the default is:

```
megs: 512
cpu: count=2, ips=100000000, model=corei7_sandy_bridge_2600k
romimage: file=/usr/local/share/bochs/BIOS-bochs-latest
vgaromimage: file=/usr/local/share/bochs/VGABIOS-lgpl-latest.bin
ata0: enabled=1, ioaddr1=0x1f0, ioaddr2=0x3f0, irq=14
ata0-master: type=cdrom, path=<absolute path to build/oxys.iso>, status=inserted
boot: cdrom
com1: enabled=1, mode=file, dev=<absolute path for the captured log>
display_library: nogui
```

`display_library: nogui` requires `--with-nogui`; a build without it reports
`display library 'nogui' not available` and there is no headless run to be had.
A boot takes some minutes and the captured file is then read exactly as
`build/serial.log` is.

The `BXVGA` messages about an unsupported guest pixel format are the display
stub declining to render a 32-bit framebuffer upon a display library that has
none, and are not the kernel's.

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
`kernel/test/verify_elf.c` assembles images to be wrong on purpose. Two questions
follow: whether `readelf` agrees the *valid* one is well formed, and whether the
loader accepts the output of a real `x86_64-elf-gcc`. It has so far seen only
images this project composed.

**The QEMU monitor** — `info mem`, `info tlb`, `info registers`. QEMU walks the
paging hierarchy with its own code and reports what is mapped. At present the
hierarchy is checked by `PagingTranslate` agreeing with `PagingMapPage`, which
are both this project's and can share a misconception — as they did until
sub-task 6.7, when intermediate entries were found never to have carried the user
bit.

