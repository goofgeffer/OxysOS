# Testing: the Devices, the Storage Stack and the Kernel

**Authority**: `PROJECT_GUIDELINES.md`, Section 2, the testing mandate.

**What is here**: the verification of everything but the graphical work — the
text-mode display, the serial receive path, the backspace that crosses a row
boundary, the disk, the EXT2 superblock, the virtual filesystem layer, the
privilege apparatus, the interrupt controllers and the concurrency primitives —
each with the procedure, what it establishes, and the negative test that
confirmed the assertion was worth making.

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
| Name a destination other than this processor's local APIC identifier | `A claimed line is directed at another processor.` four times, then `The interval timer stopped when the I/O APIC took over its request line.` This is what sub-task 6.14's characteristic failure will look like when it arrives. |
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

That is not a reason to defer the testing to sub-task 6.14. It is a reason to
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
   4.1, placed the sub-task before 6.14 upon.
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

Six lines say the whole of it:

```
Per-processor area self-test passed.
Spinlock self-test passed.
Interprocessor interrupt self-test passed.
  The stale translation was observable before the shootdown.
TLB shootdown self-test passed.
Per-processor areas: 1 processor online.
```

The fourth is reported rather than asserted. The architecture nowhere requires a
processor to cache a translation it has used, so an equality there would mean the
test proved less than it hoped rather than that anything was wrong. Under QEMU it
is observable, and the run says so — which is what makes the shootdown assertion
above it a demonstration and not a tautology.

`ShootdownReport` prints `serviced 3` against `requests 1`, and that is correct:
`KernelVerifyIpi` sends the shootdown vector twice on its own account, to
establish the delivery and the end-of-interrupt, without publishing a request.

