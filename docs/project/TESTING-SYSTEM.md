<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# Testing: the Devices, the Storage Stack and the Kernel

**Authority**: `PROJECT_GUIDELINES.md`, Section 2, the testing mandate.

**What is here**: the verification of everything but the graphical work — the
text-mode display, the serial receive path, the backspace that crosses a row
boundary, the disk, the EXT2 superblock, the virtual filesystem layer, the
privilege apparatus, the interrupt controllers, the concurrency primitives, the
scheduler, the C library's string functions, its system-call wrappers and its
heap — each with the procedure, what it establishes, and the negative test that
confirmed the assertion was worth making.

**Section 12 is the first whose subject is not the kernel**, Section 13 the
second and Section 14 the third. All three are here rather than in a document of
their own because the procedure is this one: the kernel asserts them at boot, and
`make verify` reads the verdict out of the serial log. **Sections 13 and 14 have
subjects the kernel cannot call at all**, and Sections 13.1 and 14.1 are what is
done about that — Section 14's subject dividing into a half that can be called
and a half that cannot, which is why that test is in two pieces.

**The other three**: [`TESTING.md`](TESTING.md) is how the machine is tested and
in which environments; [`TESTING-GRAPHICS.md`](TESTING-GRAPHICS.md) is the
graphical half of this; [`TESTING-RECORD.md`](TESTING-RECORD.md) is the dated
record of what has actually been run. `TESTING.md` records why there are four.

**The sections were regrouped, not rewritten.** They are in ascending order of
their original numbering and their text is unchanged; only the numbers moved.

---

## 1. Verification of the text-mode display

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

## 2. Verification of the serial receive path

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

## 3. Verification of the backspace across a row boundary

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

## 4. Verification of the disk

`make verify` runs upon the q35 board, whose storage controller is AHCI; no
device answers the ATA driver there, and the disk self-test reports as much and
asserts nothing. That is the correct outcome upon that machine and is not a
failure. The driver now says which of the two causes it is, rather than leaving
"no device answered" to stand for both: see Section 4.2, and
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

### 4.1 The write path

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

### 4.2 The decisions no board here can exercise

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
machine that reported it — the HP Laptop 14-dq0052dx of [`TESTING.md`](TESTING.md), Section 5.1: no disk of
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

### 4.3 The negative tests

Each was applied to the ATA driver (now `drivers/ata/`), confirmed by `make verify`, and
reverted.

| The damage | What the run reported |
| ---------- | --------------------- |
| The class check dropped from the SD host controller's classification, leaving the subclass read alone. | `An SMBus controller was taken for storage.` Subclass `0x05` under the serial-bus class is SMBus, and the report would have offered it as a place the machine's disks might be. |
| An SD host controller classified as nothing. | `An SD host controller was not recognised as storage.` This is the fault as it was reported: a laptop told it has no disk. |
| A mass-storage controller counted as storage outside its own class. | `An IDE controller was reported as beyond this driver's class.` and `An AHCI controller was counted outside its own class.` — two assertions, because the count decides which of the two closing paragraphs is printed, and a machine with no mass-storage controller would have been told to change a firmware setting it does not have. |


### 4.4 Verification of the AHCI disk

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

### 4.5 The negative tests of the AHCI driver

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

### 4.6 Verification of the SD card and the embedded MultiMediaCard

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

### 4.7 The negative tests of the SD driver

Each was applied to `drivers/sdhci/sdhci.c`, confirmed, and reverted.

| The damage | What the run reported |
| ---------- | --------------------- |
| The capacity computed by the version 2 encoding whatever the structure field said. | `A version 1 capacity was computed wrongly.` The two encodings disagree upon the same bits, which is what makes the field load-bearing rather than decorative. |
| The version 2 `C_SIZE` shifted by 16 rather than 8 — the offset of the stripped CRC applied twice. | `A version 2 capacity was computed wrongly.` and `The greatest version 2 capacity overflowed or was truncated.` |
| A response of 136 bits composed with the index check enabled. | `A long response was composed with the index checked.` **The card still came up under QEMU**, which does not enforce the check; real silicon does, and every CMD2 and CMD9 would fail upon it. This is precisely why the composition is asserted directly rather than inferred from a card appearing. |
## 5. Verification of the EXT2 superblock

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

## 6. Verification of the virtual filesystem layer

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
write self-test)" mounts it for writing, performs the probe of Section 6.1, and
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

### 6.1 The write probe

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

## 7. Verification of the privilege apparatus

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

### 7.1 The negative test

A self-test is worth nothing until it has been seen to fail. To repeat it:

```sh
# Remove the interrupt flag from the mask the kernel writes into IA32_FMASK.
sed -i 's/RFLAGS_TRAP | RFLAGS_INTERRUPT_ENABLE |/RFLAGS_TRAP |/'     kernel/include/oxys/arch/syscall/syscall.h
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

## 8. Verification of the interrupt controllers

Sub-task 6.12 replaced the pair of 8259A controllers with the machine's own Local
APIC and I/O APIC, programmed from what the firmware's ACPI tables declare. The
reasoning is in [`../devices/ACPI.md`](../devices/ACPI.md) and
[`../devices/APIC.md`](../devices/APIC.md); what follows is how it is tested.

### 8.1 Why this change needs more than an automated pass

An interrupt controller that is programmed wrongly reports nothing. It produces a
device that is silent, and a silent device is indistinguishable from a device
that is absent, from a driver that was never initialised, and from a machine with
nothing attached.

Worse, most of the machine goes on working. When this sub-task was first run, the
interval timer's input had been taken by another line and the timer had stopped —
and the kernel booted to its banner, echoed keystrokes, and carried its serial
log exactly as before. Nothing in the boot log said anything was wrong, because
nothing was wrong with any of the things the boot log was reporting on.

The self-tests are written against that, and the assertion that catches it is the
one that lets the timer run and counts its ticks.

### 8.2 What `make verify` asserts

Five routines, in the order `KernelMain` runs them.

| Routine | When it runs | What it establishes |
| ------- | ------------ | ------------------- |
| `KernelVerifyPic` | After `IrqInitialise` | The 8259A device: its mask registers respond, the cascade is unmasked with any slave line, and — the one assertion that cannot be made by inspection — the remapping took effect. |
| `KernelVerifyIrq` | Immediately after | The routing layer while the 8259A still answers: a claimed line reaches its handler, an unclaimed one is acknowledged and counted, and a spurious request upon IR7 is neither routed nor acknowledged. |
| `KernelVerifyAcpi` | After `AcpiInitialise` | That a parse claiming success is coherent: a pointer and a directory were found, at least one processor and one local controller address were declared, every override names the ISA bus, and every line without an override resolves to the global interrupt of its own number. |
| `KernelVerifyLocalApic` | After `LocalApicInitialise` | Both enables set, the spurious vector reading back as programmed, the task priority accepting every class, neither LINT pin in the external delivery mode, the local timer masked, the version register reading as an APIC's, and no error reported. |
| `KernelVerifyIoApic` | After `IoApicInitialise` | Every unit reporting between 1 and 240 inputs, and every input above the request lines masked. |
| `KernelVerifyApicRouting` | After `IrqAdoptApic` | The 8259A fully masked and reporting itself retired; every claimed line's redirection entry carrying the vector that line has always had, naming this processor, and masked exactly as its driver asked; and **the interval timer still ticking**. |

The tables in [`../devices/ACPI.md`](../devices/ACPI.md), Section 7, and
[`../devices/APIC.md`](../devices/APIC.md), Section 8, pair each assertion with
the silent failure it exists to catch.

### 8.3 What the log should say

The passage worth reading is the one between the ACPI report and the request
layer's final report. Under QEMU with `-machine q35 -cpu qemu64 -smp cores=2`:

```
ACPI table self-test passed.
ACPI: pointer at 0x15EA60, revision 0, from the boot loader's copy.
ACPI: directory RSDT at 0x1FFE2369, tables 5: FACP APIC HPET MCFG WAET.
ACPI: local controllers at 0xFEE00000, processors 2 of which usable 2, 8259A pair declared present.
  I/O APIC 0 at 0xFEC00000, global interrupts from 0.
  ISA request 0 is carried by global interrupt 2, flags 0x0.
  ...
Local APIC self-test passed.
Local APIC: identifier 0, version 0x14, local vector entries 6, registers at 0xFEE00000, the bootstrap processor.
Local APIC: spurious vector 255, error vector 254, LINT0 0x10000, LINT1 0x400.
I/O APIC self-test passed.
I/O APIC: 1 unit(s), 24 interrupt inputs between them.
Interrupt requests: no I/O APIC input carries line 2.
APIC routing self-test passed.
8259A: retired in favour of the APIC; mask 0xFFFF.
  Global interrupt 2 presents vector 32 to processor 0, active high, edge triggered.
Interrupt requests: answered by the I/O APIC and the Local APIC, vectors 32 to 47, lines claimed 4.
  Line 0 (vector 32): interval timer, global interrupt 2, unmasked.
```

Three lines of it are the substance and are worth naming.

**`ISA request 0 is carried by global interrupt 2`** is the firmware saying the
timer is not upon the input its number would suggest. A kernel that did not read
that would programme input 0 and never hear a tick.

**`no I/O APIC input carries line 2`** is the consequence: global interrupt 2 now
belongs to line 0, so line 2 — the 8259A's cascade, which is not a device line
under any controller — has nothing. Its absence is correct and is reported rather
than passed over.

**`Line 0 (vector 32): interval timer, global interrupt 2, unmasked`** is the
whole change in one line: the same driver, upon the same line, at the same
vector, carried by a different controller through a different pin.

### 8.4 The negative tests

Each was performed by editing the source, rebuilding, executing under QEMU,
reading the log, and reverting. What is recorded is what the log said.

| The edit | What was observed |
| -------- | ----------------- |
| **None.** The ownership rule of `IrqLineOwnsItsInput` did not exist when the sub-task was first run | `A claimed line is routed to the wrong vector.`, and the report showed `Line 0 (vector 32): interval timer, global interrupt 2, masked` — line 2 having overwritten line 0's entry. The machine booted to its banner, echoed keystrokes and carried its serial log throughout, with the timer dead. **This is not a contrived negative test but the defect the self-test caught on its first run**; it is recorded in [`../design/INTERRUPTS.md`](../design/INTERRUPTS.md), Section 10.7. |
| Omit `PicDisable` from the adoption | `The 8259A pair is not fully masked.` and `APIC routing self-test FAILED.` |
| Give the redirection entries a vector below 16 | `Interrupt requests: no I/O APIC input carries line 0, which a driver has claimed.`, then `A claimed line is routed to the wrong vector.` four times. `IoApicRouteGlobalInterrupt` refuses the vector rather than programming it, so no entry is written at all and the four claimed lines keep the masked entries the initialisation left. The refusal is what makes this loud: without it the Local APIC would record an illegal vector in a register nothing reads. |
| Skip the write to the task priority register | Nothing. `make verify` passed unchanged, QEMU's firmware leaving the register clear. The assertion exists for a firmware that does not, and cannot be provoked upon one that behaves — which is recorded rather than glossed, an assertion that cannot fail here being an assertion this environment does not test. |
| Leave the spurious vector's software enable clear | `The software enable of the spurious vector register is clear.` and `Local APIC self-test FAILED.`, then `The interval timer stopped when the I/O APIC took over its request line.` Every device goes silent at once. |
| Name a destination other than this processor's local APIC identifier | `A claimed line is directed at another processor.` four times, then `The interval timer stopped when the I/O APIC took over its request line.` This was recorded as what sub-task 6.14's characteristic failure would look like; 6.14 has since arrived without producing it, every redirection entry still naming the bootstrap processor. |
| Ignore the boot loader's ACPI tag, forcing the low-memory search | `ACPI: pointer at 0xF52C0, revision 0, from the BIOS read-only memory.` — a different address, by a different route, naming the same `RSDT at 0x1FFE2369` and the same five tables, and every self-test passed. This is a **positive** negative test: it is the only thing that exercises the search of ACPI 6.5, Section 5.2.5.1, at all, GRUB always supplying the tag. |
| Write the redirection entry low half first | **Not attempted.** The window is a few instructions wide and the interrupt flag is clear throughout the adoption, so there is nothing to observe. The order is prevented by construction and recorded in [`../devices/APIC.md`](../devices/APIC.md), Section 4.3, rather than asserted. |

**One of these changed the kernel.** Naming the wrong destination made the run
outlast this target's twenty-five second timeout, so the failure was reported as
a kernel that never reached its banner rather than as a timer that had stopped —
`PitWaitTicks` is bounded by iterations *per tick awaited*, and a dead timer
makes it spin for a multiple of a bound chosen to be generous. The routing
self-test now waits by a fixed spin instead, which costs the same whether the
timer runs or not. The negative test was then repeated and produced the message
recorded above.

### 8.5 What is not tested, and cannot presently be

Several paths are written and have never been taken by any run, because no
machine this kernel has been booted upon presents the conditions:

- **The XSDT.** GRUB supplies the ACPI 1.0 tag under a legacy BIOS boot, so the
  RSDT is what is walked. The XSDT path awaits either a machine whose firmware
  is ACPI 2.0 throughout or the UEFI boot of Phase 12.
- **A Local APIC Address Override.** No machine has declared one.
- **A second I/O APIC**, and any global system interrupt base above zero.
- **Processor Local x2APIC structures.** QEMU declares type 0 for both
  processors.
- **A level-triggered or active-low request line.** QEMU declares four such
  overrides — ISA 5, 9, 10 and 11 — but no driver in this kernel claims any of
  those lines, so the flags are read and recorded and never programmed.

These are recorded rather than glossed. [`STATUS.md`](STATUS.md), Section 3,
carries the same list against the environments column, which is where a reader
looking for what has actually been run will look.

## 9. Verification of the concurrency primitives

Sub-task 6.13 added a spinlock, a per-processor data area, an inter-processor
interrupt and a translation-lookaside-buffer shootdown. The design is
[`../design/CONCURRENCY.md`](../design/CONCURRENCY.md); what follows is how it is
tested, and why the testing is shaped as it is.

### 9.1 The difficulty this section exists for

**A lock that does not lock behaves exactly like a lock that does, upon a machine
with one processor.** Every mechanism of this sub-task has that property. There
is one thread of control, so nothing contends, nothing races, and a completely
broken implementation boots to the banner and echoes keystrokes exactly as a
correct one does.

That was not a reason to defer the testing to sub-task 6.14. It was a reason to
test something other than behaviour. Three routes are available, and all three
are used:

1. **Assert the internal state.** The tickets, the owner, the counted depth, the
   interrupt flag: these are wrong or right today exactly as they will be when a
   second processor depends upon them, and a single processor can be made to show
   every one of them.
2. **Send the interrupt to oneself.** Intel SDM, Volume 3A, Section 10.6.1, gives
   the self shorthand as a delivery like any other — the same command register,
   the same gate, the same handler, the same end-of-interrupt. The whole
   inter-processor path is therefore exercisable upon one processor, and this is
   the argument [`../design/ARCHITECTURE.md`](../design/ARCHITECTURE.md), Section
   4.1, placed the sub-task before 6.14 upon. Sub-task 6.14 has since supplied
   real targets, and `KernelVerifyApplicationProcessors` asserts against them;
   the three routes below remain what 6.13 could be shown by.
3. **Create the damage deliberately.** The shootdown test rewrites a page-table
   entry by hand and invalidates nothing, which puts this processor in exactly
   the state a mapping changed upon another processor would — and then requires
   the shootdown handler to be what repairs it.

### 9.2 What `make verify` asserts

Four routines, in the order `KernelMain` runs them, all after the routing has
been adopted by the I/O APIC — an inter-processor interrupt goes out through the
local controller's command register, so none of this can be asserted before that
controller is enabled.

| Routine | What it establishes |
| ------- | ------------------- |
| `KernelVerifyPerCpu` | The area is reachable and correct: its self pointer names itself; `GS.base` holds it and `IA32_KERNEL_GS_BASE` holds zero, **read back from the model-specific registers**; its APIC identifier agrees with what the controller reports; its kernel stack is the one the task state segment names; and no critical section or lock is outstanding. |
| `KernelVerifySpinlock` | An acquire masks interrupts, takes a ticket, names this processor as the owner and leaves the lock exactly one ticket ahead of its serving number; a nested section deepens and restores the count without re-enabling the flag; a release restores the flag to what the acquire *found*; an uncontended acquire is not counted as a contention; and a conditional acquire of a held lock fails rather than waits. |
| `KernelVerifyIpi` | An interrupt sent with the flag clear is held and delivered when it is set; an interrupt this processor sends to itself arrives; **a second interrupt of the same vector arrives**, which is the only available evidence that the handler signalled the end of the first; and the controller reported no error and abandoned no send. |
| `KernelVerifyShootdown` | A window whose page-table entry has been changed behind the processor's back reads through the **new** mapping after a shootdown; the handler ran exactly once; the address it was given is the address requested; and nothing was abandoned. |

### 9.3 The two panics, tested by hand

Neither of the spinlock's two guards can be exercised by an automated pass, both
ending in `KernelPanic`. Both were tested on 2026-09-09 by temporarily inserting
the misuse at the end of `KernelVerifySpinlock`, running `make verify`, reading
the captured serial output, and reverting the edit.

| Misuse inserted | What the run reported |
| --------------- | --------------------- |
| A second `SpinlockAcquire` of a lock already held | `Spinlock: an acquisition of a lock already held by the acquiring processor.` followed by `Spinlock "self-test": ticket 3, serving 2, waiters 1, owner 0, acquisitions 3, contended 0.` and `KERNEL PANIC: A processor acquired a spinlock it already held.` Without the guard this is a machine that stops inside a lock nothing names. |
| A `SpinlockRelease` of a lock not held | `Spinlock: a release by a processor that does not hold it.` followed by `Spinlock "self-test": ticket 2, serving 2, waiters 0, owner none, acquisitions 2, contended 0.` and `KERNEL PANIC: A spinlock was released by a processor that did not hold it.` |

The report before the panic is the point of both. A panic carries one string, and
what is wanted here is four numbers: which lock, who holds it, who is waiting,
and how many are behind them.

### 9.4 Reading the log

Five lines and the report say the whole of it:

```
Per-processor area self-test passed.
Spinlock self-test passed.
Interprocessor interrupt self-test passed.
  The stale translation was observable before the shootdown.
TLB shootdown self-test passed.
```

**These five run before `SmpInitialise`**, and `KernelVerifyPerCpu` asserts that
one processor is online at that point. Since sub-task 6.14 that is a statement
about the ordering and not about the machine: moving the corpus after the
bring-up would make the assertion fail upon every multiprocessor machine, and
[`../../kernel/test/arch/smp.c`](../../kernel/test/arch/smp.c) says so where
it would be read.

The fourth line is reported rather than asserted. The architecture nowhere
requires a processor to cache a translation it has used, so an equality there
would mean the test proved less than it hoped rather than that anything was
wrong. Under QEMU it is observable, and the run says so — which is what makes the
shootdown assertion above it a demonstration and not a tautology.

`ShootdownReport` prints more services than requests, and that is correct:
`KernelVerifyIpi` sends the shootdown vector twice on its own account, to
establish the delivery and the end-of-interrupt, without publishing a request.

## 10. Verification of the application processors

**Corresponding sub-task**: 6.14. **Design**:
[`../design/SMP.md`](../design/SMP.md), Section 8.

### 10.1 The difficulty this section exists for

**A count of processors is not evidence that there are any.** A kernel that
incremented a variable and started nobody produces the same count, the same
report and the same closing banner, and boots to its echo loop exactly as a
correct one does. Every quantity the bring-up prints about itself is a quantity
the bring-up itself wrote.

What only a running processor can produce is an **acknowledgement to an interrupt
it was sent**, written into an area that processor alone writes, from inside a
handler that processor alone runs. That is the whole of what
`KernelVerifyApplicationProcessors` rests upon, and it is why the test's substance
is a shootdown broadcast rather than a comparison.

### 10.2 What `make verify` asserts

The identity mapping at `0x8000` is gone. Every area within the online count
exists, is marked online, holds the index it is at, and its `self` names itself;
no two carry the same local controller identifier; index 0 is the bootstrap
processor and no other claims to be. The online count equals the started count
plus one.

Then, for each started processor, what **that processor** read out of its own
registers with the instructions that read them — `STR`, `SGDT`, `SIDT`, and the
control registers — against what the bootstrap processor reads from its own:

| Register | What its disagreement would mean |
| -------- | -------------------------------- |
| Task register | A processor with no task state segment. It runs correctly until its first double fault, which is then a triple fault: a gate naming an interrupt stack table entry cannot be delivered upon a processor whose task register is null. |
| `GDT` base and limit, `IDT` base | A processor still upon the trampoline's own table, which is about to be unmapped. |
| `CR3` | A processor upon a paging hierarchy of its own. |
| `CR0.PG` | Long mode not actually entered. |
| `CR0.WP` | **The one nothing else would report.** Without it that processor may write the kernel's own text while its fellows may not, and nothing faults, ever. |
| `CR4.PAE` | Long mode cannot have been entered at all. |

And last, the assertion the test exists for: a shootdown is broadcast, and each
target's own `shootdowns_serviced` is read afterwards and must have risen.
`ShootdownBroadcast` waits for every acknowledgement before it returns, so a
target that did not answer would have made the broadcast itself fail.

### 10.3 Upon a machine with one processor

The test asserts the other side of the same coin, and it is not a skip: that
nobody was started, that `SmpDeclinedReason` names the condition that produced it,
that no processor is reported started that is not online, and that the online
count is one. A test that reported nothing there would be a test that passed upon
a machine where the bring-up silently did nothing.

### 10.4 The negative tests

| Change | What the run said |
| ------ | ----------------- |
| **None.** `SmpMapTrampolinePage` had its call to `PagingMapKernelPage` disabled when the sub-task was first run | `#PF` at `CR2 0x8000`, error code `0x2` — page not present, write, supervisor mode — raised inside the copy of `SmpPlaceTrampoline`, and `KERNEL PANIC: An unresolved page fault was raised within the kernel.` with no banner. **This is not a contrived negative test but the defect the first run met**; the symptom names the cause exactly, the identity mapping being the one thing this kernel otherwise does not have and the one thing the bring-up requires. |
| **None.** `IA32_PAT` was not written upon the started processor when the sub-task was first run | **Nothing.** Every self-test passed, the banner appeared, and the display looked correct. The started processor was writing the framebuffer through a mapping carrying the page-attribute-table flag while its own entry 4 still held write-back — one physical page under two memory types, which Intel SDM, Volume 3A, Section 11.12.4, declines to define. It was found by reading [`../design/FRAMEBUFFER.md`](../design/FRAMEBUFFER.md), limitation 2, against the new entry path, and by no run. It is recorded here because it is the shape of defect this whole section exists for: correct-looking output from a machine in an undefined state. |
| Park the started processor with `cli; hlt` rather than `sti; hlt` | The log stops dead after `Processor 1 is online, local controller identifier 1.` — no banner, no panic, nothing further within the 25-second bound `make verify` allows. The cause is `SmpUnmapTrampolinePage`: its shootdown is never acknowledged by a processor that cannot take an interrupt, and `ShootdownBroadcast` spins out `SHOOTDOWN_WAIT_LIMIT`, which is 100,000,000 iterations and outlasts the timeout. **The eventual panic is correct and arrives far too late to be the diagnostic**, which is worth knowing: the observable symptom of an unresponsive processor here is a hang, not a report. |
| Skip the wait for a started processor to come online | **Nothing. `make verify` passed unchanged.** This is recorded because the prediction was wrong and the reason is the environment: QEMU declares two processors, so there is exactly one application processor, so the loop never starts a second and the prepared index can never collide with a claimed one. The panic in `SmpApplicationProcessorEntry` — `A starting processor claimed an area other than the one prepared for it.` — is therefore **unreachable upon every machine this project has tested against**. It is retained because the serialisation of [`../design/SMP.md`](../design/SMP.md), Section 7, is what makes the two indices agree, and a check that fires loudly when that stops holding is worth more than one that was proven to fire here. |
| Give a started processor no task state segment | **Nothing, at the time.** It came online, answered a shootdown, and passed every assertion that then existed — and would have taken a triple fault upon its first double fault. That negative test, recorded in the header of [`../../kernel/arch/x86_64/smp/smp.c`](../../kernel/arch/x86_64/smp/smp.c), is why the register comparisons of Section 10.2 exist at all. |

**Two of the five say "nothing", and that is the finding.** This sub-task's
characteristic failure is not a crash. It is a machine that boots, prints correct
figures about itself, and is wrong in a way no output distinguishes — which is
why Section 10.2 asserts registers read by the processor being asked about,
rather than counts written by the kernel doing the asking.
### 10.5 Reading the log

```
  Processor 1 is online, local controller identifier 1.
Per-processor areas: 2 processors online.
Application processors: 1 started, 0 did not answer, 0 declined; 2 processor(s) online in all.
  Trampoline at 0x8000, startup vector 0x8, 254 bytes; its identity mapping is removed.
Application processors: asserting what was started.
  every started processor answered a shootdown and invalidated in its own handler.
Application processor self-test passed.
```

Three details in that are worth reading rather than skimming.

`its identity mapping is removed` is the report of a hazard closed, not a
formality: the alternative reads `STILL PRESENT (unexpected)`, and would mean
every stray low pointer in the kernel silently working.

`TLB shootdown: … last address 0x8000` — the last shootdown before the reports is
the removal of that mapping, and the address confirms it.

`Processor 1: acquisitions 1` in the per-processor report is the design of
[`../design/SMP.md`](../design/SMP.md), Section 6, visible in the accounting. The
one lock a parked processor ever takes is the diagnostic channel, once, to
announce that it arrived.

## 11. Verification of the scheduler

**Corresponding sub-task**: 6.15. **Design**:
[`../design/SCHEDULER.md`](../design/SCHEDULER.md), Section 7.

### 11.1 The difficulty this section exists for

**A count of admissions is not evidence that anything ran.** A scheduler that
enqueued four threads and gave none of them a processor produces the same
admissions, the same queue lengths and the same report, and boots to its echo
loop exactly as a correct one does. Every quantity the scheduler prints about
itself is one the scheduler wrote.

What it cannot produce is a counter that moves while nothing in the test writes
it. So the fixture is four kernel threads that do work and record it, and every
assertion is made against what they recorded.

### 11.2 What `make verify` asserts

See [`../design/SCHEDULER.md`](../design/SCHEDULER.md), Section 7, for the table
pairing each assertion with the failure it catches. In outline: a mask naming no
online processor is refused; the timer entry is unmasked, read back from the
entry; every fixture thread completed all its rounds, upon a processor it named
itself, having been given the processor at least once; the slices across the
fixture exceed the number of threads, which is the rotation; and a quantum
expired, which no voluntary yield can demonstrate.

### 11.3 The three failures, and why two of them passed a test

This sub-task is the clearest case in this project so far of a verdict being
worth less than a report.

| What happened | What the run said |
| ------------- | ----------------- |
| `SchedulerInitialise` required a current thread before calibrating, and the bootstrap processor has none at that point in the boot | `Scheduler: the local timer could not be calibrated; nothing will be pre-empted.` and then `Scheduler self-test passed: the kernel says why it schedules nothing.` **The test passed, and was right to.** A machine whose timer cannot be calibrated runs unpre-empted and reports it, and that is a legitimate outcome the test exists to distinguish from silence. It is also, on this machine, entirely wrong — and only the report says so. |
| A newly scheduled thread inherited the scheduler's masked critical section | **The machine hung.** No fault, no panic, and nothing in the log after `Scheduler: asserting the run queues and the rotation.` The counted interrupt-disable belongs to the processor and not to the thread, so a thread that had never run began with the depth and flag of the thread that gave it the processor: interrupts masked for ever, no timer tick, never pre-empted. |
| The scheduler adopted an idle thread for the bootstrap processor | `a child that could not be started was collected as though it had run`, in `verify_lifecycle` — a self-test three hundred lines away and two sub-tasks old. `ThreadStart` succeeds or fails according to whether a thread is current, and that test asserts the failing branch; adopting a thread had quietly turned it into a test of the other branch. |
| The fixture yielded after every round and did no work between them | **Everything passed and nothing was demonstrated.** The threads completed in microseconds, so no two were ever runnable at the same moment: each ran to completion upon a single slice, no queue ever held two, and no quantum ever expired. The fixture now waits at a barrier and then works without yielding. |
| The rotation was asserted as "slices at least rounds" | `a thread completed more rounds than it was given the processor. FAILED.` — four times, and the assertion was the thing that was wrong. A thread that yields into an *empty* queue is not switched away, so it carries on and completes many rounds upon one slice. |

Three of those five produced a passing or absent verdict. That is the argument
for [`../design/SCHEDULER.md`](../design/SCHEDULER.md), Section 7, existing at
all: an assertion is only as good as the state it can actually reach.

### 11.4 Reading the log

```
Scheduler: asserting the run queues and the rotation.
  the fixture ran upon 2 of 2 processor(s), completing 32 rounds upon 32 slice(s).
Scheduler self-test passed.
Scheduler: round-robin, quantum 10 ms, tick vector 0xFB, local timer 62607 counts/ms.
Scheduler: admitted 4, switches 34, pre-empted 19, found idle 49.
  Processor 0: queue 0, timer running, running thread 0.
  Processor 1: queue 0, timer running, running thread 7 (idle).
```

**The report comes after the self-test**, which is the other way round from every
other subsystem here, and the reason is that there is nothing to report until
something has been scheduled. A report before the test would print four zeroes,
which reads exactly like a scheduler that does not work.

`pre-empted 19` is the figure to read. Every other number in that line could be
produced by threads that yielded; only that one requires the timer to have taken
a processor back.

`running thread 0` upon processor 0 is not a defect. The bootstrap processor is
not itself a scheduled thread — it executes `KernelMain` — and the self-test
released the thread it adopted, for the reason the third row of Section 11.3
gives.

---

## 12. Verification of the C library's string and memory functions

**Corresponding sub-task**: 7.1. **Design**:
[`../design/LIBC.md`](../design/LIBC.md), Section 5.

### 12.1 The difficulty this section exists for

**Every function here has a correct implementation and several plausible wrong
ones, and the wrong ones give right answers for the inputs anybody tests with.**
That is the whole difficulty. A string library is the easiest thing in this
repository to write and among the hardest to convince oneself of, because the
obvious test — copy a word, compare two words, measure a length — passes against
implementations that are wrong in three recurring ways.

**The signed byte.** ISO/IEC 9899:2011 requires the comparing and searching
functions to work upon `unsigned char`. Plain `char` is signed upon x86_64 with
this toolchain, so an implementation that used it would agree with a correct one
for every byte below 128 and disagree for every byte above it. Every string of
letters sorts correctly; the first UTF-8 sequence, hash or binary buffer sorts
backwards, and nothing faults.

**The byte just past the end.** A loop bounded by `<=` where it should be `<`
writes one byte too many. In a test whose buffers are adjacent zeroes, that byte
is a zero written into a zero and is invisible.

**The empty case.** A length of zero, an empty string, an empty set, an empty
needle. Each is where the standard says something a natural loop does not do.

### 12.2 What `make verify` asserts

`KernelVerifyString`, in
[`../../kernel/test/libc/string.c`](../../kernel/test/libc/string.c). See
[`../design/LIBC.md`](../design/LIBC.md), Section 5, for the table pairing each
assertion with the failure it catches. In outline: the three comparing functions
and both searching ones are asserted upon `0x80` and `0xFF` rather than upon
letters; every destination is a region inside a buffer filled with the sentinel
`0x5A`, whose margin either side is asserted intact afterwards; `memmove` is
asserted over an overlap in both directions against eight distinguishable bytes;
`strncpy` is asserted to pad and **not** to terminate what it fills; `strchr` and
`strrchr` are asserted to find the terminator and to disagree upon a subject
holding a byte twice; and a finished `strtok` scan is asserted to stay finished.

The sentinel is neither `0x00` nor `0xFF` because both are values these functions
legitimately write — a terminator and a `memset` fill. A sentinel a correct
function may produce is not a sentinel.

### 12.3 The negative tests, and the one that found something

Five defects were inserted and removed. Four behaved as intended and are
tabulated in [`../design/LIBC.md`](../design/LIBC.md), Section 5.1: `memcmp`
comparing through plain `char`, `memcpy` bounded by `<=`, `strncpy` made to
terminate, `memmove` copying forwards in both directions, and `strrchr` keeping
the first match.

**The fifth found a gap, and it was in the test.** The guard at the head of
`strstr` — which returns the haystack when the needle is empty — was deleted, and
every assertion still passed. The search loop already produces the right answer
for an empty needle against a haystack that is not empty; the guard is
load-bearing in exactly one case, an empty needle in an *empty* haystack, and the
self-test had asserted the empty needle only against a subject that was not
empty. So the test asserted a property that could not fail, beside a comment
describing a job the code was not doing. The assertion now covers the empty
haystack and the comment was corrected in the same change.

This is the ordinary yield of the discipline and is recorded because the defect
it found was in the test, which is the class of defect a passing run cannot
report.

### 12.4 What this verification cannot establish

**Nothing here has ever run in a program.** The four translation units are
compiled into the kernel image and called by a boot-time self-test, because
`make verify` is the only thing in this project that can execute anything at all
until this phase produces a userland to host a harness in.

What has therefore never been exercised is the code as a program will use it:
compiled with the flags a user program requires rather than the kernel's —
`-mcmodel=kernel` puts every symbol in the topmost two gibibytes of the address
space, and a program does not live there — and executed at privilege level 3.
Sub-task 7.5 is where that first happens, and it is a genuine second verification
rather than a formality. [`../design/LIBC.md`](../design/LIBC.md), Section 7.

### 12.5 Reading the log

```
String: asserting the C library's string and memory functions.
String self-test passed.
```

**Two lines, and that is the whole of a passing run.** It is the quietest test in
the corpus, and deliberately: there is nothing a correct string function can
report about itself that is worth a line of the boot log. Every line between
those two is a failure, each naming the function and the property that failed —
`strncpy terminated a destination it filled FAILED.` — so a run that fails says
which of the nineteen, and in which of the three ways of Section 12.1.

---

## 13. Verification of the C library's system-call wrappers

**Corresponding sub-task**: 7.2. **Design**:
[`../design/LIBC.md`](../design/LIBC.md), Section 8. **Implementation of the
test**: [`../../kernel/test/libc/wrappers.c`](../../kernel/test/libc/wrappers.c).

### 13.1 The difficulty this section exists for

**The kernel cannot call the thing under test.** `SYSCALL` executes at any
privilege level, but the `SYSRET` that ends the kernel's handling of it returns
to privilege level 3 unconditionally — so a kernel that called `OxysWrite` would
enter its own entry path and leave it as a user program, upon a stack and in an
address space that are not a user program's. Nothing survives that. Since
sub-task 6.7 the only executor of `SYSCALL` in this system is a program, and
[`../design/PRIVILEGE.md`](../design/PRIVILEGE.md), Section 9.4, records the same
fact from the kernel's side.

Every earlier section of this document asserts something the kernel may call.
This one cannot, and the ways out of that are three:

1. **Assert nothing below the instruction**, and say so. It would leave the whole
   substance of the sub-task — the register shift, which is the one thing a
   wrapper is *for* — covered by inspection alone.
2. **Substitute a test double for the invocation.** It would assert that the
   layers above the instruction agree with a fiction this project wrote, which is
   the kind of test that passes for ever and reports nothing.
3. **Run the real thing where it can run.** Which is what is done.

The third is possible only because of how the invocation is written.
[`../../libc/syscall/invoke.asm`](../../libc/syscall/invoke.asm) contains no
memory operand, no relative displacement, no absolute address and no relocation,
so the bytes the assembler emits mean the same thing at every address. The test
copies them out of the kernel image, into a program composed for the purpose, and
runs them at privilege level 3 — so what is asserted is the code this library
ships and not a reconstruction of it. That property is the reason the invocation
is a translation unit of assembly rather than inline assembly inside the C
wrappers; [`../design/LIBC.md`](../design/LIBC.md), Section 8.1.

### 13.2 What `make verify` asserts

**On this side of the instruction**, by ordinary calls:

- A result that is not negative is returned exactly, at zero, at a length and at
  `INT64_MAX`, and `errno` is not touched by any of them.
- Each of the seven failure results produces `-1` and its own `errno`, from a
  previous `errno` that no result maps to.
- A negative result beyond the reserved range, and `INT64_MIN`, both produce
  `ENOSYS` — and no translation ever leaves `errno` at zero.
- Every number `<errno.h>` defines has a message, no two of them share one, and
  `strerror` answers for `-1`, for an unassigned number and for both extremes of
  `int`.
- `strerror(errno)` after a failed translation describes that failure.

**On the far side**, by a program at privilege level 3 that the test composes:

1. `ticks()`, kept in `RBP`.
2. `version(buffer, 64)`, which begins a sum in `RBX`.
3. `write(1, buffer, that length)` — so the system's name appears in the log.
4. `write(1, newline, 1)`.
5. `version(buffer, 8)`, a capacity *smaller* than the string.
6. `write(99, buffer, 1)`, which must fail with `EBADF`.
7. `exit(sum × 1,000,000 + ticks)`.

and then, of what came back: that exactly seven calls reached the dispatcher;
that the program ended and its process is marked ended; that the sum is exactly
what the kernel computes it must be from its own version string; and that the
tick count lies between what this processor observed either side of the run.

**The status carries two numbers on purpose.** The sum is exact and the kernel
knows it; the tick count cannot be exact, nobody being able to say what it was in
between two observations. Multiplying the first by a scale no boot reaches keeps
the uncertainty in the second from absorbing an error in the first — added
together they would be one number with a tolerance, and an off-by-one in the sum
would hide inside it.

### 13.3 The negative tests, and the two that found something

Seven defects were inserted and removed. The table is in
[`../design/LIBC.md`](../design/LIBC.md), Section 8.7, with what each run said.
Five behaved as intended, including a failure result renumbered in the kernel's
interface header, which fails at compile time rather than at boot:
`static assertion failed: "EBADF does not name SYSCALL_EBADF."`

**Two did not, and both were defects in the one file the test was written for.**
The three-argument invocation was made to drop `mov rdx, rcx` — losing the third
argument of every three-argument call — and **every assertion passed**. The lost
argument was a *length*, and the kernel bounds a length rather than refusing an
implausible one: 0x402000 became 4096, the range was readable because a program's
data page is a whole page, and the write emitted the same string it would have
emitted anyway. The only trace was a newline missing from the log, and nothing
was asserting the log. The two-argument invocation passed for the same reason: a
capacity larger than the string is not a capacity the result depends upon.

The assertion is now the sum of *every* result, and step 5 above exists solely so
that one capacity is smaller than the string it is given. Both defects now fail
it.

This is the second time in this phase that the negative-test discipline has
found the defect in the test rather than in the code — Section 12.3 is the first
— and the two are worth reading together. A passing run cannot report a test that
does not test.

### 13.4 What this verification cannot establish

- **The typed wrappers.** `OxysSyscallResult` is asserted by calling it and the
  invocation by running its own bytes; `OxysWrite` passing its `length` where the
  kernel reads a length is checked by nothing. It cannot be until a program is
  linked against this library, the wrappers being compiled `-mcmodel=kernel` and
  holding a reference to `errno` at a kernel address — which is exactly the
  property that makes the invocation copyable and them not. Sub-task 7.5.
- **`R10`.** There is no invocation of four or more arguments, no call needing
  one, and therefore no assertion upon the one register where this kernel's
  convention departs from the C one.
- **`errno` per thread.** ISO/IEC 9899:2011, Section 7.5, requires thread local
  storage duration and there is one object. Nothing can assert the difference
  while there is one thread.

### 13.5 Reading the log

```
Wrappers: asserting the C library's system-call wrappers.
  A program at privilege level 3 reports, through the library's own invocation: Oxys-OS unreleased
Wrapper self-test passed.
```

**The middle line is the point of the whole section.** No part of the kernel
composed it: the name was fetched by one system call and written by another, both
made through the bytes `libc/syscall/invoke.asm` ships, by a program executing at
privilege level 3 in an address space of its own. Every other line between the
first and the last is a failure, each naming the property that failed.

---

## 14. Verification of the C library's heap, and of the break beneath it

**Sub-task**: 7.3. **Design**: [`../design/LIBC.md`](../design/LIBC.md), Section
9, whose Section 9.4 holds the two tables pairing every assertion with the silent
failure it exists to catch. **Test**:
[`../../kernel/test/libc/heap.c`](../../kernel/test/libc/heap.c).

### 14.1 The difficulty this section exists for

The same one Section 13.1 describes, and one more of its own.

**The kernel cannot execute `SYSCALL`.** `brk` is a system call, so every
property of it has to be asserted by something that may make one — a program at
privilege level 3, composed by hand, because there is no compiler to produce one
until sub-task 7.5.

**But the allocator above it is not a system call**, and asserting it through a
program would be asserting it through the one thing that cannot be run. So the
sub-task was built with the seam named:
[`../../libc/include/heap.h`](../../libc/include/heap.h) declares
`OxysHeapExtend`, which is the whole of what the allocator knows about the
machine beneath it, and `OxysHeapAdopt`, by which a caller gives the heap a
region it obtained itself. The kernel's self-test gives it sixty-four kibibytes
and exercises the policy directly. **What runs is the code the library ships**,
not a reconstruction of it and not a copy.

**`OxysHeapAdopt` is not a test hook**, which matters to whether the arrangement
is honest. A program with a statically reserved arena, or one running before a
break exists, has the same need and no other way to meet it; the self-test is
merely its first caller.

### 14.2 What `make verify` asserts

**Of the policy**, by ordinary calls against the adopted region:

- A null region and a region too small for one block are refused, and a refused
  region is not counted.
- An adopted region becomes exactly one free block of exactly its own size.
- Two requests of zero bytes return two different pointers, neither null.
- Every pointer returned is a multiple of sixteen, which is
  `_Alignof(max_align_t)` upon this architecture.
- Three allocations keep three distinct patterns, which is what distinguishes
  disjoint storage from merely distinct addresses.
- **Everything released leaves the heap as it began** — one block, one free
  block, the same available bytes, the same largest request — with the releases
  performed out of order so that both directions of coalescing must work.
- `free(NULL)` is neither a release nor a refusal; a pointer that is not an
  allocation is refused; a block released twice is released once and refused
  once.
- `realloc` grows in place where the next block is free, moves and carries the
  contents where it is not, shrinks without moving, and refuses a pointer that is
  not an allocation with `EINVAL`.
- `calloc` clears a block that was **dirtied and released**, and refuses a count
  and a size whose product would wrap.
- A request of `SIZE_MAX` is refused by the arithmetic, without the heap asking
  the system for anything.
- The census balances, and the heap never asked the system for memory at all.

**Of the break**, by a program at privilege level 3 that the test composes:

1. `brk(0)`, which reports the break. Kept in `RBX`.
2. `version(break, 64)`, which **must fail with `EFAULT`**.
3. `brk(break + 4096)`, whose *difference* from `RBX` is summed.
4. `version(break, 64)`, which now succeeds.
5. `write(1, break, that length)`, which reads it back into the log.
6. `write(1, newline, 1)`.
7. `brk(break)`, giving the page up.
8. `version(break, 64)`, which **must fail with `EFAULT` again**.
9. `brk(0)`, which must report where it began.
10. `exit(sum)`.

and then, of what came back: that exactly ten calls reached the dispatcher; that
the program ended and its process is marked ended; that the sum is exactly what
the kernel computes it must be from its own version string; and — independently
of anything the program said — that the kernel recorded one growth, one shrink,
and no page left mapped by either.

**Steps 2 and 8 are the two that matter most.** Every other assertion here would
pass against a `brk` that reported an address without mapping anything, or that
shrank a number and left the mapping. Those two are the ones that fail.

### 14.3 The negative tests

Fourteen defects were inserted and removed;
[`../design/LIBC.md`](../design/LIBC.md), Section 9.7, holds the table of what
each run said. Twelve were caught. **Two were not**, and both are recorded rather
than explained away:

- **The undo of a failed growth** was removed and nothing reported it. No test
  here can exhaust the frame allocator, which is the only thing that makes a
  growth fail part way; asserting it needs a way to make `FrameAllocate` fail on
  demand, which this kernel has not got. Section 14.4.
- **A redundant size check in `OxysHeapAdopt`** was removed and nothing reported
  it — correctly, because a second check made later rejects strictly more. The
  code was deleted rather than kept, which is the outcome a negative test is
  supposed to be able to produce and rarely does.

### 14.4 What this verification cannot establish

- **The two halves joined.** `OxysBrk`, `OxysSbrk` and `OxysHeapExtend` are
  compiled into this image and cannot be called from it, exactly as Section 13.4
  records of the seven wrappers before them. The first run of the whole path is
  sub-task 7.5.
- **A growth that fails part way.** Section 14.3.
- **A heap page arriving unzeroed.** The composed program has no comparison
  instruction and no branch, so it cannot read a byte and judge it. What *is*
  asserted is the allocator's own clearing, by a `calloc` upon a block that was
  deliberately soiled first.
- **Anything under concurrent use.** There is no locking in the allocator and
  there are no userland threads to need one.

### 14.5 Reading the log

```
Heap: asserting the C library's allocator and the break beneath it.
  The allocator handled 16 allocation(s), 16 release(s), 3 resize(s), 3 refusal(s); 18 split(s) and 18 join(s).
  A program at privilege level 3 reports, from a page it asked the kernel for: Oxys-OS unreleased
Heap self-test passed.
```

**The third line is the point of the whole section**, as the middle line is in
Section 13.5. The bytes it carries were written by the kernel into a page that
did not exist when the program started, at an address the program asked for and
the kernel granted, and were read back out of that page by the program itself.
Every other line between the first and the last is a failure, each naming the
property that failed.

The counts upon the second line are not assertions and are worth reading beside
them: eighteen splits and eighteen joins is a heap that gave back everything it
divided, which is the same fact the final assertion states and is visible without
knowing that it is being asserted.
