# Oxys-OS Test Procedure

**Corresponding phase**: All phases. `PROJECT_GUIDELINES.md`, Section 2, requires every
milestone to be bootable and testable under QEMU and VirtualBox.

## 1. Automated verification

The `verify` target executes the ISO under QEMU without a display, directs the
serial port to a file, and asserts that the string `initialisation complete.`
appears in the captured output. The assertion is deliberately made upon that
fragment rather than upon the whole line, the line naming the sub-task most
recently completed and therefore changing with every advance of `PLAN.md`. It
requires no operator observation and is therefore suitable for use as a
regression test after every change.

The captured output also carries the result of every boot-time self-test. A
self-test that fails reports the fact but does not prevent the kernel from
reaching completion, so the output must be read and not merely the exit status
consulted: the string `FAILED` appearing anywhere within it denotes a regression.

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
exactly one row. The table in `docs/devices/DISPLAY.md`, Section 7.1, pairs each property
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

**Present status**: `VBoxManage` is not installed in this WSL2 environment, so
the target cannot be executed here. VirtualBox is a Windows host application;
execution requires either that the Windows `VBoxManage.exe` be reachable from
WSL2 or that the test be performed from a Windows command prompt against the ISO
in the WSL2 filesystem. The project owner performs this test upon the Windows
host directly, and sub-task 1.11 of `PLAN.md` is recorded as passed upon that
authority.

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

## 13. Test record

| Date | Test | Result |
| ---- | ---- | ------ |
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
