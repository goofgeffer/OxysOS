# Oxys-OS Test Procedure

**Corresponding phase**: All phases. `PROJECT_GUIDELINES.md`, Section 2, requires every
milestone to be bootable and testable under QEMU and VirtualBox.

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

## 3. Verification of the text-mode display

The display driver is asserted at each boot by `KernelVerifyVga`. It guards a
class of failure which is silent to the machine and visible only to a person
reading the screen: a control character for which the driver has no case is
written into the frame buffer as whatever glyph the adapter's font holds at that
code point, and the cursor then advances rightward. The backspace was broken in
exactly that way until 2026-08-31.

From sub-task 4.2 the test covers the whole of the driver, and each property it
asserts is chosen because its failure would otherwise be invisible: the adapter's
register configuration, the disabling of blinking through the attribute
controller, the cursor movements of every control character, the erase limit and
the backspace that crosses into the row above, the agreement between the driver's
cursor position and the one read back out of the CRT controller, the refusal of a
position outside the display and of an impossible cursor shape, the hiding and
restoration of the hardware cursor, and a scroll that moves the display by
exactly one row. The table in `docs/devices/DISPLAY.md`, Section 8.1, pairs each property
with the failure it would catch.

The scroll assertion reads the frame buffer back through `VgaCharacterAt` and
costs one row of the boot log, which leaves the top of the display for the
purpose. The record upon the serial line, which is what `make verify` reads, is
unaffected.

What the self-test cannot establish is that the frame buffer is rendered at all.
The serial path and the display path are independent, and a defect in the VGA
driver would not be detected by the serial assertion alone. The display is
therefore also verified by capturing a screen image through the QEMU monitor:

```sh
( sleep 10; echo "screendump /tmp/oxys.ppm"; sleep 3; echo "quit" ) \
  | qemu-system-x86_64 -machine q35 -cpu qemu64 -smp cores=2 -m 512M \
      -cdrom build/oxys.iso -display none -monitor stdio -serial null
```

The resulting image is 720 by 400 pixels, which is the pixel resolution of VGA
mode 3, and contains the rendered banner. This was performed at the completion of
Phase 1 and the banner was confirmed to read `Oxys-OS`.

## 4. Verification of the serial receive path

The self-tests cannot establish that a character arrives from outside the
machine; they can only establish what happens to one that already has. The echo
loop the kernel enters at the completion of initialisation drains the serial
receive buffer as well as the keyboard's, so the path may be driven from the
host by attaching the emulated line to the standard input stream:

```sh
( sleep 9; printf 'serial-in-works'; sleep 4 ) \
  | qemu-system-x86_64 -machine q35 -cpu qemu64 -smp cores=2 -m 512M \
      -cdrom build/oxys.iso -display none -monitor none -serial stdio
```

The typed characters are expected to appear upon the captured output after the
echo loop's banner, having traversed the adapter, IRQ4, the interrupt
controller, the handler and the receive buffer. The delay before the text is
sent must exceed the time the self-tests take, since anything arriving earlier is
discarded by `SerialFlushBuffers` at the close of the serial self-test.

## 5. Verification of the backspace across a row boundary

A backspace in the first column carries the cursor into the row above, as far
back as the erase limit and no further. The self-test asserts the movement upon
the driver's own state; that the movement is produced by a real keystroke, and
that the serial terminal is told of it, must be driven from outside.

Both sources of characters were exercised for sub-task 4.2. Over the serial line:

```sh
( sleep 6; printf 'ab\ncd\b\b\b\b\b'; sleep 4 ) \
  | qemu-system-x86_64 -machine q35 -cpu qemu64 -smp cores=2 \
      -cdrom build/oxys.iso -display none -serial stdio
```

and by scan code, through the QEMU monitor:

```sh
( sleep 7; for k in a b ret c d backspace backspace backspace backspace backspace; \
      do echo "sendkey $k"; sleep 0.3; done; sleep 2; echo quit ) \
  | qemu-system-x86_64 -machine q35 -cpu qemu64 -smp cores=2 \
      -cdrom build/oxys.iso -display none -monitor stdio -serial file:kbd.log
```

Both produced the identical echo, shown here with the control characters made
visible by `cat -v`:

```
ab
cd^H ^H^H ^H^[[A^[[3G ^[[3G^H ^H^H ^H
```

The first two backspaces erase `d` and `c` within the row. The third finds the
cursor in the first column, so the display crosses into the row above and stops
immediately after `ab`, in column 3 counting from one; the separator between the
rows is consumed and no character is. The serial terminal is told of that
movement by ECMA-48 CUU followed by CHA to column 3, a space — which lands upon a
column that was already blank — and CHA again. The fourth and fifth backspaces
then erase `b` and `a` in the ordinary way. A sixth would do nothing at all, the
cursor then standing at the erase limit, which the echo loop set below its own
banner.

## 6. Verification of the disk

`make verify` runs upon the q35 board, whose storage controller is AHCI; no
device answers the ATA driver there, and the disk self-test reports as much and
asserts nothing. That is the correct outcome upon that machine and is not a
failure.

The disk is exercised upon the i440fx board, which presents the PIIX3 IDE
controller at `0:1.1` in compatibility mode. The image is created sparse and
larger than 28-bit addressing can name, so that the 48-bit commands — whose
register discipline differs in kind and not merely in width — are reachable at
all; it occupies a few kilobytes upon the host.

```sh
qemu-img create -f raw disk.img 256G
# seed sector 0, sector 1, and sector 0x10000001 with distinguishable text

qemu-system-x86_64 -machine pc -cpu qemu64 -smp cores=2 -m 512M \
    -cdrom build/oxys.iso \
    -drive file=disk.img,format=raw,if=ide,index=0,media=disk \
    -display none -serial file:ata.log
```

The driver is expected to report a disk of 536870912 sectors with 48-bit
addressing upon the primary master, and the optical drive holding the ISO as an
ATAPI packet device upon the secondary master — recognised by its signature
rather than mistaken for a disk.

The self-test cannot know what the medium holds, so the seeded content is
confirmed from outside: each seeded sector was read back by the driver and
compared against what was written to the image, including the sector beyond the
28-bit limit.

### 6.1 The write path

The driver writes only when the operator has asked for it. The GRUB entry
*Oxys-OS (disk write self-test)* passes the option `disk-write-test`, upon which
the self-test reads the final sector, writes a pattern, reads it back, compares
it byte for byte, and restores the sector from what it first read, verifying the
restoration in its turn. Anybody may boot this kernel upon their own machine, and
a self-test that wrote to their disk unbidden would destroy their data; see
`docs/storage/DISK.md`, Section 6.

The entry is selected at the menu. For an unattended run, an ISO may be generated
with `set default=2` in place of `set default=0`:

```sh
sed 's/^set default=0/set default=2/' boot/grub/grub.cfg > isodir/boot/grub/grub.cfg
grub-mkrescue -o write.iso isodir
```

## 7. Verification of the EXT2 superblock

The parser is asserted at every boot against a volume composed within the
memory-backed block device, which is what makes it verifiable upon a machine with
no disk. That establishes the parser consistent with itself; the corroboration
must come from a volume built by something else.

```sh
mke2fs -q -t ext2 -b 1024 -L oxys-root -F ext2.img 16384

qemu-system-x86_64 -machine pc -cpu qemu64 -smp cores=2 -m 512M \
    -cdrom build/oxys.iso \
    -drive file=ext2.img,format=raw,if=ide,index=0,media=disk \
    -display none -serial file:ext2.log
```

Every figure the kernel reports is then compared against `dumpe2fs -h` upon the
same image: the block and inode counts, both free counts, the block size, the
geometry of the groups, the inode size, the first usable inode and the three
feature words. The reported group line is compared against `dumpe2fs` in full —
the two bitmap blocks, the first block of the inode table, the two free counts
and the directory count — and the absence of a complaint about the table means
the free counts of every group summed to the totals the superblock states.

The root inode is read and its blocks resolved upon every device at every boot,
so any volume the machine carries exercises the inode code as well. It is
compared against `debugfs -R "stat <2>"` upon the same image: the mode, the size,
the link count, the sector count and the block list. An image whose root
directory is large enough to need the indirect blocks is made with `mke2fs -d`
from a directory of many files, since a root of one block would exercise nothing
but the first of the fifteen pointers. A second image of 4096-byte blocks exercises the other block size
this kernel accepts, and a disk holding no filesystem is expected to be refused
for want of the magic number.

## 8. Execution under UEFI firmware

```sh
make run-uefi
```

This target invokes QEMU with the OVMF firmware. It is expected to fail at
present, because the ISO carries only the legacy BIOS boot path of GRUB. The
target is provided in advance so that the UEFI work of Phase 12 has an
established point of entry; sub-task 12.7 will render it functional.

## 9. Execution under VirtualBox

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

### 9.1 There is no serial channel under VirtualBox

The serial port is omitted from the commands above deliberately. The kernel does
not detect VirtualBox's 16550A: it reports `Serial self-test skipped; no adapter
is present.` and `Serial adapter: absent; no diagnostic channel.`, claims no
request line, and therefore transmits nothing. A `--uartmode1 file` log is
written as an empty file and a `--uartmode1 tcpserver` socket accepts a
connection and delivers no byte. This is a property of the machine and not of any
one sub-task; it predates the tests recorded here and is not investigated by
them.

The consequence is that **the automated assertion of Section 1 cannot be
performed under VirtualBox**, that assertion being made upon the serial output.

What can be read instead is the screen. Until sub-task 6.2 that was the VGA text
console; between sub-tasks 6.2 and 6.4 it was nothing at all, requesting a
framebuffer having put the adapter in a graphics mode with no console upon it;
and from sub-task 6.4 it is the graphical console, which draws the boot log upon
the framebuffer.

The kernel emits more of the log than the screen holds in any of those states —
80 by 60 characters at VirtualBox's 640 by 480 — so the procedure below is needed
to catch a particular line before it scrolls away.

### 9.2 Reading a self-test verdict that has scrolled away

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
the screen. It is a crude procedure and it is the only one available while the
machine has no serial channel.

## 10. Testing upon physical hardware

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
provides one. Sub-task 1.12 remains open.

## 11. Debugging with GDB

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

## 12. Verification of the virtual filesystem layer

The layer is asserted at every boot against two volumes composed within two
memory-backed block devices, which is what makes it verifiable upon a machine
with no disk. The properties asserted, and the silent failure each would catch,
are tabulated in [`../storage/VFS.md`](../storage/VFS.md), Section 10.

The corroboration must come from a volume built by something else, and it is
performed with four images and two boots of each.

```sh
# A volume with a directory, a file within it, a symbolic link, and a regular
# file for the write probe to act upon. The probe never creates one, so an image
# without it is left untouched.
mkdir -p seed/sub
printf 'corroboration' > seed/hello.txt
printf 'placeholder'   > seed/oxys-write-test
printf 'inner'         > seed/sub/inner.txt
ln -sf sub seed/link

mke2fs -q -t ext2 -b 1024 -L oxys-probe -d seed -F probe.img 16384
mke2fs -q -t ext2 -b 4096 -L oxys-4k    -d seed -F big.img   16384

qemu-system-x86_64 -machine pc -cpu qemu64 -smp cores=2 -m 512M \
    -cdrom build/oxys.iso \
    -drive file=probe.img,format=raw,if=ide,index=0,media=disk \
    -display none -serial file:probe.log
```

The default GRUB entry mounts the volume **read-only**; the entry "Oxys-OS (EXT2
write self-test)" mounts it for writing, performs the probe of Section 12.1, and
then withdraws it and mounts it afresh read-only.

What is compared, and against what:

| The kernel reports | Compared against |
| ------------------ | ---------------- |
| The mount line: the device, the type, the block size and whether it is writable. | `dumpe2fs -h`, for the block size; the GRUB entry, for the writability. |
| The listing of `/`: an inode number, a type and a name for each entry. | `debugfs -R "ls -l /"` upon the same image, which must agree in every column. |
| The number of nodes held and descriptors open once the self-test has finished. | Must be exactly one and zero: the root node the mount holds, and nothing else. |

And what is examined upon the image afterwards:

| Examined | Expected |
| -------- | -------- |
| `dumpe2fs -h` after a **read-only** mount. | `Filesystem state: clean`, `Mount count: 0`. The volume is untouched, byte for byte. |
| `dumpe2fs -h` after a **writable** mount that was never withdrawn. | `Filesystem state: not clean` — and *not* "with errors" — with `Mount count: 1`. This is the mark of Section 8 of `VFS.md`, and it persists precisely because the machine stopped while the volume was open. |
| `dumpe2fs -h` after a writable mount that **was** withdrawn. | `Filesystem state: clean`, `Mount count: 1`. |
| `e2fsck -fn` in every case. | No error through all five passes. |

### 12.1 The write probe

Under the write-permitting entry alone, and only where the volume already holds a
regular file named `/oxys-write-test`, the kernel opens that file through the
layer with a truncation, writes 5000 bytes derived from their own offsets, seeks
back to the beginning of the same descriptor, reads the whole of it back and
compares it. Five thousand bytes is deliberately more than one block of either
block size, so the write and the read both cross a block boundary and the
descriptor's position is carried across it — which is the whole of what the layer
adds to the write of sub-task 5.6.

It is then confirmed from outside:

```sh
debugfs -R "stat /oxys-write-test" probe.img     # size, block count, mode
debugfs -R "dump /oxys-write-test out.bin" probe.img
xxd out.bin | head                               # bytes are ((offset*31)+7) & 0xFF
e2fsck -fn probe.img
```

The contents depend upon the offset rather than being a constant or a pattern
repeating every block, so a file written from the wrong place is distinguishable
from one written correctly when it is examined from outside — which a constant
fill would not be.

**This corroboration found a defect**, and it is the kind that only a tool
outside the kernel can see: the operation reported success and the volume read
back correctly, and `e2fsck` nevertheless reported every inode the kernel had
freed as the member of a corrupted orphan list. It is recorded in
[`../storage/VFS.md`](../storage/VFS.md), Section 11.1.

## 13. Verification of the privilege apparatus

The descriptors, the task state segment, the interrupt stack table and the three
system-call registers of sub-task 6.1 are asserted at every boot by
`KernelVerifyPrivilege`. Each assertion, and the silent failure it catches, is
tabulated in [`../design/PRIVILEGE.md`](../design/PRIVILEGE.md), Section 7.

Two of the five parts do more than inspect a structure, and they are the two
worth describing here, because inspecting a structure the processor reads
establishes only what this kernel wrote into it.

**The interrupt stack table is exercised.** Vector 200 — clear of everything the
machine uses — is registered, raised by `int $200`, then given the double fault's
interrupt stack table entry and raised again. The evidence is the address at
which the trap frame was built, that frame being the first thing placed upon
whatever stack the processor selected. The two addresses must differ, the first
must lie outside the double-fault stack and the second within it. The double
fault itself cannot be raised for this: its handler is fatal by design.

**The transition is exercised.** `SYSCALL` may be executed from privilege level
0. It raises no privilege, there being none to raise, but it loads `CS` and `SS`
from `IA32_STAR`, transfers to `IA32_LSTAR`, saves the return address in `RCX`
and the flags in `R11`, and clears the bits `IA32_FMASK` names — so the whole
mechanism is exercisable now, with no user program, no user mapping and no user
stack. The provisional entry point records what the processor loaded, those
values existing nowhere else. It is executed twice, once with the interrupt flag
clear and once with it set, because the assertion that `IA32_FMASK` cleared the
flag says nothing whatever if the flag was already clear.

### 13.1 The negative test

A self-test is worth nothing until it has been seen to fail. To repeat it:

```sh
# Remove the interrupt flag from the mask the kernel writes into IA32_FMASK.
sed -i 's/RFLAGS_TRAP | RFLAGS_INTERRUPT_ENABLE |/RFLAGS_TRAP |/'     kernel/include/oxys/syscall.h
make verify
```

The run must report two failures and end `Privilege self-test FAILED.` — the
configuration assertion of Section 7.4 of the design document and the exercised
assertion of Section 7.5, which is the pair that distinguishes "the register does
not say so" from "the processor did not do so":

```
  IA32_FMASK does not clear the interrupt flag, so the kernel would be entered
    interruptible upon a user stack
  the interrupt flag was still set within the handler, so the kernel was entered
    interruptible
Privilege self-test FAILED.
```

Restore the file afterwards.

`make verify` reports `VERIFICATION FAILED` and names the offending line. That
was not so when this test was first performed: the harness then asserted only the
completion banner and knew nothing of any self-test's verdict, so a run in which
every assertion failed still succeeded. Section 1 records the second assertion
that closed it.

## 15. Verification of the framebuffer

The description, the mapping and the memory type of the framebuffer acquired by
sub-task 6.2 are asserted at every boot by `KernelVerifyFramebuffer`. Each
assertion, and the silent failure it catches, is tabulated in
[`../design/GRAPHICS.md`](../design/GRAPHICS.md), Section 8.

**One thing the kernel cannot assert about a display is that anything appeared
upon it.** A framebuffer that is mapped, written and read back correctly may
still be scanned out by nothing at all. That half of the verification is
performed by a person, and the self-test paints a pattern for them to judge:
bands of red, green and blue across the top sixteenth of the screen, and a single
white pixel in the very last position of the last row.

### 15.1 Capturing the pattern

**The second menu entry must be selected.** From sub-task 6.4 the console owns
the screen and would erase the pattern within the same boot, so the figures are
painted only when the command line carries `graphics-figure` — which the entry
**Oxys-OS (graphics figures)** passes, and which suppresses the console for that
boot. The assertions of both self-tests run either way; it is only the drawing
that this governs. See [`../design/GRAPHICS.md`](../design/GRAPHICS.md),
Section 19.5.

The entry is reached by sending a keystroke to the boot menu through the QEMU
monitor:

```sh
( sleep 2;  echo "sendkey down"; sleep 0.3; echo "sendkey ret"; \
  sleep 12; echo "screendump /tmp/oxys-fb.ppm"; \
  sleep 3;  echo "quit" ) \
  | qemu-system-x86_64 -machine q35 -cpu qemu64 -smp cores=2 -m 512M \
      -cdrom build/oxys.iso -display none -monitor stdio -serial null
```

A capture that shows text rather than the figures is a capture of the default
entry: the keystroke arrived before the menu was drawn, or after the three-second
timeout had elapsed.

The image is a binary PPM, whose header states the mode the boot loader chose.
Four things are read from it, and each establishes something the kernel cannot
establish for itself:

| What to look for | What its absence would mean |
| ---------------- | --------------------------- |
| The bands are **red, then green, then blue**, left to right | The channel positions or widths were misread; the kernel would be writing blue where it meant red, with nothing to report it. |
| They are **flat**, not sloping | The pitch is wrong. A traversal stepping by the occupied width rather than the pitch shears the image progressively down the screen. |
| They reach the **right-hand edge** | The width or the pitch is short. |
| The **last pixel of the last row is white** | The mapping is short by less than a page — an amount every assertion made upon the start of the range would pass. |

The bands occupy the top sixteenth of the screen; the remainder is black, being
memory nothing has written.

### 15.2 The negative test

To confirm the self-test can fail, change the memory type written into the page
attribute table from write-combining to write-back:

```sh
sed -i 's/FRAMEBUFFER_PAT_ENTRY_WC   UINT64_C(0x01)/FRAMEBUFFER_PAT_ENTRY_WC   UINT64_C(0x06)/'     graphics/framebuffer.c
make verify
```

The run must report `entry 4 of IA32_PAT does not hold write-combining, so the
framebuffer is write-back and the display may lag the memory indefinitely` and
end `Framebuffer self-test FAILED.` Restore the file afterwards.

### 15.3 What the display self-test does now

It is skipped. Requesting a framebuffer causes the boot loader to set a graphics
mode, and every assertion the display test makes reads a character cell back out
of the text buffer at `0xB8000`, which in a graphics mode is not the text buffer.
The expected line is:

```
Display self-test skipped; the adapter is in a graphics mode, which the framebuffer owns.
```

A run in which it is *not* skipped is a run in which the boot loader left the
adapter in a text mode, and the display test then applies as it always did. Both
are correct; which occurs is the boot loader's decision. See
[`../design/GRAPHICS.md`](../design/GRAPHICS.md), Sections 2.1 and 7.

## 16. Verification of the drawing primitives

The primitives of sub-task 6.3 are asserted at every boot by
`KernelVerifyGraphics`, **against a surface composed in memory and not against
the framebuffer**. Every assertion, and the silent failure it catches, is
tabulated in [`../design/GRAPHICS.md`](../design/GRAPHICS.md), Section 16.

The test surface is 32 by 16 pixels of four bytes in rows of 40. The pitch
exceeds the width deliberately: a primitive that stepped from row to row by the
width rather than the pitch would still write inside the array, merely writing
the wrong pixels, so the eight pixels of padding on each row hold a sentinel that
no test ever writes and the padding is checked after each operation. A failure
therefore names the operation that caused it.

Because the surface is in memory, **all of this holds upon a machine with no
display at all**, which is the reason the primitives take a surface rather than
drawing upon the framebuffer by name.

### 16.1 The figure a person judges

The self-test also draws upon the framebuffer, and that part is judged by eye.
Capture it as in Section 15.1, **which from sub-task 6.4 means booting the
Oxys-OS (graphics figures) entry**; the default entry gives the screen to the
console instead. Four things are drawn, and each shows something different:

| What to look for | What its absence would mean |
| ---------------- | --------------------------- |
| A one-pixel frame around the **whole** screen, on all four edges | The extent or the pitch is wrong. A wrong pitch makes the vertical edges lean rather than run straight. |
| A panel with its two diagonals **crossing exactly at its centre** | The line is not exact; an error accumulated wrongly puts the crossing off-centre. |
| The second panel's fill and line **stopping dead at the clip boundary**, with the line's slope unchanged where it stops | Clipping is not confining the fill, or — the subtler fault — the line was clipped by moving its endpoints, which meets the boundary at a slightly different height. |
| The first panel **copied below itself, identically** | The blit is displaced, or takes the wrong part of the source. |

The frame is one pixel wide, so it is easily lost when a captured image is
scaled down; sample the corner pixels rather than trusting the eye at reduced
size.

### 16.2 The negative test

To confirm the self-test can fail, make the primitives address a row by the
surface's width instead of its pitch — the fault the padding sentinel exists to
catch:

```sh
sed -i 's/(uint64_t)(uint32_t)y \* surface->pitch/(uint64_t)(uint32_t)y * surface->width * surface->bytes_per_pixel/' \
    graphics/draw.c
make verify
```

The run must report `wrote into the row padding` from **six** independent
primitives — the fill, the clipped fill, the clear, the line, the blit and the
trimmed blit — and end `Graphics self-test FAILED.` Restore the file afterwards.

## 17. Verification of the font and the console

The font and the console of sub-task 6.4 are asserted at every boot by
`KernelVerifyConsole` — twenty-seven assertions in three groups, each tabulated
against the silent failure it catches in
[`../design/GRAPHICS.md`](../design/GRAPHICS.md), Section 21.

The face and its drawing are asserted **against a surface composed in memory**,
as the primitives of Section 16 are, so that the whole of that holds upon a
machine with no display. The four control characters are asserted upon the live
console, because the position they move is the console's own and there is no
second one to make; only characters that draw nothing are used — CR, HT and BS —
so the boot log the test is written into is not disturbed by the test of it.

The assertions worth naming here are the ones a compiler cannot make about a
table authored by hand: that **no two glyphs are identical**, that exactly one
glyph is blank, and that no glyph draws into the two columns reserved for the
spacing between characters.

### 17.1 The half a person judges

That the log is legible is not something the kernel can assert, for the reason
Section 15 gives: a framebuffer written correctly may be scanned out by nothing.
Boot the **default** menu entry — the console owns the screen there — and look at
it:

| What to look for | What its absence would mean |
| ---------------- | --------------------------- |
| The log begins at its **first line**, `Oxys-OS`, at the top of the screen | The replay buffer is not being replayed, and the screen begins part way through the boot. |
| **Every number is present** — addresses, counts, sizes | `KernelWriteHexadecimal` or `KernelWriteDecimal` is naming an output device itself rather than emitting through `KernelWriteString`. This reads as a formatting error in the messages and is a missing output path; see [`../design/GRAPHICS.md`](../design/GRAPHICS.md), Section 19.1. |
| Letters are upright and not mirrored, and words have gaps between them | The bit order is reversed, or a glyph draws into its spacing columns. |
| Text that has **scrolled** is unsmeared | The blit copied in the wrong direction and read bytes it had already overwritten. Visible only upon a display too short to hold the log, which is VirtualBox's 640 by 480 and not QEMU's 1280 by 800. |
| The echo loop's backspace stops at the prompt | The erase limit is not set, or did not move with a scroll. |

A screendump serves for the first four:

```sh
( sleep 11; echo "screendump /tmp/oxys-console.ppm"; sleep 3; echo "quit" ) \
  | qemu-system-x86_64 -machine q35 -cpu qemu64 -smp cores=2 -m 512M \
      -cdrom build/oxys.iso -display none -monitor stdio -serial null
```

### 17.2 The negative test

To confirm the self-test can fail, duplicate a glyph — precisely the
copy-and-paste the assertion exists for, and one that leaves a plausible-looking
table because the picture comment beside it is not touched:

```sh
# Give 'O' (0x4F) the bytes of '0' (0x30).
sed -i "/0x4F  'O'/{n;s/.*/    { 0x78, 0x84, 0x8C, 0x94, 0xA4, 0xC4, 0x78, 0x00 },/}" \
    graphics/font.c
make verify
```

The run must report

```
  two glyphs are identical, at codes 0x30 and 0x4F
Console self-test FAILED.
```

and `make verify` must itself fail, the harness having gained the assertion upon
`FAILED` recorded in Section 1. Restore the file afterwards — the correct bytes
for `'O'` are `0x78, 0x84, 0x84, 0x84, 0x84, 0x84, 0x78, 0x00`, and the picture
comment beneath the line states them.

## 18. Verification of the drawing optimisation

The primitives gained a path that writes a four-byte pixel as one 32-bit store,
and the console gained one that draws a character cell and its glyph in a single
pass. Both are asserted by `KernelVerifyGraphics` and `KernelVerifyConsole`, and
both are tabulated in [`../design/GRAPHICS.md`](../design/GRAPHICS.md),
Sections 25.1 and 25.2.

**A fast path is the most dangerous kind of code to leave unasserted.** It runs
only when its own precondition holds, so a fault in it is invisible upon every
surface that does not meet the condition — and the surface the self-tests use is
not the surface a person looks at. The test therefore draws upon **two surfaces
differing in nothing but their alignment** and requires them to produce identical
pixels, and it compares the two glyph routines for every glyph in the face.

### 18.1 Measuring it again

The figures in Section 23.1 of the design document were obtained with `RDTSC`,
not with the interval timer: interrupts are disabled for most of the boot and
only seventeen ticks elapse in the whole of it. To repeat the measurement, time
the operations from `KernelMain` after `PitInitialise` and read the counter
directly; the ratios are what matter, QEMU's interpreter making the absolute
figures proportional to instructions executed rather than to cycles.

### 18.2 The negative tests

Two, because the optimisation has two halves.

```sh
# The alignment conditions removed, so every four-byte surface claims the fast path.
sed -i 's/    surface->whole_words = (bytes_per_pixel == 4U) \&\&/    surface->whole_words = (bytes_per_pixel == 4U); \/\//' \
    graphics/draw.c
```

The run must report `a surface whose base and pitch are both odd was marked as
addressable by words, so every row would be written misaligned` and end
`Graphics self-test FAILED.`

The second is applied by hand: make `GraphicsPatternBlock` skip its clear bits,
as the transparent glyph does, by replacing the word store in its inner loop with
`if ((bits & bit) != 0U) { word[column] = ink; }`. Three assertions must fire in
the graphics self-test and a fourth in the console self-test, the last naming the
code point at which the two glyph routines first disagree. Restore the file
afterwards.

## 19. Verification of the fault screens

`KernelVerifyFaultScreen` asserts the table of fault screens at every boot — that
every severe fault has a screen of its own, that **no two of them share a title
or a colour**, and that every title will fit a 640-pixel display. Each assertion
and the silent failure it catches is tabulated in
[`../design/GRAPHICS.md`](../design/GRAPHICS.md), Section 25.3.

It asserts the table and not the drawing, for the reason Section 15 gives about
the display generally. **Nothing in it draws**, deliberately: drawing would set
the flag that records a screen as having been shown, and a real fault later in
the same boot would then find the display taken and draw nothing.

### 19.1 Looking at the screens

Two GRUB entries, which prove different things and must not be confused.

**Oxys-OS (fault screen demonstration)** composes a trap frame and draws one
vector's page, then halts. It proves the page — that its text fits, that its
panels lay out, that its colour and title are its own — and nothing about the
processor. The frame holds values no machine would produce, so a photograph of it
cannot be mistaken for a real report. Press `e` at the menu to change
`fault-screen=14` to another vector: 0, 6, 8, 10, 11, 12, 13, 14, 17, 18, or 256
for a panic the kernel raises itself.

**Oxys-OS (raise a genuine page fault)** writes to an unmapped address. It proves
the wiring — handler, report upon the serial port, screen upon the framebuffer,
end to end. A page fault is used because it is the one severe fault that can be
raised deliberately without endangering the machine.

Capture either as in Section 15.1, counting the keystrokes to the entry wanted:

```sh
( sleep 2;  echo "sendkey down"; sleep 0.25; echo "sendkey down"; sleep 0.25; \
  echo "sendkey ret"; \
  sleep 13; echo "screendump /tmp/oxys-fault.ppm"; \
  sleep 3;  echo "quit" ) \
  | qemu-system-x86_64 -machine q35 -cpu qemu64 -smp cores=2 -m 512M \
      -cdrom build/oxys.iso -display none -monitor stdio -serial null
```

What to look for:

| What to look for | What its absence would mean |
| ---------------- | --------------------------- |
| A coloured banner with the fault's title at several times life size, **not clipped at the top** | The banner is shorter than the title, or something scrolled the framebuffer after the screen was drawn. This is exactly the fault Section 24.3 of the design document records. |
| The mnemonic and vector beneath the title | The general screen was drawn, meaning the table has no entry for this vector. |
| Panels that differ **between faults** — an address for a page fault, a decoded selector for a general protection fault, instruction bytes for an invalid opcode | The evidence flags are not being consulted, and every fault is being given the same page. |
| The instruction bytes reproduced as real values, or an explicit statement that the address is unmapped | The bytes are being read without asking the paging hierarchy, which would raise a second fault. |
| Nothing written over the page afterwards | The console was not suspended. |
| At **640 by 480**: the title still fits, paragraphs re-wrap, and the footer is still on the screen | The layout was fitted to 1280 pixels. |

### 19.2 The negative tests

```sh
# Two screens given the same identity, as a copied row would be.
sed -i 's/{ 11U, "SEGMENT NOT PRESENT", "#NP, vector 11",/{ 11U, "INVALID TASK STATE SEGMENT", "#NP, vector 11",/' \
    graphics/faultscreen.c
make verify
```

The run must report `two fault screens share a title, at vectors 0xA and 0xB` and
end `Fault screen self-test FAILED.`

For the second, delete the `{ 18U, "MACHINE CHECK", ... }` row from the table.
The run must report `a severe fault has no screen of its own, at vector 0x12` —
the fault this catches being the one that would otherwise look like nothing at
all, the general screen still naming the vector. Restore the file afterwards.

## 20. Test record

| Date | Test | Result |
| ---- | ---- | ------ |
| 2026-09-04 | `make verify` — the drawing optimisation | Passed. The console had been measured at **15.2% of the whole boot**; it is now 4.5%, and the four operations that matter are 3.6 to 7.9 times faster. The assertions that guard it are the ones a fast path needs: a four-byte surface on a word boundary with a word-multiple pitch is marked word-addressable and one whose base and pitch are both odd is not, a three-byte pixel never is whatever its alignment, and — the assertion the rest rests upon — **two surfaces differing in nothing but their alignment are drawn upon and compared pixel for pixel**, so the fast path cannot quietly draw something different from the path the other tests exercise. The two glyph routines are compared for every one of the ninety-five glyphs, the console having changed which one it goes through. |
| 2026-09-04 | `make verify` — the drawing optimisation, **the negative tests** | Passed, both. With the alignment conditions removed from `whole_words`, the run reported `a surface whose base and pitch are both odd was marked as addressable by words`. With `GraphicsPatternBlock` made to skip its clear bits, three assertions fired in the graphics self-test and a fourth in the console self-test — `the two glyph routines disagree, at code 0x20` — which is the assertion that exists because no other test uses the path the console uses. Both edits were reverted. The procedure is Section 18.2. |
| 2026-09-04 | `make verify` — the fault screen table | Passed. Every severe vector has a screen of its own; no two share a title, a colour or a vector; every title fits a 640-pixel display at the scale used there; the evidence flags name only panels that exist; and no screen had been drawn when the test ran, which is itself an assertion — a screen drawn early would leave a real fault later in the boot with nothing to display. |
| 2026-09-04 | QEMU screendump — the fault screens, **a person's judgement** | Passed at 1280 by 800 for the page fault, general protection fault, double fault, divide error and kernel panic. Each carries its own banner colour and title, and — the point of the exercise — **its own evidence**: the page fault decodes its address and states that no translation existed; the general protection fault decodes error code `0x43` into selector index `0x0008` in the interrupt descriptor table, raised externally; the double fault shows the stack and says in as many words that the fault to pursue is the one before it; the divide error shows its operands. The instruction panels reproduced real bytes read from the instruction pointer. |
| 2026-09-04 | QEMU — the fault screens, **the wiring, end to end** | Passed. A write to an unmapped address raised a genuine page fault; the handler reported it in full upon the serial port, the screen was drawn upon the framebuffer, and the run ended `KERNEL PANIC: An unresolved page fault was raised.` The screen showed the real faulting address `0xFFFF900000000000`, `No translation existed for this address.` and `The access was a write.` — all three read from the actual fault and not composed. |
| 2026-09-04 | QEMU screendump — **a fault found by looking**, not by asserting | The first correct-looking screen was displayed wrongly: its banner was clipped and its whole layout shifted up by three character rows. The cause was not in the drawing. `KernelPanic` follows every fatal exception and writes to the diagnostic path, which includes the console, which is upon the same framebuffer — and its cursor stood at the foot of a screen of boot log, so each newline of the panic message scrolled the framebuffer up by eight pixels. `ConsoleSuspend` was added and the fault screen now takes the display. Recorded because no assertion available would have found it: every pixel was drawn where it was asked for. |
| 2026-09-04 | VirtualBox 7, headless, 512 MiB, legacy BIOS — the fault screens at 640 by 480 | Passed. The title fits at twice life size where it is drawn at three times upon QEMU, the paragraphs re-wrap to the narrower line, and the footer — which wraps to two lines at this width — is still upon the screen. This is the assertion `KernelVerifyFaultScreen` makes about title length made good in practice, and it is the machine the person judging these screens would not have used. |
| 2026-09-04 | `make verify` — sub-task 6.4, the font and the console | Passed; twenty-seven assertions. The face is asserted against the metrics it was drawn to — no glyph draws into the two columns reserved for spacing, exactly one glyph (the space) is blank, the replacement glyph is not blank, and **no two of the ninety-five glyphs are identical**, which is the assertion worth having in a table authored by hand. A glyph drawn upon a 16 by 16 surface at (4, 4) matches its own bytes pixel for pixel and leaves the margin around its cell untouched, which is what catches a reversed bit order — mirroring being invisible in the symmetric letters. A pixel the glyph does not set keeps the background it was given, so text can be drawn over an image. The four control characters were asserted upon the live console: CR returns to column 0 without changing the row; HT lands on column 8 from column 0 and on 16 from 8, so it advances to a multiple and not by eight; BS moves exactly one position, and does not move at the erase limit, nor cross to the row above when the limit stands at column 0. |
| 2026-09-04 | QEMU screendump — sub-task 6.4, **the half a person judges** | Passed at 1280 by 800, 160 by 100 characters. The log is rendered from its first line, `Oxys-OS`, at the top of the screen — the replay buffer having carried 1903 bytes written before the framebuffer could be mapped, with nothing dropped against its 4 KiB capacity. Every number is present, which is the assertion this capture exists for: `KernelWriteHexadecimal` and `KernelWriteDecimal` named the display and the serial port themselves until this sub-task, so the console was shown every word of the log and not one of its addresses, counts or sizes. Letters are upright, words are separated, and the echo loop's backspace stops at the prompt. |
| 2026-09-04 | `make verify` — sub-task 6.4, **the negative test** | Passed; glyph `0x4F` (`'O'`) was given the bytes of glyph `0x30` (`'0'`), the picture comment beside it left alone so that the table still looked correct. The run reported `two glyphs are identical, at codes 0x30 and 0x4F`, ended `Console self-test FAILED.`, and **`make verify` itself failed**, the harness's second assertion — the one upon the word `FAILED`, described in Section 1 — naming the offending line. The edit was reverted and the run repeated, reporting `Console self-test passed.` The procedure is Section 17.2. |
| 2026-09-04 | VirtualBox 7, headless, 512 MiB, legacy BIOS — sub-task 6.4 | Passed, and it is the result this sub-task was for. VirtualBox had **no readable diagnostic output at all**: no serial adapter this kernel detects (Section 9.1), and since sub-task 6.2 no text mode either. It now draws the boot log upon its 640 by 480 framebuffer as 80 by 60 characters, reporting `Console: 80 by 60 characters of 8 by 8 pixels` and `1847 bytes replayed from before the console existed`, with nothing dropped. Eighty by sixty does not hold the log, so this machine **scrolls where QEMU does not**, and the screen at the end of the boot was legible and unsmeared after dozens of scrolls — which is the copy direction of the blit, chosen in sub-task 6.3 for exactly this case, being exercised for the first time by the thing it was written for. The screen was decoded pixel by pixel rather than eyeballed: rows fall exactly eight pixels apart, with descenders occupying row 7 as the metrics say. |
| 2026-09-04 | QEMU screendump — sub-tasks 6.2 and 6.3, the figures, **through the new menu entry** | Passed. From sub-task 6.4 the console erases the figures within the same boot, so they are painted only when the command line carries `graphics-figure`. Selecting the second menu entry by `sendkey down` and `sendkey ret` through the QEMU monitor produced a capture holding the bands `(200,30,30)`, `(30,200,30)`, `(30,30,200)`, a white pixel at (1279, 799), white at all four corners of the frame, and **no console text whatsoever** — so the option reaches the kernel, the figures are drawn, and the console is suppressed. The procedure is Section 15.1. |
| 2026-09-03 | `make verify` — sub-task 6.3, the drawing primitives | Passed; sixty-five assertions against a surface composed in memory, so the whole of it holds upon a machine with no display. The rectangle arithmetic treats touching edges as disjoint and an empty intersection as non-negative; a clip of `{-1000, -1000, 100000, 100000}` is confined to the surface, so no argument can widen it; a fill straddling a corner leaves exactly the 5 by 5 that remains; an outline of 6 by 4 is exactly 16 pixels, so no corner is written twice; a line drawn backwards lights the same pixels as one drawn forwards; a blit trimmed by the clip takes the right half of a source whose halves differ, so it is cropped and not shifted; and rows moved up and down within one surface move rather than smear. After every operation the row padding still holds its sentinel. |
| 2026-09-03 | `make verify` — sub-task 6.3, **the assertion that clipping does not move a line** | Passed. The unclipped line is drawn and the pixels it lights inside a region recorded; the surface is then cleared entirely and the same line drawn with the clip set to that region. The two sets coincide pixel for pixel. An implementation that clipped by moving the endpoints would pass a count and fail this, the error accumulating from a different start and lighting a neighbouring pixel here and there — which is invisible until two clipped regions meet along a seam and the line through them has a kink. |
| 2026-09-03 | QEMU screendump — sub-task 6.3, the figure a person judges | Passed at 1280 by 800. The frame reaches all four edges, sampled white at every corner and at the midpoint of each side; the panel's diagonals cross at its centre; the second panel's fill and line stop dead at the clip boundary with the line's slope unchanged; and the blitted copy is identical to its original and in the position asked for. The frame is one pixel wide and is lost to a scaled-down view, so the corners were sampled rather than eyeballed. |
| 2026-09-03 | `make verify` — sub-task 6.3, **the negative test** | Passed; with rows addressed by the surface's width instead of its pitch, `wrote into the row padding` was reported by six independent primitives — the fill, the clipped fill, the clear, the line, the blit and the trimmed blit — and the run ended `Graphics self-test FAILED.` That six operations report it separately is the point of checking the padding after each rather than once at the end: the failure names its cause. The edit was reverted. The procedure is Section 16.2. |
| 2026-09-03 | `make verify` — sub-task 6.2, the framebuffer self-test | Passed. GRUB supplied an RGB framebuffer of 1280 by 800 at 32 bits, pitch 5120, at physical `0xFD000000`; it was mapped at `0xFFFFC00000004000`, 4000 KiB, write-combining. Both ends of the mapping translate to the reported physical range, so it is contiguous and not a single frame repeated; the page-table entry sets PAT and clears PCD and PWT; entry 4 of `IA32_PAT` holds `0x01` and entries 0 to 3 are still `0x06`, `0x04`, `0x07`, `0x00`, so no existing mapping had its memory type changed beneath it; black encodes as zero and white as non-zero; and a pixel written to the last position of the last row read back. |
| 2026-09-03 | QEMU screendump — sub-task 6.2, **the half a kernel cannot assert** | Passed; the captured image is 1280 by 800. The bands read red `(200,30,30)`, green `(30,200,30)`, blue `(30,30,200)` left to right, so the channel positions were read correctly and not assumed; they are flat across every row of the band, so the pitch is right; they reach column 1279; and the pixel at (1279, 799) is `(255,255,255)`, so the mapping covers its whole declared extent. |
| 2026-09-03 | `make verify` — sub-task 6.2, **the negative test** | Passed; with the page attribute table given write-back instead of write-combining, the run reported `entry 4 of IA32_PAT does not hold write-combining` and ended `Framebuffer self-test FAILED.` The edit was reverted and the run repeated, reporting `Framebuffer self-test passed.` The procedure is Section 15.2. |
| 2026-09-03 | VirtualBox 7, headless, 512 MiB, legacy BIOS — sub-task 6.2 | Passed, upon a hypervisor whose boot loader chose a different mode entirely: **640 by 480**, against QEMU's 1280 by 800. The bands appear in the right order, flat, and reach the right-hand edge, so the tag was parsed, the mapping reaches the adapter, the channel positions were read rather than assumed, and the pitch is right — upon a display this kernel had never seen. That the mode differs is the result, not an inconvenience: it is the evidence that nothing was hard-coded to what QEMU happens to supply. |
| 2026-09-03 | VirtualBox — the diagnostic path, after sub-task 6.2 | **Nothing readable remains.** The serial adapter is not detected under VirtualBox (Section 9.1, established in sub-task 6.1) and the adapter is now in a graphics mode with no console upon it, so no line of the boot log can be read there at all until sub-task 6.4. The pattern is the whole of what VirtualBox can now show, and the pause-and-photograph procedure of Section 9.2 has nothing to catch. This is recorded rather than remedied: QEMU carries the assertions, and the console returns two sub-tasks from now. |
| 2026-09-03 | GRUB `gfxpayload` — **a claim disproved rather than a test passed** | GRUB 2.12 ignores `gfxpayload` for a multiboot2 image. Asking for `1024x768x32` yielded 1280 by 800 at 32 bits; asking for `text` yielded a graphics mode likewise; and `insmod all_video` changed the outcome to 800 by 600 at 24 bits by changing which driver GRUB chose from, not by honouring anything. Established with a purpose-built ISO whose default entry was the one under test, so that the result could not be a mis-selected menu entry. The directives were removed rather than left to look as though they worked; `boot/grub/grub.cfg` records the position. |
| 2026-09-03 | `make verify` — the display self-test, after sub-task 6.2 | Skipped, as intended, the adapter being in a graphics mode. It is reported as skipped rather than passed, and the test was moved to run after the Multiboot2 parse, that being the only place the mode is known. See Section 15.3. |
| 2026-09-03 | VirtualBox 7, headless, 512 MiB, two processors, legacy BIOS — sub-task 6.1 | Passed; the machine reached `Phase 6 initialisation complete` and the echo loop. The privilege report read identically to QEMU's upon a different hypervisor's descriptor tables and a different memory map: task state segment at `0xFFFFFFFF80186960` with limit 103 and task register `0x30`, `RSP0` `0xFFFFFFFF80186960`, `IST1` `0xFFFFFFFF80182960`, I/O map base 104 beyond the limit, `IA32_STAR` `0x18000800000000` deriving `CS 0x8`/`SS 0x10` and `CS 0x2B`/`SS 0x23`, and `IA32_FMASK` `0x47700`. `Privilege self-test passed.` was read from the console by the pause procedure of Section 9.2. That the machine reached the banner at all is itself evidence, `LTR`, `int $200` upon an interrupt stack and two executions of `SYSCALL` all occurring before it and each failing as a fault rather than as a message. |
| 2026-09-03 | VirtualBox — the serial channel | Not available; the kernel reports `Serial adapter: absent; no diagnostic channel.` and transmits nothing, so `make verify`'s assertion cannot be made under this hypervisor. Pre-existing and unrelated to sub-task 6.1; recorded in Section 9.1. |
| 2026-09-03 | `make verify` — sub-task 6.1, the privilege self-test | Passed; forty-eight assertions. The table's limit covers the eight slots and `GDTR` names this table; each user descriptor is decoded field by field and says what it must — present, DPL 3, and 64-bit code, compatibility-mode code or writable data respectively; and the three descriptors stand at the displacements `SYSCALL` and `SYSRET` derive their selectors by, which is asserted as an ordering because every descriptor may be individually perfect and the transition still fail. The task state segment descriptor's base and limit name the segment exactly, and its type is **11 and not 9**, which only the processor writes and is therefore the sole evidence that `LTR` was accepted; the task register holds `0x30`; `RSP0` is non-zero and sixteen-byte aligned; the double fault's stack is non-zero and distinct from it; and the I/O map base lies beyond the limit, so no port is permitted to user mode. `IA32_EFER.SCE` is set and `IA32_LSTAR` holds the entry point, both read back from the processor; the four selectors the processor will derive are computed by its own arithmetic and are `0x08`, `0x10`, `0x2B` and `0x23`; and `IA32_FMASK` clears `IF`, `DF`, `TF`, `NT` and `AC`. |
| 2026-09-03 | `make verify` — sub-task 6.1, the interrupt stack table exercised | Passed; vector 200 raised without an interrupt stack table entry built its trap frame outside the double-fault stack, and raised with the double fault's entry built it within, the two addresses differing. This is the assertion that the processor *reads* the task state segment, as against the assertions that this kernel wrote one: a segment whose descriptor the processor had rejected, or a task register never loaded, would satisfy every inspection and switch no stack. The gate for vector 14 is confirmed to hold no entry, an interrupt stack table entry being a fixed address that does not nest and the page-fault handler being one that may itself fault; an entry above the seven the architecture provides is refused and leaves the gate unaltered; and the probe vector is left as it was found. |
| 2026-09-03 | `make verify` — sub-task 6.1, the transition exercised | Passed; `SYSCALL` executed from privilege level 0 reached the entry point `IA32_LSTAR` names, and the entry point observed `CS` `0x08` and `SS` `0x10` — the selectors the processor loaded, which exist nowhere else, the instruction loading them and the return replacing them. Executed a second time with the interrupt flag deliberately set, the entry point observed it **clear**, which is `IA32_FMASK` working and is not observable at all in the first pass; and the flag was set again upon return, so the flags saved in `R11` were restored. |
| 2026-09-03 | `make verify` — sub-task 6.1, **the negative test** | Passed; with `RFLAGS_INTERRUPT_ENABLE` removed from `SYSCALL_FLAG_MASK`, exactly the two assertions that bear upon it reported — the one upon the register and the one upon what the processor did — and the run ended `Privilege self-test FAILED.` The edit was reverted and the run repeated, reporting `Privilege self-test passed.` The procedure is Section 13.1. |
| 2026-09-03 | `make all` — build with the full diagnostic regime, after sub-task 6.1 | Passed; no diagnostics, including from the `_Static_assert` upon the size of `TaskStateSegment`, which fails the compilation if the packed attribute is ever lost. |
| 2026-09-02 | `make verify` — sub-task 5.8, the filesystem self-test | Passed; some ninety assertions upon two composed volumes. A path resolves through components, repeated and trailing separators, `.`, `..` and both forms of symbolic link, and is refused for the reason that distinguishes each refusal; a descriptor reads a file whose contents depend upon their offsets, its position advancing by exactly what was transferred, and the end of the file is reported by the count; two descriptors upon one file have two positions and one identity; a directory is listed and what it lists is what resolves; a file is created, written, read back identically, appended to, truncated in both directions, given a second name and destroyed, and is refused destruction while something holds it; a new directory bears two links and its parent gains one, which is returned when it is removed. |
| 2026-09-02 | `make verify` — sub-task 5.8, the mount | Passed; a second volume, identical to the first but for the owner of one file, is mounted upon a directory of it. The mount point becomes the second volume's root; a path crossing it reaches the second volume and one that does not reaches the first; what the mount covers is entirely unreachable; `..` from the mounted root leaves the volume and arrives at the parent of the mount point, and a path that returns and crosses again reaches the second volume once more; a read-only mount refuses a write and a creation; neither mount may be withdrawn while anything is held; and the covered directory reappears exactly as it was. No node was left held and no descriptor open. |
| 2026-09-02 | `make verify` — sub-task 5.8, the mark a mount leaves | Passed; the state read back **out of the medium** after a writable mount has the clean bit clear and the error bit clear, with the mount count raised; after a clean withdrawal the clean bit is set again; and `Ext2VerifyGroupDescriptors` passes upon a superblock read afresh afterwards. |
| 2026-09-02 | QEMU i440fx with an image from `mke2fs -d` — the root mount, read-only | Passed; the volume mounted at `/` as `ata0`, block size 1024, read-only, and its root listed as inodes 2, 2, 11, 12, 13, 14 with the types and names `debugfs -R "ls -l /"` gives for the same image. Afterwards `dumpe2fs -h` reported `clean` with a mount count of 0 — the volume untouched — and `e2fsck -fn` reported no error. |
| 2026-09-02 | QEMU i440fx with `ext2-write-test` — the root mount, writable, never withdrawn | Passed; `dumpe2fs -h` afterwards reported `not clean` — and not "with errors" — with `Mount count: 1`, which is the mark persisting because the machine stopped while the volume was open. `e2fsck -fn` reported no structural error through all five passes. |
| 2026-09-02 | QEMU i440fx with `ext2-write-test` — the write probe and the clean withdrawal | Passed; 5000 bytes were written to `/oxys-write-test` through a descriptor and read back identically through the same descriptor after a seek to the beginning, the volume was withdrawn and mounted afresh read-only, and `dumpe2fs -h` then reported `clean` with a mount count of 1. `debugfs` states the file is 5000 bytes with a block count of 10, and the extracted contents match `((offset * 31) + 7) & 0xFF` byte for byte at both ends. `e2fsck -fn` reported no error. |
| 2026-09-02 | QEMU i440fx with a 4096-byte-block image — the same, at the other block size | Passed; mounted as block 4096, the root listed identically, the 5000-byte probe crossed a block boundary at that size also, and `e2fsck -fn` reported no error. |
| 2026-09-02 | `e2fsck -fn` over a volume the layer had created and destroyed files upon — **a defect found** | Initially failed; `e2fsck` reported inodes 16 and 17 as "part of the orphaned inode list", the deletion time recorded for want of a clock being the constant 1 and `i_dtime` being overloaded as the orphan-list link. Corrected to `EXT2_DELETION_TIME_UNKNOWN`; re-run, all five passes clean. See `docs/storage/VFS.md`, Section 11.1. |
| 2026-08-30 | `make all` — build with `-Wall -Wextra -Werror` | Passed; no diagnostics. |
| 2026-08-30 | `grub-file --is-x86-multiboot2` | Passed; the image is Multiboot2 compliant. |
| 2026-08-30 | `make iso` — ISO generation | Passed. |
| 2026-08-30 | `make verify` — QEMU boot and serial assertion | Passed. |
| 2026-08-30 | QEMU screendump — VGA text-mode rendering | Passed; the banner reads `Oxys-OS`. |
| 2026-08-31 | `make verify` — sub-task 3.5, the interrupt controller self-test | Passed; the controllers are remapped, the mask is honoured, a spurious request is recognised, and the interrupt flag may be set without a double fault. |
| 2026-08-31 | `make verify` — sub-task 3.6, the interval timer self-test | Passed; divisor 1193 realising 1000.152 Hz, ticks counted only with interrupts enabled and only while the line is unmasked. |
| 2026-08-31 | `make verify` — sub-task 3.7, the keyboard self-test | Passed; the controller and port self-tests pass, and the decoder, the modifier discipline and the buffer overrun behave as `docs/devices/KEYBOARD.md`, Section 7.1, requires. |
| 2026-08-31 | QEMU `sendkey` — the keyboard interrupt path, end to end | Passed; the keystrokes `h e l l o spc o x y s` were echoed upon the serial port as `hello oxys`. |
| 2026-08-31 | `make verify` — the display self-test | Passed; the backspace, the tabulation, the carriage return and the line feed move the cursor as ANSI X3.4-1986 defines them, and a backspace in the first column does not move it. |
| 2026-08-31 | `make verify` — sub-task 4.1, the serial self-test | Passed; the request line is claimed and unmasked, a sequence returns unaltered through local loopback, an impossible line configuration is refused, 9600 baud yields a divisor of twelve, a written string raises the adapter's interrupt, and the transmitter interrupt is withdrawn once idle. |
| 2026-08-31 | QEMU `-serial stdio` — the serial receive path, end to end | Passed; `serial-in-works`, typed upon the host, was received by interrupt and echoed back. |
| 2026-08-31 | QEMU `sendkey` — the backspace, end to end | Passed; `o x y s spc b a d` followed by three backspaces and `g o o d` produced `oxys bad` then the erasing sequence three times then `good` upon the serial port, a terminal rendering it `oxys good`. |
| — | VirtualBox boot | Passed; performed by the project owner upon the Windows host, `VBoxManage` being unavailable in this environment. |
| — | Physical hardware boot | Not performed. |
| 2026-09-01 | `make verify` — sub-task 4.2, the display self-test | Passed; the adapter reports its colour configuration at `0x03D4`, blinking is disabled, every control character moves the cursor as ANSI X3.4-1986 defines it, the backspace crosses into the row above and stops at the erase limit, the CRT controller holds the position the driver believes it holds, an impossible cursor position and shape are refused, and a scroll moves the display by exactly one row. |
| 2026-09-01 | QEMU `-serial stdio` — the backspace across a row boundary | Passed; `ab`, a line feed, `cd` and four backspaces erased both rows' characters in turn, the third backspace crossing into the row above and emitting the ECMA-48 correction to the terminal. |
| 2026-09-01 | QEMU `sendkey` — the backspace across a row boundary | Passed; the same sequence delivered as scan codes produced an identical echo, so the keyboard path and the serial path share the behaviour. |
| 2026-09-01 | QEMU `-serial stdio` and `sendkey` — the backspace consumes the separator alone | Passed; after `ab`, a line feed and `cd`, the third backspace left the cursor after `ab` with both characters standing, and the fourth and fifth erased them in turn. |
| 2026-09-01 | `make verify` — sub-task 4.3, the bus self-test | Passed; mechanism one answers its own probe, an absent function reads as all ones, the narrow accessors agree with the wide one, a host bridge stands at `0:0.0`, and every base address has had its type bits removed. Six functions were enumerated upon one bus, each corresponding to a device QEMU is known to emulate upon the q35 board. |
| 2026-09-01 | `make verify` — sub-task 4.4, the disk self-test upon q35 | Passed; no ATA device answers upon that board, the self-test reports as much and asserts nothing, and the kernel proceeds. |
| 2026-09-01 | QEMU i440fx with a 256 GiB sparse image — the disk self-test | Passed; a disk of 536870912 sectors with 48-bit addressing was identified upon the primary master and the ISO's optical drive recognised as a packet device by its signature. The relationship between sectors was asserted: a two-sector read begins where a one-sector read did and continues with the sector that follows; a range beyond the capacity is refused; a sector beyond the 28-bit limit reads. |
| 2026-09-01 | QEMU i440fx — the seeded content, confirmed from outside | Passed; sectors 0, 1 and 0x10000001 of the image were seeded upon the host and each was read back by the driver with its content intact, which is the one property the self-test cannot establish for itself. |
| 2026-09-01 | QEMU i440fx with `disk-write-test` — the write path | Passed; a pattern written to the final sector read back byte for byte, the sector was restored to its previous contents, and the restoration was confirmed both by the kernel and by inspection of the image upon the host. |
| 2026-09-01 | `make verify` — sub-task 4.5, the block self-test | Passed; a device of memory is registered and withdrawn, a duplicate name and a device whose nature and operations disagree are refused, a range that would wrap a 64-bit block number is refused, a two-block transfer carries both blocks in order, and the accounting matches the blocks that moved. |
| 2026-09-01 | QEMU i440fx — the ATA disk presented through the block layer | Passed; the disk of the primary master registered as `ata0`, 536870912 blocks of 512 bytes, writable. |
| 2026-09-01 | `make verify` — sub-task 4.6, the buffer self-test | Passed; a block held is not read again, a modified block does not reach the device until it is written back, a dirty block evicted under pressure is written back as it goes, a buffer held by a caller survives 64 subsequent misses, a request is refused rather than served when every buffer is held, and invalidation writes back and discards. |
| 2026-09-01 | `make verify` — sub-task 5.1, the volume self-test | Passed; every field of a composed superblock is read from the offset the format defines, the geometry derived from it is correct, a revision 0 volume receives its fixed values, and each of the twelve refusals refuses — including a volume whose block count and inode count imply different group counts. |
| 2026-09-01 | QEMU q35 and QEMU i440fx with a disk, after the `LOAD` segments were separated by permission | Passed; every self-test reported as before and the kernel reached its completion banner upon both boards. `readelf -lW build/oxys.elf` shows five `LOAD` segments — `R E`, `RW`, `R E`, `R`, `RW` — each of alignment `0x1000` with its file offset congruent to its address, and the linker emits no warning. |
| 2026-09-01 | QEMU i440fx with an image from `mke2fs -d` — a root directory of 900 entries in 40 blocks | Passed; the root inode reported mode `0x41ED`, 40960 bytes, 3 links and 82 sectors, and blocks 580, 616, 640, 664, 688, 712, 736, 760, 784, 808, 832, 856, 881. `debugfs -R "stat <2>"` states the same inode and the same blocks, index 12 having been reached through the indirect block at 880. |
| 2026-09-01 | QEMU i440fx with an image from `mke2fs -d` — a root directory of 9000 entries in 500 blocks | Passed; the root reported 512000 bytes and 1006 sectors, the prefix 772, 786-796, 798, and `[268]=1056`. `debugfs` states `(12-267):798-1053, (DIND):1054, (IND):1055, (268-499):1056-1287`, so index 268 was resolved two levels down and matches. |
| 2026-09-01 | QEMU i440fx with an image from `mke2fs` — 1024-byte blocks, sub-task 5.2 | Passed; group 0 reported block bitmap at 66, inode bitmap at 67, inode table at 68, 7599 free blocks, 2037 free inodes and 2 directories, each matching `dumpe2fs`. The whole-table check passed silently: 7599 and 7612 free blocks sum to the 15211 the superblock states, and 2037 and 2048 free inodes to 4085. |
| 2026-09-01 | QEMU i440fx with an image from `mke2fs` — 4096-byte blocks, sub-task 5.2 | Passed; one group with bitmaps at blocks 6 and 7, inode table at block 8, 18736 free blocks and 19989 free inodes, matching `dumpe2fs`. |
| 2026-09-01 | QEMU i440fx with an image from `mke2fs` — 1024-byte blocks | Passed; the kernel reported 16384 blocks of 1024 bytes with 15211 free, 4096 inodes of 256 bytes with 4085 free, 2 groups of 8192 blocks and 2048 inodes, first data block 1, first usable inode 11, and features `0x38`/`0x2`/`0x3`. Every figure matches `dumpe2fs -h` upon the same image. |
| 2026-09-01 | QEMU i440fx with an image from `mke2fs` — 4096-byte blocks | Passed; 20000 blocks of 4096 bytes, one group, first data block 0 as the format requires of any block size but 1024. |
| 2026-09-01 | QEMU i440fx with a disk holding no filesystem | Passed; refused with *the volume bears no EXT2 magic number*, and the kernel proceeded. |
| 2026-09-02 | `make verify` — sub-task 5.4, the directory self-test | Passed; the root of a composed volume yields `.`, `..`, `file` and `sub` with the inode number, file type, block and offset each stands at, the unused record between them is passed over and the final record ends the traversal; a name is matched by its whole length and not by a prefix; twelve paths resolve to the inodes they name and eight that name nothing are refused; every malformed record of `docs/storage/EXT2.md`, Section 10.6, is refused; and the same two bytes at offset 6 are refused under the sixteen-bit reading and accepted under the eight-bit one according to the feature flag alone. |
| 2026-09-02 | QEMU i440fx with an image from `mke2fs -d` — a root directory of 46 entries in two blocks | Passed; the kernel listed the entries with the inode numbers, file types and offsets `debugfs -R "ls -l /"` gives — 11 for `lost+found`, 12 for `README`, 13 for the first long name — and counted 46, which requires every record length in both blocks to be read correctly and the traversal to cross from block 292 to block 331 at exactly the right point. `/lost+found` resolved to inode 11, a directory of 12288 bytes, matching `debugfs -R "stat <11>"`. |
| 2026-09-02 | QEMU i440fx, the probe path set to `//sub/deeper/../deeper/buried` for one boot | Passed; resolved to inode 56, a regular file of 2 bytes, which `debugfs -R "stat /sub/deeper/buried"` states identically. The path carries a doubled leading separator and a `..` that returns to the directory it came from, so five lookups were performed to reach a file three components deep. |
| 2026-09-02 | QEMU i440fx with an image from `mke2fs -d` — a root directory of 9000 entries in 530 blocks | Passed; the kernel counted 9003 entries, which `debugfs -R "ls -l /"` confirms — the 9000 files, `.`, `..` and `lost+found`. The traversal crossed both the direct-to-indirect boundary at index 12 and the indirect-to-doubly-indirect boundary at index 268 without losing or repeating a record. |
| 2026-09-02 | `make verify` — the allocator self-test, after the integer-wrap corrections | Passed; a page count one beyond the arena's capacity, one of 2^38 that wraps the addition and one of 2^52 that wraps the multiplication are each refused; `SIZE_MAX`, `SIZE_MAX - sizeof(void *)` and `SIZE_MAX - PAGE_SIZE` are each refused by the heap; the pages in use are unchanged across all six refusals; and an ordinary single-page allocation made afterwards returns an address within the arena, which is the assertion that distinguishes the corrected allocator from the previous one, a request of 2^52 pages having returned NULL under both. |
| 2026-09-02 | `make verify` — the allocator self-test, after the full-range check upon `KernelPagesFree` | Passed; a legitimate four-page range is allocated, written, read back, released, reissued from the free list at the same address and released again, and the arena's pages in use return to exactly their previous figure. The refusal itself is not asserted and cannot be: every impossible argument to `KernelPagesFree` panics, and no means of surviving a panic exists before the test harness of Phase 7. This test covers the admit direction, so that a bound inverted or off by one halts the boot rather than passing silently. |
| 2026-09-02 | `make verify` — sub-task 5.5, the file self-test | Passed; a 1500-byte composed file reads exactly across the boundary between its two blocks, a run beginning at offset 1000 crosses that boundary and returns the right bytes on both sides, a read crossing the end is shortened to it, a read at or beyond the end returns zero bytes and succeeds, block 12 of the sparse file reads as data while block 13 beside it reads as zeroes, a directory is refused, both forms of symbolic link are recognised and read, a target longer than the buffer is refused rather than truncated, five paths resolve through links including one relative target and one within a path, `Ext2ResolvePathNoFollow` returns the link while still following links within the path, a trailing separator overrides it, and a link altered to name itself is refused by the depth bound. The composed file holds a byte derived from its own offset, so a read returning the right number of bytes from the wrong block fails. |
| 2026-09-02 | QEMU i440fx with an image from `mke2fs -d` — reading a regular file | Passed; `/content.txt` resolved to inode 12 of 22 bytes and its first sixteen bytes read `0x4F 0x78 0x79 0x73 0x2D 0x4F 0x53 0x20 0x72 0x65 0x61 0x64 0x73 0x20 0x61 0x20`, which `xxd` upon the host gives as `4f7879732d4f53207265616473206120` for the same prefix. `debugfs` states inode 12 and 22 bytes. |
| 2026-09-02 | QEMU i440fx — both forms of symbolic link upon a real volume | Passed; `/shortlink` reported inode 17, 4 bytes, a target held within its inode reading `deep`; `/longlink` reported inode 16, 71 bytes, a target held in a block reading `/deep/../deep/../deep/../deep/../deep/../deep/../deep/deeper/buried.txt`. `debugfs -R "stat"` states `Blockcount: 0` for the first and `2` for the second, which is the distinction the kernel draws, as the volume itself records it. |
| 2026-09-02 | QEMU i440fx, the probe path set to `/shortlink/deeper/buried.txt` for one boot | Passed; resolved to inode 15, a regular file of 7 bytes, reading `0x62 0x75 0x72 0x69 0x65 0x64 0xA`. `debugfs -R "stat /deep/deeper/buried.txt"` states inode 15 and 7 bytes, and the contents are `buried`. The link was followed mid-path and its relative target resolved against the root that holds it. |
| 2026-09-02 | `make verify` — sub-task 5.6, the write self-test | Passed; both bitmaps report the volume as composed, a block allocated is in use and the counts fall by one upon the volume as well as in memory, freeing something already free is refused for a block and for an inode, the one free inode is allocated and a second allocation refused, a reserved inode may not be freed, a write reaches the volume without touching the bytes on either side, a file truncated to nothing returns exactly the two blocks it held and takes exactly them back when rewritten, a write beyond the end leaves a hole that reads as zeroes, truncation upward allocates nothing, a write into an unoccupied entry of the doubly indirect block allocates two blocks and not one, `Ext2VerifyGroupDescriptors` still passes afterwards, and a read-only volume refuses every allocation, free and write. |
| 2026-09-02 | QEMU i440fx from the `EXT2 write self-test` GRUB entry — writing a real volume | Passed; the kernel emptied `/oxys-write-test` and wrote 8192 bytes into it, reporting inode 13, 16 sectors, and 7877 free blocks and 2035 free inodes remaining. |
| 2026-09-02 | `e2fsck -fn` upon the volume the kernel had written | Passed with no errors through all five passes, including Pass 5, which checks the group summary information the kernel maintained and wrote back. The block count rose from 308 to 315, the file having held one block of twelve bytes and now holding eight of 1024. This is an independent judgement of the whole allocation path by the tool whose business it is. |
| 2026-09-02 | `debugfs dump` upon the same volume — the contents written | Passed; all 8192 bytes match the expected pattern, each byte derived from its own offset, over the whole length. The other file in the image reads exactly as it did before, so nothing was written that was not asked for. |
| 2026-09-02 | `make verify` — sub-task 5.7, the name self-test | Passed; an insertion yields exactly one entry more when the whole directory is traversed and the name resolves as a path, a duplicate name is refused, a removal returns the directory to exactly what it held, `.` and `..` may not be removed, sixty-four insertions and removals of one name consume no blocks, a created file has one link and may be written and reached, a second name raises the link count and removing one of two names removes the name and not the file, removing the last name frees the inode and its blocks and the inode is then free in the bitmap and refused as deleted, a created directory has two links and its parent gains one with `/made/.`, `/made/..` and `/made/../made` all resolving, a directory holding a file is not empty and is not removed, an emptied directory is removed and the parent's link count returns, the root is not removed, the free counts and the root's entries return to what they were, `Ext2VerifyGroupDescriptors` passes, and a read-only volume refuses every one of these. |
| 2026-09-02 | QEMU i440fx from the `EXT2 write self-test` GRUB entry — creating and removing names upon a real volume | Passed; within one boot the kernel created `/oxys-made` (inode 14), created `within` (inode 15) inside it, wrote to that file, and removed both, then rewrote `/oxys-write-test` with 8192 bytes. |
| 2026-09-02 | `e2fsck -fn` upon the volume after creation and removal | Passed with no errors through all five passes. Pass 2 checks the directory structure the kernel split and joined, Pass 3 the connectivity of the `.` and `..` it wrote, and Pass 4 the reference counts it raised and lowered — including the parent's, which is the one a kernel cannot see for itself. The volume reported `13/2048 files`, exactly as before the test, so both inodes created were returned; `debugfs -R "ls -l /"` listed the same five entries as before; and the 8192 bytes still matched byte for byte. |
