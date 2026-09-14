<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# The Initial Ramdisk

**Phase**: 7, sub-task 7.7, of [`../project/PLAN.md`](../project/PLAN.md).

**Authority**: `PROJECT_GUIDELINES.md`, Sections 2, 3 and 6. Every assertion
about the boot protocol below carries a citation, and the specification is
registered in [`../project/REFERENCES.md`](../project/REFERENCES.md).

**Implementation**: the image is built by the `$(INITRD_IMAGE)` rule of the
[`../../Makefile`](../../Makefile); the boot loader is told to load it by
[`../../boot/grub/grub.cfg`](../../boot/grub/grub.cfg); the module tag is parsed
by [`../../kernel/handoff/multiboot2.c`](../../kernel/handoff/multiboot2.c) into
the `BootModule` of
[`../../kernel/include/oxys/boot/bootinfo.h`](../../kernel/include/oxys/boot/bootinfo.h);
its frames are reserved by
[`../../kernel/mm/pmm.c`](../../kernel/mm/pmm.c); it is presented as a block
device by [`../../drivers/ramdisk/ramdisk.c`](../../drivers/ramdisk/ramdisk.c)
behind
[`../../kernel/include/oxys/dev/storage/ramdisk.h`](../../kernel/include/oxys/dev/storage/ramdisk.h);
it is mounted by `KernelMountRootVolume` in
[`../../kernel/kernel.c`](../../kernel/kernel.c); and it is asserted by
[`../../kernel/test/storage/initrd.c`](../../kernel/test/storage/initrd.c).

## 1. The problem this solves

By the end of sub-task 7.6 this project could build a program, load it into an
address space, enter it at privilege level 3, give it an argument vector, and let
it reach a filesystem through fourteen system calls. It could do none of that
upon a machine somebody actually owns.

Every program this kernel had ever run came from one of two places: it was
embedded in the kernel image by `incbin`, or it was written onto a volume the
kernel had composed in an array for the purpose. Both are self-tests' apparatus.
Neither is a filesystem, and neither survives the moment a person boots the ISO
and expects to type a command — which is what sub-task 8.1 begins.

**An initial ramdisk is the smallest thing that closes the gap.** It is a
filesystem image carried in the boot medium, placed in memory by the boot loader,
and mounted as the root before any disk is consulted. From the kernel's side it
is an ordinary volume upon an ordinary block device; from the build's side it is
a file produced beside the kernel image; and from a program's side `/bin/ls` is
simply there.

## 2. What is upon it

Five files and two directories.

| Path | What it is |
| ---- | ---------- |
| `/bin/echo` | The utility of sub-task 7.6. |
| `/bin/cat` | The same. |
| `/bin/ls` | The same. |
| `/bin/mkdir` | The same. |
| `/bin/rm` | The same. |
| `/mnt` | Empty. Where a volume the machine carries is mounted; Section 6.3. |
| `/lost+found` | `mke2fs` makes it. Nothing here uses it. |

**The four check programs of sub-task 7.6 are deliberately not here.**
`arg-check`, `exec-check`, `file-check` and `startup-check` are a test's
apparatus: each exists to make a machine-readable statement about a system call,
and each is embedded in the kernel image beside the self-test that runs it. A
system that shipped them in `/bin` would be shipping its own test harness to
somebody who asked for a shell.

The image is 2 MiB and holds about 155 KiB. The slack costs a few tens of
kibibytes of ISO; being short costs a build that fails on the day a utility
grows.

## 3. Why the image is EXT2, and why `mke2fs` makes it

### 3.1 Why not a format of this project's own

The obvious alternative is an archive format defined here — a header, a name, a
length, the bytes, repeated — with a small read-only filesystem driver in the
kernel to walk it. It is perhaps four hundred lines and it would work.

It is refused for two reasons, and the second is the one that decides it.

**It has no specification.** `PROJECT_GUIDELINES.md`, Section 2, requires that
the authoritative specification of a subsystem be retrieved and cited before the
subsystem is implemented. A format invented in this repository has no authority
to cite, and the rule is not a formality: it exists because a format nobody else
has written against is a format whose mistakes nobody else can find.

**This kernel already reads a filesystem.** Phases 4 and 5 built a disk driver, a
block layer, a buffer cache, an EXT2 implementation and a virtual filesystem
layer, and every one of them is asserted. A second filesystem would be a second
thing to get right in order to reach a capability the first one already has.

So the ramdisk is an EXT2 volume, and the whole of sub-task 7.7 above the block
layer is *nothing at all* — the mount is `VfsMountVolume(…, "ext2", …)`, and the
code that reads `/bin/ls` off it is the code that reads a file off a disk.

### 3.2 Why `mke2fs` rather than a composer in this repository

This project possesses a complete understanding of the EXT2 format: it is the
whole of `kernel/fs/ext2/`, and `kernel/test/volume.c` already composes a volume
byte by byte. Writing a composer for the ramdisk would have been easy.

It would also have been worth very little. The note at the head of
[`../../kernel/test/volume.h`](../../kernel/test/volume.h) says why, of the
composed fixture: *it shares this kernel's understanding of the format, so a
misreading of the specification would be composed into it and asserted against
itself.* A ramdisk composed here and read here would prove the reader consistent
with the composer. A ramdisk produced by e2fsprogs proves the reader consistent
with an implementation that has never seen this one.

[`EXT2-VERIFICATION.md`](EXT2-VERIFICATION.md), Section 6, already made that
comparison by hand, three sub-tasks ago, upon images somebody had to remember to
build. **This makes it happen at every boot, in every environment, on every
machine**: the volume the kernel mounts as its root was written by e2fsprogs, and
the superblock, the group descriptor, the root inode, the directory entries and
the file blocks are all read from it before the banner is printed. It is the same
argument the `clang-check` target rests upon and the same one that made the
sixty-six `snprintf` conversions of sub-task 7.4 worth checking against the
host's C library.

### 3.3 What it costs

`mke2fs` becomes a build dependency — the first this project has that is not a
compiler, an assembler, a linker or an image builder. It is recorded in
[`../project/TOOLCHAIN.md`](../project/TOOLCHAIN.md) and reported by `make
toolcheck`, and the `$(INITRD_IMAGE)` rule says plainly what is missing and where
it comes from rather than failing as a command that is not found.

`PROJECT_GUIDELINES.md`, Section 3, names five tools that must be installed and
functional. That statement remains true: all five still must be. It is not
amended, Section 7 of that document reserving amendments to the project owner.

### 3.4 The options, and why each is there

```sh
SOURCE_DATE_EPOCH=1789257600 mke2fs -q -F -t ext2 -b 1024 -r 1 \
    -U <fixed> -E hash_seed=<fixed> -L oxys-initrd -d build/initrd build/initrd.img 2048
```

| Option | Why |
| ------ | --- |
| `-b 1024` | The block size the EXT2 implementation is written against, and the only one the buffer cache of sub-task 4.6 holds. A 4096-byte volume is readable — [`EXT2-VERIFICATION.md`](EXT2-VERIFICATION.md), Section 6, records one being read — but 1024 is the size every self-test asserts against. |
| `-r 1` | Revision 1, which supplies the three feature words the superblock reader of sub-task 5.1 refuses an unknown bit in. |
| *(no `-I`)* | The inode size is left to `mke2fs`, which chooses 256. The reader takes it from the superblock, and a volume somebody else made is far likelier to be 256 than 128, so that is the layout worth exercising. |
| `-d` | Populate from a directory. This is what makes the whole arrangement possible without privilege: no loop device, no mount, no root. |
| `-F` | Do not ask. The target is a regular file and the answer is always yes; a build that stops for a question is a build that hangs in continuous integration. |
| `-U`, `-E hash_seed` | Fix the two values `mke2fs` would otherwise draw at random. Section 3.5. |
| `-L oxys-initrd` | A label, so that a boot log says which volume was mounted rather than that one was. |

The kernel reads the result and reports `features compatible 0x38, incompatible
0x2, read-only 0x3` — `ext_attr resize_inode dir_index`, `filetype`, and
`sparse_super large_file` — which is exactly what `dumpe2fs -h` says of the same
image, and none of which this kernel implements or needs to: the first word is
advisory, the third is only binding upon a writer, and `filetype` is understood.

### 3.5 Reproducibility, stated exactly

`mke2fs` draws three things from outside the source: the volume UUID, the seed of
the directory hash, and the timestamps. The first two are fixed by `-U` and
`-E hash_seed` upon every version. The third is fixed by `SOURCE_DATE_EPOCH`,
**which e2fsprogs honours only from version 1.47.1**.

The version upon the machine this was built on is 1.47.0, so it does not, and the
position is stated rather than assumed: two builds of an unchanged tree made in
different seconds differ in forty-two bytes, every one of them within a time
field, and two builds made within the same second are identical to the byte.
`cmp -l` is what says so.

The variable is set regardless. It costs nothing, and the build becomes
reproducible upon a host whose e2fsprogs is new enough without anybody editing
the `Makefile`.

## 4. How it reaches memory

The boot loader loads it. Multiboot2 Specification 2.0, Section 3.6.6, defines
the module tag: type 3, carrying after the common `type` and `size` fields the
32-bit physical start and end addresses of one module and then a zero-terminated
string. One tag appears per module and the type may appear any number of times.

`boot/grub/grub.cfg` asks for it with GNU GRUB's `module2` command, in every menu
entry:

```
multiboot2 /boot/oxys.elf
module2 /boot/initrd.img initrd
```

### 4.1 The name, not the position

The kernel finds the ramdisk by the string `initrd` and never by being the first
module loaded. A kernel that took the first module would find the right thing
today and the wrong thing on the day a second module is added — silently, because
a module is a range of bytes and every range of bytes looks alike.

The comparison is exact and not by prefix, for the reason the block layer's names
are exact: a prefix match accepts `initrd-debug` where `initrd` was asked for, and
the kernel then mounts something nobody intended without a word being said.

### 4.2 Four modules, not one

`BootInformation` records up to `BOOT_MODULE_MAXIMUM` modules and notes when the
boot loader supplied more than that. Recording one would have made the *set* of
modules a thing the kernel could not represent, and a kernel that cannot
represent a set cannot select from it by name.

### 4.3 What is refused

A module tag shorter than its own two addresses is refused, because the string
would then be read from beyond the tag. An extent whose end does not exceed its
start is refused, because every caller computes a length by subtraction and would
obtain zero or a length that has wrapped. And a module beyond the number that can
be recorded is refused with the truncation noted, rather than overwriting the last
one recorded — which would leave the kernel holding a set it could not tell was
incomplete.

### 4.4 The frames are reserved, and the module is not copied

Multiboot2, Section 3.6.8, warns that the memory map "includes the regions
occupied by kernel, mbi, segments and modules", and that the kernel must take
care not to overwrite them. `PhysicalMemoryInitialise` therefore reserves each
module's extent, exactly as it already reserved the kernel image, the boot
information structure and the frame bitmap.

A kernel that omitted this would boot, would mount the ramdisk, and would then
read from it whatever the frame allocator had since put there. That is not a
filesystem that fails; it is a filesystem that decays under load, and by the time
anything notices, the evidence is gone.

**The module is not copied anywhere.** The obvious implementation copies it into
memory the kernel owns and releases the module's frames. It is not done, for two
reasons. The copy would come from the kernel heap, which would make the largest
ramdisk this kernel can mount a function of how much heap remains at the moment
the device is registered — a limit nobody can predict and nothing states. And it
buys nothing: the frames are reserved either way, so freeing them would mean
releasing them deliberately, which is a thing to get wrong rather than a saving.

### 4.5 No page alignment is asked for, and that is a decision

Multiboot2, Section 3.1.11, defines a header tag by which an image may require
its modules to be page aligned. This kernel does not carry it.

It would do nothing. The ramdisk is read through the direct map at byte
granularity, so alignment cannot affect a transfer; and `FrameMarkRange` reserves
every frame a range touches *in its entirety*, so a module sharing its first or
last frame with something else costs that frame and nothing is ever issued from
beneath a module. The tag would be eight bytes that change no behaviour, and this
project has removed a linker flag for precisely that reason — see the note upon
`USER_LDFLAGS` in the `Makefile`, where `-z max-page-size=0x1000` was measured to
change the output by not one byte and was deleted. A flag that does nothing is a
flag somebody will one day reason from.

## 5. The device beneath it

`drivers/ramdisk/ramdisk.c` registers the module's extent with the block layer as
`ram0`, of `BLOCK_SIZE_DEFAULT` blocks.

It is the only storage driver in this project that converses with nothing. A
transfer is a copy; it cannot fail for any reason outside the file; there is no
state to reset, no command to time out and no status register to read. Almost the
whole of what the ATA and AHCI drivers are is absent, and what remains is the
arithmetic that turns a block number into an address.

**That is exactly why it belongs behind the block layer.** The value of the
arrangement is not that a ramdisk needs a driver. It is that the EXT2
implementation of Phase 5, the buffer cache of sub-task 4.6 and the filesystem
layer of 5.8 read the initial ramdisk through the path they read a disk through,
and are therefore exercised upon every boot rather than only upon a machine that
happens to carry a volume.

Two things it refuses:

**A module that is not a whole number of blocks.** Rounding down would produce a
device whose last block is the image's last block with something else after it,
and the something else is whatever the boot loader placed next. A filesystem
would read it as data and would be right to. An image this project builds is
always a whole number of blocks, so the refusal fires only when something else has
already gone wrong — which is worth saying plainly.

**Nothing else.** The block layer has already refused a null buffer, a count of
zero, a range outside the device and a write to a read-only device
([`BLOCK.md`](BLOCK.md), Section 3). Repeating those tests here would be the
second of two places a bound is written, which is the arrangement in which the
two disagree and the weaker one wins.

### 5.1 It is registered writable

Every volume upon a disk is mounted read-only unless the operator asked otherwise
at the GRUB menu, and the reason is recorded in [`VFS.md`](VFS.md), Section 8: the
disk belongs to whoever owns the machine, and a kernel that mounted it for writing
would mark it as not cleanly unmounted merely by having been booted — so their
disk would demand a check before they could mount it again.

**None of that reasoning reaches a ramdisk.** It was made by this build, it is
read by nothing else, and it ceases to exist when the machine is switched off.
There is nothing to protect and nobody to inconvenience, so the device says what
is true of it and the mount takes it at its word. A root nothing may write to is
also a root the shell's output redirection at sub-task 8.5 cannot redirect into.

## 6. The root

### 6.1 It is chosen by name

`VfsMountRoot` walks the registered devices and mounts the first volume it can.
That is right when the question is "is there anything to mount" and wrong when
the answer must be a particular thing, so `KernelMountRootVolume` names `ram0`
directly.

A machine carrying an EXT2 volume upon a disk would otherwise boot with a
stranger's filesystem at the root or with the ramdisk there according to which
driver had registered first — a difference nobody chose, that changes every path
in the system, and that a boot log does not obviously show.

### 6.2 The fall-back

A kernel booted without a ramdisk falls back to the rule that governed this
function before sub-task 7.7: the first volume any device carries, read-only
unless the operator asked otherwise. Every ISO this project builds carries a
ramdisk, so the fall-back exists for a kernel loaded by some other means; `make
verify` exercises the ramdisk path and nothing presently exercises this one.

A ramdisk that is present and will not mount is reported and fallen through from
rather than treated as fatal. The self-test of Section 7 is what turns it into a
failure; stopping here as well would deny a machine with a real volume the root
it could still have had.

### 6.3 `/mnt`, and the capability that would otherwise have vanished

Before sub-task 7.7 the root was whatever volume the machine carried, and the
write probe of sub-task 5.8 reached that volume by resolving a path from the root.
The ramdisk takes the root. Without somewhere else to put it, a machine with a
disk would have lost that probe entirely.

That is the worst shape a regression can take. What it leaves behind is not a
test that fails but a diagnostic that no longer prints, and the two are
indistinguishable to anyone who was not looking for it.

So `/mnt` exists upon the ramdisk, and `KernelMountMachineVolume` mounts the first
volume any other device carries there — read-only unless the operator asked
otherwise, by the rule of Section 5.1, which still governs a disk. The ramdisk is
skipped by identity: mounting it a second time would succeed, would present the
same volume twice, and would be noticed only when something wrote through one view
and read through the other.

`KernelVfsProbeVolume` accordingly takes the mount point and the path as
arguments, where it had both written into it. It is `/mnt` where there is a
ramdisk and `/` where there is not.

## 7. Verification

`KernelVerifyInitrd` runs **after** `KernelMountRootVolume` and not among the
self-tests, which is the whole shape of it. Every other assertion in this project
composes its subject; this one's subject is the root the machine actually booted
with, and a test that mounted a ramdisk for itself would establish that a ramdisk
*can* be mounted and say nothing whatever about whether this kernel did.

| What is asserted | The silent failure it catches |
| ---------------- | ----------------------------- |
| A module named `initrd` was supplied, and `ram0` is registered. | A `module2` line dropped from a menu entry, or a module found by position and misidentified. |
| The device's block count times its block size is the module's extent. | A geometry off by a block, which reads correctly everywhere except at the end of the volume — which is where the last file's last block is. |
| The device is not read-only. | A ramdisk registered under the rule that governs somebody else's disk, which Phase 8's redirection would then fail upon for a reason nothing states. |
| Something is mounted at the root, and it is mounted upon that device. | A root that is the machine's own disk, reached because the ramdisk was preferred by accident rather than by name. Every assertion below would then be about the wrong filesystem. |
| **Each of the five utilities is byte for byte the copy embedded in the kernel image.** | A block read from the wrong offset, an indirect block followed wrongly, a length rounded up to the block, a transfer that reported a count it did not deliver. Every one of those returns *data*, and a test that opened the files and found them present passes upon all of them. |
| A read of one byte beyond the declared length delivers nothing. | A file whose size and whose contents disagree, which a reader that asks only for the size cannot see. |
| `/bin/echo` is read from the root, loaded, entered at privilege level 3, and ends with a status of zero. | A chain from module to block device to filesystem to loader that delivers something which is not an executable — and, since `_start` is at the beginning of a program, a last page that is wrong would still start. |
| A file is created upon the root, read back identically, removed, and is then absent. | A root mounted read-only, or one whose writes do not reach the medium beneath it. |
| No program run from the ramdisk leaves an open file behind it. | A descriptor leaked per execution, which costs the machine a table entry per command in Phase 8. |

**The comparison against the embedded copy is the assertion that matters.** The
`Makefile` copies one file, `build/user/<name>.embed.elf`, into the kernel image
and onto the ramdisk, so the bytes are known to be identical at build time. Any
difference observed at boot was introduced by the path between them: the module's
extent, the direct map, the block device's arithmetic, the buffer cache, the
inode's direct and indirect block pointers, the file's length. That is a stronger
statement than any of the individual layers can make about itself.

### 7.1 What it cannot assert

It cannot assert what `echo` printed. Nothing in this kernel captures the
diagnostic path, so the line the program writes is evidence for a person reading
the serial log and not for a machine — which is the limitation
[`../design/LIBC.md`](../design/LIBC.md), Section 12.7, records of the whole of
sub-task 7.6.

It cannot assert that the ramdisk survives being written a great deal. The write
it makes is one small file, created and removed; a ramdisk exhausted by a shell
that wrote to it for an hour would satisfy every assertion here.

## 8. Limitations

1. **The ramdisk is the root and nothing replaces it.** A real system pivots:
   the initial ramdisk brings up enough of the machine to find the real volume,
   and the root is then exchanged for that volume. This kernel mounts the
   machine's volume at `/mnt` and leaves the ramdisk where it is. The exchange
   needs a working directory, a way to move a mount, and something that decides
   which volume is the system's — none of which exists, and the first of them is
   sub-task 8.3's.

2. **Its contents are fixed at build time.** There is no way to add a program to
   the ramdisk without rebuilding the ISO. That is what a ramdisk is, and it
   stops being a limitation when Phase 8 can write to a volume that persists.

3. **Nothing frees it.** The module's frames are reserved for the life of the
   machine. Two mebibytes of a 512-mebibyte machine is not worth the code that
   would release them, and releasing them would mean unmounting the root.

4. **It is not bit-reproducible upon e2fsprogs before 1.47.1.** Section 3.5
   states exactly what differs and why.

5. **A second module cannot be reached by anything.** The kernel records up to
   four and uses one. Nothing else asks for a module, so nothing else is
   offered a way to find one; the machinery to do so is `BootInformationFindModule`
   and is one call.

6. **The write probe of sub-task 5.8 now acts upon `/mnt`**, and a machine whose
   volume the ramdisk displaced is reached one directory deeper than it was.
   Section 6.3 records why, and the probe names the point it acted upon in its
   own report so that a reader is never guessing which volume was written.

## 9. What a person sees

```
Boot modules: 1.
  0x3E7000 - 0x5E7000  2048 KiB  initrd
...
Ramdisk: ram0 is the module initrd at 0x3E7000, 2048 KiB in 4096 blocks of 512 bytes, writable.
...
EXT2 volume upon ram0: revision 1.0, labelled oxys-initrd, writable.
EXT2 volume: 2048 blocks of 1024 bytes (2048 KiB), 1894 free; 256 inodes of 256 bytes, 239 free.
...
VFS: the initial ramdisk is mounted at the root.
VFS: 1 mount(s).
  / <- ram0 (ext2, block 1024, writable)
VFS: contents of /:
  2 directory .
  2 directory ..
  11 directory lost+found
  12 directory bin
  18 directory mnt
VFS: 5 entries.
Initial ramdisk: the module of sub-task 7.7, the device beneath it, and the root it is mounted as.
the root filesystem is the initial ramdisk.
Initial ramdisk self-test passed: 5 utilities stand in /bin byte for byte as they
were built, one of them ran from there at privilege level 3, and the root took a write.
```

The second-to-last line is `/bin/echo` speaking. It is the first line in this
project's boot log written by a program that was read off a filesystem.
