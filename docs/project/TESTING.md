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
failure. The driver now says which of the two causes it is, rather than leaving
"no device answered" to stand for both: see Section 6.2, and
[`../storage/DISK.md`](../storage/DISK.md), Sections 2.2 and 2.3.

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

### 6.2 The decisions no board here can exercise

Two of the driver's decisions cannot be reached upon any machine available to
this project, and both were reported as faults from a machine that is not:

- **Where a channel in native PCI mode answers.** Every board here uses the
  compatibility addresses, so there is nothing to probe.
- **Whether storage that is not of the mass-storage class is storage.** No board
  here presents an SD host controller in the ordinary course.

Both are pure functions of a PCI configuration header — `AtaChannelAddressesFor`
and `AtaClassifyForeignStorage` — and the self-test composes headers no machine
here has and asserts what would be decided about them. The properties are
tabulated in [`../storage/DISK.md`](../storage/DISK.md), Sections 7.2 and 7.3.
This is the only alternative to writing the arithmetic and hoping, and hoping is
what produced both faults.

The second was then observed upon a board composed to have the shape of the
machine that reported it — the HP Laptop 14-dq0052dx of Section 10.1: no disk of
any kind, its system upon a 64 GB eMMC part, booted from a USB drive.

```sh
qemu-system-x86_64 -machine q35,sata=off -cpu qemu64 -smp cores=2 -m 512M \
    -device sdhci-pci -device qemu-xhci,id=xhci \
    -drive if=none,id=usbstick,file=build/oxys.iso,format=raw,media=cdrom \
    -device usb-storage,bus=xhci.0,drive=usbstick \
    -display none -serial stdio
```

`sata=off` removes the q35 board's own AHCI controller, which is what leaves the
mass-storage class empty; the kernel is then booted from the USB drive, as it was
upon the machine in question. Note that the ISO cannot be attached with `-cdrom`
once the SATA controller is gone, that option needing an IDE bus to hang it upon.

### 6.3 The negative tests

Each was applied to the ATA driver (now `drivers/ata/`), confirmed by `make verify`, and
reverted.

| The damage | What the run reported |
| ---------- | --------------------- |
| The class check dropped from the SD host controller's classification, leaving the subclass read alone. | `An SMBus controller was taken for storage.` Subclass `0x05` under the serial-bus class is SMBus, and the report would have offered it as a place the machine's disks might be. |
| An SD host controller classified as nothing. | `An SD host controller was not recognised as storage.` This is the fault as it was reported: a laptop told it has no disk. |
| A mass-storage controller counted as storage outside its own class. | `An IDE controller was reported as beyond this driver's class.` and `An AHCI controller was counted outside its own class.` — two assertions, because the count decides which of the two closing paragraphs is printed, and a machine with no mass-storage controller would have been told to change a firmware setting it does not have. |


### 6.4 Verification of the AHCI disk

`KernelVerifyAhci` asserts three decisions that need no hardware and then the
transfers themselves where a disk answered. The properties are tabulated against
the failure each would catch in [`../storage/AHCI.md`](../storage/AHCI.md),
Sections 8.1 and 8.2.

`make verify` exercises more of this driver than the machine appears to offer.
The q35 board's own AHCI controller answers, and the boot ISO is a packet device
upon port 2, so the discovery, the firmware handoff, the port preparation and the
signature are asserted at every boot; only the transfers report that they had
nothing to do.

The transfers need a disk, which is attached to the same board:

```sh
qemu-img create -f raw sata.img 256G
mke2fs -q -t ext2 -b 1024 -L oxys-ahci -d seed -F probe.img 16384

qemu-system-x86_64 -machine q35 -cpu qemu64 -smp cores=2 -m 512M \
    -cdrom build/oxys.iso \
    -drive file=probe.img,format=raw,if=none,id=sata0 \
    -device ide-hd,drive=sata0,bus=ide.0 \
    -display none -serial file:ahci.log
```

`if=none` with a separate `-device` is what puts the disk upon the board's AHCI
controller rather than upon an IDE bus; `-drive ...,if=ide` would attach it to
the controller the *other* driver reaches, and would prove nothing about this one.

What is compared, and against what: the volume upon `ahci0` is mounted at the
root and its directory listing is compared against `debugfs -R "ls -l /"` upon
the same image from the host. The self-test cannot know what a medium holds, so
this is the corroboration from outside that the driver read the sectors it was
asked for and not some others.

### 6.5 The negative tests of the AHCI driver

Each was applied to `drivers/ahci/ahci.c`, confirmed, and reverted.

| The damage | What the run reported |
| ---------- | --------------------- |
| The port's power state dropped, so that the detection is read alone. | `A port whose interface is not active was called usable.` A port whose device is present but whose interface is asleep would then be issued a command, and the driver's whole patience spent waiting for it. |
| The signature compared upon its low half alone. | `A signature was not recognised as what it names.` All four signatures end in `0101h`, so a packet device would be driven as a disk. |
| The write bit of the command header moved from bit 6 to bit 5. | `The write bit is not at bit 6 of the command header.` A command whose direction is wrong reads the disk into the buffer the caller meant to write from, and reports success. |
| A region descriptor's byte count halved — a byte count mistaken for a word count. | `A sector read twice differs, so less than a whole sector was transferred.` **This one first passed**, and the assertion was strengthened before it caught anything; see below. |
| A region descriptor's byte count written without its **less one**. | Nothing. Recorded as a gap in [`../storage/AHCI.md`](../storage/AHCI.md), Section 8.3: the descriptor is a capacity and the command's sector count is the length, so no adaptor available here ever reaches the extra byte. |

**The fourth is worth recording for what it revealed about the test rather than
the driver.** The first form of the transfer assertion read the same sector twice
and compared the two buffers, which both already held the previous read — so a
transfer that was consistently the wrong length left both holding the same wrong
thing and the comparison passed. The buffers are now seeded with different bytes
before the reads. Wherever the device did not write, the two still differ, and
the halved descriptor is caught at the first byte the device did not reach.

### 6.6 Verification of the SD card and the embedded MultiMediaCard

`KernelVerifySdhci` asserts the capacity and command arithmetic, which need no
hardware, and then the transfers where a card answered. The properties are
tabulated against the failure each would catch in
[`../storage/SDCARD.md`](../storage/SDCARD.md), Sections 7.1 and 7.2.

```sh
mke2fs -q -t ext2 -b 1024 -L oxys-sd -d seed -F sd.img 16384
qemu-img create -f raw sdhc.img 4G

qemu-system-x86_64 -machine q35 -cpu qemu64 -smp cores=2 -m 512M \
    -cdrom build/oxys.iso \
    -device sdhci-pci,id=sd \
    -drive if=none,id=sdcard,file=sd.img,format=raw \
    -device sd-card,drive=sdcard,bus=sd-bus \
    -display none -serial file:sd.log
```

**Both capacity encodings are reached by changing the size of the image alone.**
An image of 16 mebibytes presents as a byte-addressed SD card and one of four
gibibytes as a block-addressed SDHC card, so the driver's two paths are exercised
without either being asked for. The expected reports are

```
  SD card, byte-addressed, address 0x4567, 32768 blocks (16384 KiB)
  SDHC or SDXC card, block-addressed, address 0x4567, 8388608 blocks (4194304 KiB)
```

and both figures are exactly the image divided by 512, which is the corroboration
from outside that the arithmetic of Section 5 of the design document is right.

The seeded volume is then mounted at the root as `/ <- sd0` and its directory
listing compared against `debugfs -R "ls -l /"` upon the same image from the
host, exactly as for the other two drivers.

**VirtualBox presents no SD host controller**, so this driver's live half has no
second machine. The half that needs no hardware runs upon both. That is a
limitation of the available hardware and is recorded as one rather than left for
a reader to infer from its absence.

### 6.7 The negative tests of the SD driver

Each was applied to `drivers/sdhci/sdhci.c`, confirmed, and reverted.

| The damage | What the run reported |
| ---------- | --------------------- |
| The capacity computed by the version 2 encoding whatever the structure field said. | `A version 1 capacity was computed wrongly.` The two encodings disagree upon the same bits, which is what makes the field load-bearing rather than decorative. |
| The version 2 `C_SIZE` shifted by 16 rather than 8 — the offset of the stripped CRC applied twice. | `A version 2 capacity was computed wrongly.` and `The greatest version 2 capacity overflowed or was truncated.` |
| A response of 136 bits composed with the index check enabled. | `A long response was composed with the index checked.` **The card still came up under QEMU**, which does not enforce the check; real silicon does, and every CMD2 and CMD9 would fail upon it. This is precisely why the composition is asserted directly rather than inferred from a card appearing. |
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
provides one, and read from the screen where it does not.

**Sub-task 1.12 is closed**, upon the criterion the project owner set on
2026-09-07: **one machine, booted from a USB medium, with the boot log read and
recorded**. The machine and the run are Section 10.1.

### 10.1 The machine the storage work was reported from

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
runs two. The application-processor bring-up of sub-task 6.14 will therefore
first meet a real machine with more processors than any test has used.

It is a **UEFI-era machine**, and the kernel has no UEFI boot path until Phase
12, so booting it here went through the firmware's compatibility support module.
That is the one thing about this result which does not generalise: a machine
whose firmware offers no such module cannot boot this kernel at all before
sub-task 12.7.

### 10.2 How the boot log was read, there being no serial channel

**The machine has no serial adapter, and this kernel cannot give it one.** Its
external ports are USB Type-C and Type-A, HDMI, a headphone jack and the power
connector; there is no DE-9 port and no 16550 at `0x3F8` for `SerialInitialise`
to find. A USB-to-serial adapter would not help, there being no USB stack in this
kernel to drive one — and reaching a USB device is a longer road than this
project has taken, as [`../storage/DISK.md`](../storage/DISK.md), Section 2.3,
records.

So the log was read **from the screen**, upon the graphical console of sub-task
6.4. This is the same condition VirtualBox presents and it has the same two
consequences, set out in Section 9.1: the automated assertion of Section 1 cannot
be performed here, that assertion being made upon serial output; and the kernel
emits more of the log than the screen holds, so a particular line must be caught
before it scrolls away by the procedure of Section 9.2.

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
`KernelVerifyConsole` — thirty-three assertions in three groups, each tabulated
against the silent failure it catches in
[`../design/GRAPHICS.md`](../design/GRAPHICS.md), Section 21.

The face and its drawing are asserted **against a surface composed in memory**,
as the primitives of Section 16 are, so that the whole of that holds upon a
machine with no display. The four control characters are asserted upon the live
console, because the position they move is the console's own and there is no
second one to make; only characters that draw nothing are used — CR, HT and BS —
so the boot log the test is written into is not disturbed by the test of it.

The exception is the pair of assertions about a backspace crossing to the row
above, added when that path was found to be wrong. A landing cannot be judged
without text upon the row above to land after, so those write upon a fresh line,
assert, erase what they wrote and leave the line blank. Their absence is what let
the fault ship: every assertion here used characters that draw nothing, so the
one case that needs a character drawn was the one case never exercised.

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

### 17.3 The backspace across a line separator

The fault was reported from a real machine: backspacing over letters worked and
backspacing over a line separator did not. The cause was the console putting the
cursor at the right-hand edge of the display when it crossed to the row above,
so the erasure its caller composes — `BS SP BS` — wrote its space a hundred and
fifty columns away from the text. See
[`../design/GRAPHICS.md`](../design/GRAPHICS.md), Section 19.2.1.

It is reproduced by driving the echo loop from outside, which needs the serial
line for input rather than a log file. A socket serves for both, with the monitor
upon a second one so that the screen can be captured while the machine still
stands at the loop:

```sh
qemu-system-x86_64 -machine q35 -cpu qemu64 -smp cores=2 -m 512M \
    -cdrom build/oxys.iso -display none -no-reboot \
    -serial unix:/tmp/oxys-serial.sock,server=on,wait=off \
    -monitor unix:/tmp/oxys-monitor.sock,server=on,wait=off
```

Wait for `A backspace erases, and crosses to the line above.` upon the serial
socket, then send `ab`, a line feed, `cd`, a line feed, and three `0x08` bytes,
a quarter of a second apart so that each is processed as a keystroke would be.
Then `screendump` upon the monitor socket.

| What to look for | What its absence would mean |
| ---------------- | --------------------------- |
| The three backspaces leave `ab` alone upon the screen | The line separator, then `d`, then `c` were each consumed. Both lines standing intact is the fault as reported. |
| Ten backspaces leave neither line, and the banner above them intact | The erase limit still holds across a row crossing — the assertion that keeps an echo loop from eating the boot log. |
| After a hundred and thirty line feeds, the same sequence still erases | The row lengths moved with the text when the display scrolled. |

The third is the case the boot-time self-test cannot reach. The console at 1280
by 800 is a hundred rows tall and has not scrolled by the time the self-test
runs, so removing the shift of the row lengths upon a scroll leaves `make verify`
passing; it is caught here and nowhere else.

### 17.4 The negative tests of the backspace

Each was applied to `graphics/console.c`, confirmed and reverted.

| The damage | What the run reported |
| ---------- | --------------------- |
| The cursor put at the right-hand edge upon crossing, which is the fault as it was reported. | `a backspace crossing to the row above did not land after its text` and `three erasures did not return to the first column`. |
| The filled-row exception dropped, so that a wrapped row is treated as one that ended with a line feed. | `a backspace crossing into a filled row did not stop upon its final character` and the erasure assertion beside it. |
| The shift of the row lengths upon a scroll removed. | `make verify` **passed**, for the reason given in Section 17.3. Confirmed instead at the echo loop: after a hundred and thirty line feeds, `abc` followed by a line feed and three backspaces left `abc` standing untouched. |

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

`KernelVerifyFaultScreen` asserts two things at every boot, and the first governs
the second.

**What is to be done about each exception.** `ExceptionDispositionOf` classifies
every vector, at both privilege levels, as resumed, terminating the program that
raised it, or fatal to the kernel — and **only the last draws a screen**. A
divide by zero raised by a program must cost that program and nothing else; the
kernel treated every exception as fatal until sub-task 6.4, so it would have
halted the machine and announced it, which is a false account of what happened
given to the person least able to check it. The classification is asserted rather
than the behaviour because half of it concerns privilege level 3, where no code
runs until sub-task 6.10, and `ExceptionDispositionOf` is a pure function that
can be asked about a privilege level that does not yet exist.

**The table of screens**: that every fault which threatens the kernel whatever
raised it has a screen of its own, that **no two share a title or a colour**,
that every title fits a 640-pixel display, that every character is one the font
can draw, and that **no screen exists for a fault that can never be the
kernel's**. Each assertion and the silent failure it catches is tabulated in
[`../design/GRAPHICS.md`](../design/GRAPHICS.md), Sections 25.3 and 25.4.

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
`fault-screen=14` to another vector: 2, 6, 8, 10, 11, 12, 13, 14, 18, or 256 for
a panic the kernel raises itself. Any other vector draws the general screen,
which is what a divide by zero within the kernel gets.

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
| Every character drawn, with **no replacement boxes** in the prose | Text outside the printable ASCII the face covers. An em dash in a string literal is three UTF-8 bytes and renders as three boxes. |
| At **640 by 480**: the title still fits, paragraphs re-wrap, and the footer is still on the screen | The layout was fitted to 1280 pixels. |

### 19.2 The negative tests

Four, and the first is the one that matters most: it reproduces the fault this
section was written because of.

**Every fault made fatal.** Remove the privilege-level test from
`ExceptionDispositionOf` in `kernel/cpu/exceptions.c`, so that everything falls
through to `EXCEPTION_DISPOSITION_FATAL`. The run must name all seven program
faults in turn, beginning `a program's own fault would halt the machine rather
than the program, at vector 0x0` — vector 0 being the divide by zero.

**A duplicated identity.**

```sh
sed -i 's/{ 11U, "DESCRIPTOR NOT PRESENT",/{ 11U, "MALFORMED TASK STATE SEGMENT",/' \
    graphics/faultscreen.c
make verify
```

The run must report `two fault screens share a title, at vectors 0xA and 0xB`.

**A deleted screen.** Delete the `{ 18U, "MACHINE CHECK", ... }` row. The run
must report `a fault that threatens the kernel has no screen of its own, at
vector 0x12` — the fault this catches being the one that would otherwise look
like nothing at all, the general screen still naming the vector.

**A screen that can never be drawn.** Add an entry for vector 17, the alignment
check. The run must report `a screen exists for a fault the processor raises only
outside the kernel, at vector 0x11`. Note that the weaker rule — asking merely
whether the vector is ever fatal — does not catch this, `#AC` being nominally
fatal from a kernel selector; the architectural fact that it requires privilege
level 3 has to be stated before the assertion has any force.

Restore the files afterwards. Each run must end `Fault disposition and screen
self-test FAILED.`


## 20. Verification of the compositor

`KernelVerifyCompositing` and `KernelVerifyCompositor` run at every boot. The
first asserts the clip stack and the blend upon surfaces composed in memory, so
it holds upon a machine with no display; the second asserts the damage
arithmetic and the layer table, which is what can be asserted of a compositor
that owns one back buffer and one framebuffer and has no second of either to
compose. Both are tabulated against the failure each would catch in
[`../design/GRAPHICS.md`](../design/GRAPHICS.md), Section 27.5.

### 20.1 What only looking establishes

Three things, and all three are what the sub-task exists for.

| What to look for | What its absence would mean |
| ---------------- | --------------------------- |
| The boot log appears at all, from its first line | The console is drawing into a back buffer nothing carries out, or into the framebuffer while presentations copy the back buffer over it |
| The pointer is drawn **over** the text, its black outline cutting into the letters beneath | The layer is composited under the base, or not at all |
| After the pointer has crossed the screen, **the text it passed over is intact and there is no trail** | Section 27.3: a layer that marks only where it has arrived leaves its previous appearance standing. This is the fault the save-under existed to prevent, and reintroducing it would undo the sub-task |
| A fault screen stays on the screen | Section 27.6: the compositor must be suspended, or the next `KernelWriteString` carries the back buffer over the page |

The pointer is driven from the monitor rather than by hand, so that the path is
repeatable:

```sh
qemu-system-x86_64 -machine q35 -cpu qemu64 -smp cores=2 -m 512M \
    -cdrom build/oxys.iso -display none -no-reboot \
    -serial unix:/tmp/oxys-serial.sock,server=on,wait=off \
    -monitor unix:/tmp/oxys-monitor.sock,server=on,wait=off
```

Wait upon the serial socket for `A backspace erases, and crosses to the line
above.`, then send `mouse_move 20 12` to the monitor a dozen times, a quarter of
a second apart, and `screendump` afterwards. The pointer should stand at the far
end of the path and nothing should mark the path itself.

A trail is easier to measure than to see: a screendump in which no row holds a
run of six or more white pixels except at the pointer's final position is a
screen with one pointer upon it.

### 20.2 The negative tests of the compositor

Each was applied, confirmed, and reverted.

| The damage | What the run reported |
| ---------- | --------------------- |
| A push replacing the clip in force rather than intersecting it. | `a push to a wider region widened the clip` |
| The mask indexed by the destination's width rather than the source's. | `the mask was indexed by the wrong stride` and `the mask was indexed by the destination's width` — but only after the test was corrected; see below. |
| `CompositorMoveLayer` marking only where the layer has arrived and not where it was. | **Nothing at all** from the self-test, and a trail upon the screen: thirteen pointers along the path of one. Measured rather than admired — a screendump in which four rows hold a run of six or more white pixels holds one pointer, and this one had fifty-two. |
| The layers taken by the self-test not given back. | Nothing from the self-test, and a machine that boots **with no pointer** and reports no failure: the table was exhausted, `CursorInitialise` was refused a layer, and every routine in the pointer then correctly did nothing. |

**Two of the four are invisible to every assertion available**, and both were
found by looking at the screen. That is what Section 20.1 is for, and it is why
the compositor's verification is not finished when `make verify` passes.

The second is worth recording for what it revealed about the test rather than
the code. Applied to the test as first written it **passed**: the composited
source and the destination were both sixteen pixels wide, so an index taken from
either was the same number and the fault the assertion named could not be
produced. The source is now seven wide against a destination of sixteen with a
pitch of nineteen, so the three strides differ at every row after the first. An
assertion that cannot fail is worse than no assertion, because it is counted
among those that pass.

## 21. Verification of the mouse and the pointer

Neither self-test needs a mouse, and that is what makes them run at every boot
upon every machine.

**The decoder is driven directly.** `MouseProcessByte` is exposed for the same
reason `KeyboardProcessScancode` is: the decoding of a movement packet is not a
property of the 8042, and a byte arriving by any route decodes identically. So
`KernelVerifyMouse` composes packets — of whichever length the device negotiated
— and asserts the nine-bit sign extension, the inversion of the vertical sense,
the confinement of the position at all four edges, the naming of button
transitions, the discarding of an overflowed movement, the refusal and counting
of a byte that cannot begin a packet, the recovery of the stream afterwards, the
abandonment of a partial packet by a flush, and the behaviour of a full buffer.

**The pointer is asserted upon a surface composed in memory**, whose pitch
exceeds its width and whose padding holds a sentinel. That is what permits the
assertion that a pointer at the edge writes nothing outside the surface, which a
framebuffer could not be asked. It also asserts what a lazier test would omit:
that the **transparent** pixels of the shape still hold the background, without
which a pointer drawn as a solid rectangle would pass.

Every assertion and the silent failure it catches is tabulated in
[`../devices/MOUSE.md`](../devices/MOUSE.md), Sections 8.1 and 8.2.

### 21.1 What is skipped, and why that is not a gap

The assertions that need a device — that one answered, that IR12 was claimed and
unmasked, that the controller agrees the second port is usable — are made only
where `MouseIsPresent`. A machine may genuinely have no mouse, and the driver is
required to discover that **without blocking**; reaching the skip at all is
evidence that it did, every wait upon the controller being bounded.

### 21.2 The negative tests

Apply each, run `make verify`, then revert it.

```sh
# The device's vertical sign forwarded rather than inverted.
sed -i 's|movement_y = -MouseExtendMovement|movement_y = MouseExtendMovement|' \
    drivers/mouse/mouse.c

# The magnitude sign-extended as eight bits rather than nine.
sed -i 's|return (int32_t)magnitude - (negative ? 256 : 0);|(void)negative;\n    return (int32_t)(int8_t)magnitude;|' \
    drivers/mouse/mouse.c
```

For the pointer, change `CursorRestoreUnder` in `graphics/cursor.c` to put the
pixels back at `CursorPositionX`/`CursorPositionY` rather than at
`CursorSavedX`/`CursorSavedY`; or move the `CursorSaveUnder` call in
`CursorDrawAt` from before the drawing loop to after it.

Each run must end `Mouse self-test FAILED.` or `Pointer self-test FAILED.`

### 21.3 A caveat for headless capture

A keystroke injected through the QEMU monitor **before the controller has been
initialised** makes `Ps2Initialise` fail, and the machine then reports no
controller, no keyboard and no mouse. The byte lands in the output buffer after
the single drain at the start of the sequence and is read as the answer to a
command that follows.

It is an artefact of injecting into an emulated controller whose ports are
disabled — a real machine does not scan a disabled port — and it is recorded here
because it is an easy way to spend an hour diagnosing a driver that is working.
When capturing a screenshot, either let GRUB's timeout elapse or wait for the
boot to finish before drawing conclusions from the report.

## 22. Judges this project did not write

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

### 22.1 The judges presently used

| Judge | What it is independent of | Where |
| ----- | ------------------------- | ----- |
| `grub-file --is-x86-multiboot2` | This project's reading of the Multiboot2 header format. Run at every link. | `Makefile`, the `all` target |
| `e2fsck`, `debugfs`, `dumpe2fs` | This project's reading of EXT2. | Sections 7 and 12.1 |
| `clang` | `x86_64-elf-gcc`, and therefore what one toolchain tolerates. | [`TOOLCHAIN.md`](TOOLCHAIN.md), Section 9 |
| Two independent renderings of a specification | A single transcription of a document not publicly distributed. | [`REFERENCES.md`](REFERENCES.md) |
| Real hardware | Every emulator's approximation of a machine. | Sections 10.1 and 10.2 |

Each has already returned something. `grub-file` fails the build outright if the
header is malformed. `e2fsck` found the defect above. `clang` found, upon its
first run, that `kernel/cpu/tss.c` named a 32-bit register to an instruction the
architecture defines upon r/m16 — which GNU `as` had accepted, and assembled
correctly, for as long as the file existed. The physical machine of Section 10.1
found two, and both changed the design rather than the code.

### 22.2 The judges available and not yet used

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

## 23. Test record

| Date | Test | Result |
| ---- | ---- | ------ |
| 2026-09-04 | `make verify` — **sub-task 6.5, the mouse decoder** | Passed. The packet arithmetic is asserted without a mouse and without anybody moving one: a plain packet yields its movement, position and buttons; the vertical sense is inverted at the one place that knows the device meant otherwise; magnitude `0xFF` with the sign bit is −1 and magnitude `0x00` with the sign bit is **−256**, which is the case an eight-bit sign extension turns into zero; the position stops at each of the four edges; a button transition is named once and not twice; an overflowed movement is discarded while its buttons are kept; a byte lacking the always-set bit is refused, counted, and the stream recovers with the next packet; a flush abandons a partial packet; and a full buffer discards the newest and counts exactly what it discarded. |
| 2026-09-04 | `make verify` — the mouse decoder, **the negative tests** | Passed, both. Forwarding the device's vertical sign gave `A movement was decoded with the wrong sense or magnitude.` Sign-extending the magnitude as eight bits gave `A movement of minus 256 was decoded as zero.` — the failure being invisible for every movement but the largest a hand can make in one report period. Both edits reverted. |
| 2026-09-04 | `make verify` — **the pointer**, upon a surface composed in memory | Passed. The shape is well formed — no interior pixel that is not opaque, which is how the two masks would drift apart with no symptom — and the hot spot is part of it. The opaque pixels hold their colours **and the transparent ones still hold the background**, so a pointer drawn as a solid rectangle fails. Showing twice draws once; a move restores the old position; a pointer at the edge draws what fits and writes nothing into the row padding; a concealment nests, so one reveal of two does nothing; a move made while concealed takes effect on the reveal; and draws equal restores once it is hidden. |
| 2026-09-04 | `make verify` — the pointer, **the negative tests** | Passed, both, and the first of them found a real weakness in the code rather than in the test. Restoring the saved pixels at the pointer's position rather than where they came from **passed** at first: `CursorMoveTo` repaired the old location before advancing the position, so the two agreed at every restore and the damage could not show. The order was changed — position first, repair second — which makes the saved coordinates load-bearing; the same damage then gave `The pointer left a trail where it had been.` and three further failures. Saving what is beneath the pointer after drawing it rather than before gave `Hiding the pointer did not restore what was beneath it.` and three further failures. |
| 2026-09-04 | QEMU screendump 1280 by 800 — **the pointer, a person's judgement** | Passed. The arrow is white within a black outline and is legible over the boot log, which is light text upon black: neither colour alone would have been. Its tip is the position it reports. Moved twice through the monitor, it left no trail at either previous position, which is the property the save-under exists for and the one a screenshot judges better than an assertion — an assertion sees a surface in memory, and a person sees the framebuffer. |
| 2026-09-04 | VirtualBox 640 by 480, headless, screenshot | Passed. The interrupt controller reports `IR1 (vector 33): PS/2 keyboard, unmasked` and `IR12 (vector 44): PS/2 mouse, unmasked`, three lines claimed and a mask of `0xEFF8` — the cascade upon IR2 included, IR12 being a line of the slave. The pointer is drawn over the boot log at the centre of the display. This board also has a legacy IDE controller and the disk driver finds it: `ATA: 3 devices, poll...`, `Primary master: ATAPI packet device`. QEMU q35 presents an AHCI controller instead and the same driver finds nothing, the legacy ports not being where an AHCI controller answers. |
| 2026-09-04 | `make verify` — the 8042 given one owner | Passed, with the keyboard's own self-test unchanged: the same decoder assertions, the same modifiers, the same latch behaviour, from a driver that no longer touches the controller. `8042 controller: present, first port usable, second port usable` and `configuration 0x40, translation on, commands 11, timeouts 0, bytes drained 0`. The interrupt controller reports IR1 and IR12 both claimed and unmasked, which is the arrangement the shared configuration byte had to be got right for. |
| 2026-09-04 | `make verify` — **the disposition of every exception** | Passed, and it corrects a real fault rather than adding a feature. Every exception was treated as fatal to the machine, so a divide by zero — the plainest mistake a program can make — would have halted the system and drawn a page saying so. The classification now depends upon the vector **and the privilege level together**: the aborts, the non-maskable interrupt and the two descriptor-table faults are fatal whatever raised them, and everything else terminates the program at privilege level 3 and is fatal only at privilege level 0. All of it is asserted at both privilege levels without raising an exception, `ExceptionDispositionOf` being a pure function — which is the only way the privilege level 3 half can be tested before any privilege level 3 code exists. |
| 2026-09-04 | `make verify` — the disposition, **the negative test** | Passed. With the privilege-level test removed, so that every fault fell through to fatal as it did before this change, the run named all seven program faults in turn: `a program's own fault would halt the machine rather than the program`, at vectors `0x0`, `0x5`, `0x6`, `0xC`, `0xD`, `0xE` and `0x11`. Vector 0 is the divide by zero the fault was reported with. |
| 2026-09-04 | `make verify` — the fault screen table, narrowed | Passed. The screens are now ten and are reserved for what the kernel cannot survive; `#DE` and `#AC` no longer have one. Two new assertions guard that: no screen may exist for a fault that is never fatal, and none for a fault the processor raises **only** outside the kernel. The second was needed because the first does not catch `#AC` — it is nominally fatal from a kernel selector, and only the architectural fact that it requires privilege level 3, `CR0.AM` and `RFLAGS.AC` together gives the rule any force. Confirmed by restoring the `#AC` entry: `a screen exists for a fault the processor raises only outside the kernel, at vector 0x11`. |
| 2026-09-04 | QEMU screendump — **a fault the font found** | The general screen rendered with three replacement boxes in the middle of a sentence. An em dash had reached a string literal instead of staying in a comment, and in UTF-8 that is three bytes, none of them within the printable ASCII the face covers. The font was behaving exactly as designed, which is why nothing failed. Every screen's text is now asserted to be drawable, and the assertion was confirmed by putting an em dash back: `a fault screen's text holds a character the font cannot draw, at vector 0x8, code 0xE2`. |
| 2026-09-04 | QEMU screendump — the narrowed screens | Passed. A divide error now draws `UNEXPECTED KERNEL FAULT` naming `#DE Divide Error`, with the instruction, the stack and the control registers, and states that most exceptions reaching that page are ordinary mistakes of a program — so arriving there means the kernel made one. The non-maskable interrupt screen is new and distinct. The rest were re-titled to say what they are: `KERNEL PAGE FAULT`, `KERNEL PROTECTION FAULT`, `KERNEL STACK FAULT`, `BAD INSTRUCTION IN KERNEL`. |
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
| 2026-09-06 | `make verify` — **the channel addressing**, upon composed headers | Passed. No board here presents an IDE controller in native mode, so the decision is asserted as a pure function of a configuration header: a channel in compatibility mode is not moved whatever its base address registers hold; a native channel takes its command base from its own register and its control base **two bytes into** the next; the secondary channel reads the third and fourth registers; one channel native and the other not is honoured separately; a native declaration with no address, and one describing memory rather than ports, are both refused; and the programming interface is read only for subclass `0x01` — an AHCI controller reporting interface `0x01` would otherwise have its memory registers read as I/O ports, which is exactly what this project's own q35 board presents. |
| 2026-09-06 | `make verify` — **the storage that is not of the storage class** | Passed. An SD host controller and a USB controller are each recognised as storage; a subclass is read only against its own class, so subclass `0x05` under the serial-bus class is SMBus and not somewhere disks might be; an IDE and an AHCI controller are **not** counted here, the count deciding which closing paragraph the report prints; and a function that names nothing is not storage. Composed headers again — no board here presents an SD host controller in the ordinary course. |
| 2026-09-06 | QEMU q35 with `sata=off`, an `sdhci-pci` and the kernel booted from a USB drive | Passed; this is the machine that reported the fault, reproduced. The bus carries an SD host controller at `0:3.0` (class `0x8`, subclass `0x5`) and a USB controller at `0:4.0` (class `0xC`, subclass `0x3`) and no mass-storage controller at all. The report named both, said each has no command block registers, and closed `this machine has no mass-storage controller at all, so there is no firmware setting that would present its storage as a disk`. Before this change the same board was told `the bus carries no mass-storage controller; this machine has no disk`, of a machine that had just booted from its own storage. |
| 2026-09-06 | QEMU q35 with `-device qemu-xhci`, the ordinary ISO | Passed; with the board's own AHCI controller present, the report names it, names the USB controller beneath it, and closes with the firmware remedy — which is the right paragraph here and the wrong one above. That the two are chosen apart is the assertion `An IDE controller was reported as beyond this driver's class.` guards. |
| 2026-09-06 | `make verify` — the disk diagnosis, **the negative tests** | Passed, all three, each reverted afterwards. Reading the subclass without its class gave `An SMBus controller was taken for storage.`; classifying an SD host controller as nothing gave `An SD host controller was not recognised as storage.`, which is the reported fault itself; counting mass storage as foreign gave two failures at once. The procedure is Section 6.3. |
| 2026-09-06 | VirtualBox 7, headless, 512 MiB, legacy BIOS, screenshot | Passed; both new self-tests report soundly and the addressing change alters nothing upon a machine that was already working, which is the property that most needed confirming. The PIIX4 controller is in compatibility mode and both channels are still addressed there — `ATA: primary channel at 0x1F0, control 0x3F6, the compatibility address.` — and the same three devices answer as before, the primary master being the ATAPI drive holding the ISO, so the run reports `devices answered but none is a disk`. This board also carries a `system peripheral (class 0x08, subclass 0x80)`, which is **not** an SD host controller and is not named as storage: the subclass distinction asserted upon composed headers, seen upon a real one. |
| 2026-09-06 | `make verify` — **the backspace across a line separator** | Passed. A backspace at the first column now crosses to the row above and lands after its text, three erasures then return to the first column, a backspace crossing into a row filled to its last column stops upon that final character rather than one past it, and erasing such a row returns to the first column. These are the first assertions of the console self-test that need a character drawn; every one before them used characters that draw nothing, which is exactly why this path was never exercised. |
| 2026-09-06 | `make verify` — the backspace, **the negative tests** | Passed, two of three, and the third is recorded as a gap rather than a pass. Restoring the right-hand edge upon a crossing — the fault as reported — gave `a backspace crossing to the row above did not land after its text`. Dropping the filled-row exception gave `a backspace crossing into a filled row did not stop upon its final character`. Removing the shift of the row lengths upon a scroll left `make verify` passing: the console at 1280 by 800 is a hundred rows tall and has not scrolled when the self-test runs. That case is confirmed at the echo loop instead; see Section 17.3. |
| 2026-09-06 | QEMU q35, the echo loop driven through a serial socket — **the fault as reported** | Passed. `ab`, a line feed, `cd`, a line feed and three backspaces leave `ab` alone upon the screen: the separator, then `d`, then `c` were each consumed. Before the correction the same sequence left both lines standing intact, which is what a person sees as backspacing over letters working and backspacing over a line ending doing nothing. |
| 2026-09-06 | QEMU q35, the echo loop — the erase limit across a crossing | Passed. Ten backspaces after the same two lines leave neither line and leave `A backspace erases, and crosses to the line above.` above them untouched. The limit holds upon the row-crossing path, which is the path that would otherwise eat the boot log a line at a time. |
| 2026-09-06 | QEMU q35, the echo loop after a hundred and thirty line feeds | Passed; this is the test of the row lengths moving with a scroll, which the boot-time self-test cannot reach. `abc`, a line feed and three backspaces leave `a`. With the shift removed the same sequence left `abc` untouched — the row lengths describing rows the text had left. |
| 2026-09-06 | VirtualBox 7, headless, 640 by 480 — **through the keyboard itself** | Passed. Scancodes were injected with `VBoxManage controlvm keyboardputscancode`, so this exercises the whole path a person uses: key, 8042, decoder, echo loop, console. Typing `ab`, Enter, `cd`, Enter and three backspaces left `ab` alone. This display is sixty rows and the boot log has already scrolled it, which QEMU's hundred rows have not. |
| 2026-09-06 | VirtualBox 7, the same session, after seventy further Enters | Passed, and it is the second machine to confirm the row lengths move with a scroll: with the display scrolled by the typing itself, `abc`, Enter and three backspaces left `a`. |
| 2026-09-06 | `make verify` — **sub-task 4.7, the AHCI decisions** | Passed. A port is usable only where the detection says a device is present *and* the power state says the interface is active, with the negotiated speed between them read as part of neither; `DET` of 0, 1 and 4 and `IPM` of 0, 2 and 6 are each rejected; the four port signatures are told apart although all four end in `0101h`; and the command header places the FIS length in double words, the write bit at bit 6 and the region count in the high half. None of these values can be produced by any board here, so each is asked of the decision directly. |
| 2026-09-06 | QEMU q35 — the AHCI adaptor, with no disk attached | Passed, and it exercises more than it appears to. `AHCI: adaptor at 0:31.2, version 0x10000, 6 ports implemented, 32 command slots, 64-bit addressing.` and `port 2: packet device` — the boot ISO, recognised by its signature and correctly **not** registered as a disk. The discovery, the firmware handoff, the port stop-and-restart and the signature all run at every `make verify`. |
| 2026-09-06 | QEMU q35 with a 256 GiB sparse disk upon the AHCI controller | Passed. `port 0: serial ATA disk, 536870912 sectors (268435456 KiB), 48-bit addressing, QEMU HARDDISK`, and the transfers assert what the ATA driver's do and two more besides: a sector beyond the 28-bit limit reads, and a buffer at an **odd** address is refused, the region descriptor having no room for its low bit. |
| 2026-09-06 | QEMU q35 with a seeded EXT2 volume upon the AHCI disk | Passed, and this is the corroboration from outside. The volume was mounted at the root as `/ <- ahci0 (ext2, block 1024, read-only)` and its directory listing — inodes 2, 2, 11, 12, 13, 14, 15 for `.`, `..`, `lost+found`, `hello.txt`, `link`, `oxys-write-test` and `sub` — is exactly what `debugfs -R "ls -l /"` reports of the same image upon the host. The whole stack above the driver read real data through it. |
| 2026-09-06 | QEMU q35 from the `disk write self-test` entry — the AHCI write path | Passed. The final sector was read, overwritten with a pattern, read back, compared byte for byte and restored; `sectors read 8, written 2, device errors 0`. The image's MD5 sum is unchanged afterwards, which is the assertion the kernel cannot make about itself: the restoration was exact. |
| 2026-09-06 | `make verify` — the AHCI driver, **the negative tests** | Passed, three of five, and the other two are recorded rather than claimed. The power state dropped gave `A port whose interface is not active was called usable.`; the signature compared on its low half gave `A signature was not recognised as what it names.`; the write bit moved to bit 5 gave `The write bit is not at bit 6 of the command header.` A halved region byte count **first passed** and the assertion was strengthened until it failed; a byte count without its less-one is not caught at all. The procedure and the reasoning are Sections 6.5 and `docs/storage/AHCI.md` Section 8.3. |
| 2026-09-06 | VirtualBox 7, headless, 640 by 480, an Intel ICH8-M AHCI controller | Passed, upon a second silicon design rather than a second copy of the first. The bus shows `0:13.0 0x8086:0x2829 serial ATA controller (class 0x1, subclass 0x6, interface 0x1)`, the self-test passes, and the seeded volume is mounted as `/ <- ahci0 (ext2, block 1024, read-only)` with the same seven entries and the same inode numbers as QEMU reported. |
| 2026-09-06 | `make verify` — **sub-task 4.8, the capacity and command arithmetic** | Passed. Both encodings of a card's capacity are computed correctly and, upon the same bits, **disagree** — which is what makes `CSD_STRUCTURE` load-bearing rather than decorative; the greatest version 2 capacity neither overflows nor truncates; an unknown structure yields zero rather than a guess; and the command register is composed with the index check off for a response of 136 bits and both checks off for the operating conditions register. No card here uses the first encoding, so the values are composed, each field named by its position in the card specific data rather than in the response. |
| 2026-09-06 | QEMU q35 with a 16 MiB image upon an `sdhci-pci` controller | Passed. `SD: host controller at 0:3.0, specification version 2.00, base clock 52 MHz.` and `SD card, byte-addressed, address 0x4567, 32768 blocks (16384 KiB)` — the **first** capacity encoding, and 32768 blocks is exactly the image divided by 512. The seeded volume mounted at the root as `/ <- sd0 (ext2, block 1024, read-only)` with the same seven entries and inode numbers `debugfs` reports of the image upon the host. |
| 2026-09-06 | QEMU q35 with a 4 GiB image upon the same controller | Passed, and it reaches the other encoding by changing nothing but the size of the image: `SDHC or SDXC card, block-addressed, address 0x4567, 8388608 blocks (4194304 KiB)`, again exactly the image divided by 512. The byte-addressed and block-addressed forms of the transfer argument are both exercised across the two runs. |
| 2026-09-06 | QEMU q35 from the `disk write self-test` entry — the SD write path | Passed. The final block was read, overwritten with a pattern, read back, compared byte for byte and restored; `blocks read 8, written 2, card errors 0`. The image's MD5 sum is unchanged afterwards, which is the assertion the kernel cannot make about itself. |
| 2026-09-06 | `make verify` — the SD driver, **the negative tests** | Passed, all three. The structure field ignored gave `A version 1 capacity was computed wrongly.`; the response offset applied twice gave two failures at once; a long response composed with the index check gave `A long response was composed with the index checked.` The third is the one worth recording: **the card still came up under QEMU**, which does not enforce the check, so the assertion upon the composition is the only thing standing between this driver and silicon that does. |
| 2026-09-06 | VirtualBox 7, headless — sub-task 4.8 | Not applicable, and recorded as such. VirtualBox presents no SD host controller, so the driver reports `SD: no host controller upon this machine.` and its live half cannot run there. The half that needs no hardware — the capacity and command arithmetic — runs upon both machines and passes upon both. |
| 2026-09-06 | VirtualBox 7, headless — **the serial port, which had never worked there** | Fixed and confirmed. The kernel had written nothing to COM1 upon VirtualBox since the driver was written: the presence probe of `SerialInitialise` read the receive register immediately after writing the loopback byte, and VirtualBox does not make it available in the same breath, so the probe concluded there was no adapter. The tell was in the interrupt controller's own report — `lines claimed 3` and no IR4, against `lines claimed 4` upon QEMU — which had been on the screen throughout. The probe now waits for data ready. The log is 10964 bytes and reports 4977 characters transmitted upon 55 interrupts with no line error. |
| 2026-09-06 | VirtualBox 7 — the serial loopback self-test, five consecutive runs | Passed, five of five, with identical log sizes and no failure line. Two further faults were found once the port spoke: a character still being shifted out when the test entered loopback returned as a surplus `0x79` — the letter `y` from the boot log — and a data-ready flag left standing from the previous character caused the second character to be read as `0x00`. Before the corrections the test failed upon roughly half of all VirtualBox boots and upon none of QEMU's, which is the worst way for a test to be wrong. See [`../devices/SERIAL.md`](../devices/SERIAL.md), Section 8.4. |
| 2026-09-06 | `make verify` — the serial corrections upon QEMU | Passed, unchanged: `Serial self-test passed.`, 5037 characters transmitted, `realised 115200 baud`. None of the three faults was ever visible upon this machine, which is why the second machine of the testing mandate exists. |
| 2026-09-06 | `make verify` — **sub-task 6.6, the clip stack and the blend** | Passed. A push intersects the clip in force rather than replacing it, a pop restores what the matching push saved, a pop upon an empty stack and a push beyond the depth are both refused without altering the clip, and a reset abandons the saved clips. Full coverage is exactly an opaque write and no coverage writes nothing; half coverage of white over black is a middling grey with its three channels equal; and red blended over blue produces no green, which is the assertion that the channels are combined apart rather than the packed pixel interpolated whole. |
| 2026-09-06 | `make verify` — the compositor's damage and layer table | Passed. Damage accumulates as the rectangle enclosing both regions rather than as the latest alone, damage outside the buffer is discarded, a presentation empties it, and a presentation with nothing damaged carries no pixels. The layer table takes exactly its capacity, refuses a layer beyond it and one with no surface, and gives back everything the test took. |
| 2026-09-06 | QEMU q35, screendump — **the pointer over the console** | Passed, and this is what the sub-task is for. The pointer is drawn over the boot log with its black outline cutting into the letters beneath, which is compositing rather than the save-under's rectangle. `Compositor: back buffer 1280 by 800, 4000 KiB, 1 layer(s).` and `framebuffer reads 0`. |
| 2026-09-06 | QEMU q35, the pointer walked twelve steps from the monitor | Passed. After crossing four lines of text the pointer stands at the far end of its path, the text it passed over is intact, and **there is no trail**: exactly four rows of the screendump hold a run of six or more white pixels, which is one pointer. |
| 2026-09-06 | QEMU q35 — the compositor, **the negative tests** | Passed, all four, and two of them only because somebody looked. Marking a moved layer's new position alone left thirteen pointers along the path of one and fifty-two rows with white runs, while the self-test reported nothing; the self-test failing to give back the layers it took left the machine with no pointer and no failure. The procedure is Section 20.2. |
| 2026-09-06 | QEMU q35 from the `raise a genuine page fault` entry | Passed. `KERNEL PANIC: An unresolved page fault was raised within the kernel.` and the screen holds `KERNEL PAGE FAULT` with its address, registers and account. The compositor is suspended when a fault screen takes the display; without that the next `KernelWriteString` — and the panic path makes several — would carry the back buffer over the top of the page just drawn. |
| 2026-09-06 | `make verify` — **sub-task 6.7, the dispatch and the validation** | Passed. A number beyond the table is refused and returns `ENOSYS` rather than falling through; the greatest number is refused, which is what makes a negative one safe, the comparison being unsigned. A range that wraps past the end of the address space, one beginning below the user limit and ending above it, one in the kernel's half, and one straddling a mapped page and an unmapped one are each refused; a mapped, user-accessible page is accepted, and the last byte of it is accepted while the first byte beyond is not. A write from the kernel's own memory and a version written into it are both refused. |
| 2026-09-06 | `make verify` — the paging defect the self-test found | Fixed and asserted. Intermediate page-table entries were created present and writable and **never user-accessible**, so a leaf marked accessible to privilege level 3 was unreachable from it: the permissions of a translation are the conjunction of those at every level. The comment in `PagingObtainTable` stated that rule and the code applied it to the write permission alone. It would have appeared at sub-task 6.10 as a fault at the first instruction of the first user program, with a leaf entry that said `USER`; it appeared here because the self-test needed a page it could validate against and made a real one. |
| 2026-09-06 | `make verify` — the system call, **the negative tests** | Passed, three of three, and one of them corrected the test rather than the code. Bounding the call number with `<=` gave `the number one beyond the table was accepted`; checking the first page instead of every page gave `a range straddling a mapped page and an unmapped one was accepted`; writing the wrap check as a sum gave `a length that wraps from a legitimate address was accepted` — **but only after the assertion was corrected**. As first written it used an address above the user limit, which the bounds test refuses before the arithmetic is reached, so it exercised nothing. The address must be below the limit and the length enormous for the sum to be the thing under test. |
| 2026-09-06 | `make verify` — the assertion that was lost | Recorded rather than disguised. The privilege self-test executed `SYSCALL` from privilege level 0 and asserted what the entry point observed; the real path returns by `SYSRET`, which returns to privilege level 3 unconditionally, so it can no longer do so. The alternative was a branch in the entry path returning differently for a caller the kernel trusts, which is a test hook in the one path where a test hook cannot be told from a privilege-escalation bug. What that assertion established is asserted upon the configuration still, and observed in fact at sub-task 6.10. |
| 2026-09-06 | `make verify` — **sub-task 6.8, the ELF64 loader** | Passed. Eleven refusals are asserted **by name**, not merely as refusals: no magic, a 32-bit object, a big-endian one, one for another architecture, a relocatable object, a position-independent executable, a program header table beyond the end of the file, an entry size of zero, no headers at all, more than the loader will read, a segment whose contents lie beyond the file, one larger in the file than in memory, one in the kernel's half, one beginning below the boundary and ending above it, and segments out of ascending order. A loader that refused everything would satisfy a test that only checked *that* it refused. |
| 2026-09-06 | `make verify` — the loader, what it placed | Passed, and this is what the rest is built toward. A composed image of two segments sharing a page is loaded into an address space, that space is made active, and the contents are read at the addresses the image asked for: both segments match byte for byte, the memory beyond the second segment's file contents reads as zero, the shared page is writable, and the text is accessible to privilege level 3. |
| 2026-09-06 | `make verify` — the loader, **the negative tests** | Passed, three of three, and one only after the test was strengthened. Mapping a fresh frame over the page the two segments share gave `the first segment's contents are not what the image held`; leaving that page with the first segment's permissions gave `the page the two segments share is not writable`; not zeroing a frame before copying into it gave `the memory beyond a segment's file contents was not zeroed` — **but passed before the test dirtied its frames first**. Upon a freshly booted machine the allocator hands out frames that are already zero, so a loader that never zeroed anything passed. The test now fills a handful of frames with a pattern and gives them back before the load, which is the state frames are in upon any machine that has been running for more than a moment. |
| 2026-09-06 | `make verify` — **sub-task 6.9, the process and thread structures** | Passed. A process is given an address space of its own and two processes are not given the same one; a child records its parent by number; a thread knows its process and the process records the thread; a thread's stack pointer begins at the top of its stack and the top is the end of its reservation; two threads of one process have different stacks. |
| 2026-09-06 | `make verify` — the guard page, and `rsp0` | Passed, and both were promises `docs/design/PRIVILEGE.md` made to this sub-task. The page beneath a thread's kernel stack is **not** writable and the first page of the stack is, so an overflow's push faults rather than running into whatever lies below — where before there was one stack whose overflow ran into the double-fault stack by accident of placement. `ThreadSetCurrent` points `rsp0` at the current thread's stack and follows it when another becomes current; `TssSetKernelStack` had existed and been uncalled since sub-task 6.1 for exactly that. |
| 2026-09-06 | `make verify` — the balance of the allocations | Passed. Destroying a process destroys its threads, a destroyed thread is no longer the current one, and **the arena returns to exactly what it held before the test**. That last is the assertion that catches the widest class of fault here: every other one is about a field, and that one is about whether the whole sequence of allocations and releases balanced. A new process does not take a destroyed one's identifier, so a parent outliving its child finds nobody rather than finding whoever was given that slot next. |
| 2026-09-06 | `make verify` — the process structures, **the negative tests** | Passed, all three. The guard page left writable gave `the page beneath a thread's stack is writable, so it is not a guard`. A process destroyed without destroying its threads gave `a thread outlived the process that owned it` **and** `the arena did not return to what it held before` — the second being the one that would have caught it even had the first been thought unnecessary. `TssSetKernelStack` left uncalled gave `making a thread current did not point rsp0 at its stack` and `rsp0 did not follow the thread that became current`. |
| 2026-09-06 | `make verify` — **sub-task 6.10, the context switch** | Passed. A second kernel thread is created, switched to, runs once, and switches back — and execution resumes upon the line after the switch, which is the assertion: a switch that restored the incoming thread and lost the outgoing one never reaches it, and the machine would stop rather than report anything. It is asserted alone, between two threads of the kernel, because a failure there is a failure of the switch where a failure in the descent could be a failure of anything. |
| 2026-09-06 | `make verify` — **a program at privilege level 3** | Passed, and this is the sub-task. Twenty-nine bytes of machine code are assembled by hand, wrapped in an ELF image, loaded by sub-task 6.8 into an address space made by 6.9, and entered by `IRETQ`. The program writes a string through a system call — `A program at privilege level 3 wrote this line through a system call.` appears in the log, put there by ring 3 — and then executes an undefined instruction, which is a fault belonging to it. The kernel ends the program and carries on. |
| 2026-09-06 | QEMU — the fault trace, read field by field | Passed. `CS 0x2B` is the user code selector with a requested privilege level of 3 and `SS 0x23` the user data selector likewise, so the program was at privilege level 3 and not merely at an address a program would use. `RIP 0x40101B` is the twenty-eighth byte of its text, so every instruction before the fault ran. `RSP 0x700000000000` is the top of the stack the process was given. `RAX 0x48` is seventy-two — the length the write returned, in the program's own register. `RCX` holding the return address and `R11 0x202` the flags is `SYSRET` doing exactly what sub-task 6.7 said it would. |
| 2026-09-06 | `make verify` — sub-task 6.10, **the negative tests** | Passed, three of five, and both of the others are recorded rather than claimed. The code selector pushed without its requested privilege level gave `KERNEL PANIC: An unrecoverable processor exception was raised within the kernel` — the descent did not descend and the program ran at privilege level 0. The address space not switched gave a page fault at the entry point and `User mode self-test FAILED`, caught by the exit status being −14 rather than −6. The termination not performed gave `A fault outside the kernel was raised by nothing this kernel started`, which is the behaviour this path had before this sub-task. `RFLAGS` pushed without bit 1 changed **nothing**: the processor forces the bit whether or not it is written, so the comment that justified writing it was corrected to say so. And the sixth was found during development rather than as a test: six preserved registers written onto a prepared frame gave a return to address zero, the switch keeping them in the context structure rather than upon the stack. |
| 2026-09-07 | `make verify` — **after the division of `kernel/fs/ext2.c`** | Passed, and this is the test the division rested upon. All four EXT2 self-tests — the format, directories and path resolution, files and symbolic links, allocation and writing, names and creation — report sound against the composed volume, as does the virtual filesystem layer above them. The division was then checked mechanically rather than by eye: the 117 function definitions of the original and of the nine files that replace it are the same 117, and every non-comment line of the original is present in the new files but for seven, each of which is an intended edit — five `static` qualifiers removed from helpers that crossed the new boundaries, two `#define`s moved into the private header, and two declarations given initialisers. |
| 2026-09-07 | `make verify` — **the ELF loader's two new refusals** | Passed, after failing once. A segment occupying the first page of the address space is refused; a segment of no memory size in the kernel's half is refused; and a segment of no memory size at a legitimate address is accepted and contributes no pages. The second failed on its first run — against the very change it was written for, the skip having been placed before the address check rather than after it, so the empty segment escaped judgement exactly as before. The assertion was written from the generic ABI rather than from the code, which is why it disagreed with it. |
| 2026-09-07 | `make verify` — **the three corrected reports** | Passed. `Low identity mapping: removed.` is now printed at both points in the boot at which `PagingReport` runs, where the second had said `PRESENT (unexpected)` upon every healthy boot since sub-task 6.7. The process report reads `no thread is current; 1 program(s) have run and ended` where it had said `nothing has run`, a few lines below the output of the program that ran. The completion banner names sub-task 6.10 rather than 6.1, and retains the words `initialisation complete.` that this target greps for. |
| 2026-09-07 | `make verify` — **after the division of the virtual filesystem layer, the EXT2 self-test and the ATA driver** | Passed, and identical to the run before them. Thirty-nine assertions report passed or sound and none reports a failure, which is the same count and the same lines as the pre-division build; the two serial logs differ only where they always differ between runs. The divisions were then checked mechanically rather than by eye, as the EXT2 division was: for each of the three, every non-comment line of the original is present in the files that replace it, once the `static` qualifiers removed at the new boundaries and the constants and one structure moved into the private headers are accounted for. The remainder is empty in all three cases. |
