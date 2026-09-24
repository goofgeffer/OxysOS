<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# The Initial Ramdisk

**Phase**: sub-task 7.7 of [`../project/PLAN.md`](../project/PLAN.md).
**Source**: the `$(INITRD_IMAGE)` rule of [`../../Makefile`](../../Makefile);
[`../../boot/grub/grub.cfg`](../../boot/grub/grub.cfg); the module tag in
[`../../kernel/handoff/multiboot2.c`](../../kernel/handoff/multiboot2.c) and
[`../../kernel/include/oxys/boot/bootinfo.h`](../../kernel/include/oxys/boot/bootinfo.h);
the reservation in [`../../kernel/mm/pmm.c`](../../kernel/mm/pmm.c); the device
[`../../drivers/ramdisk/ramdisk.c`](../../drivers/ramdisk/ramdisk.c); the mounts
in [`../../kernel/kernel.c`](../../kernel/kernel.c).
**Specifications**: Multiboot2 Specification 2.0, Sections 3.1.11, 3.6.6 and
3.6.8; GNU GRUB manual, `module2`.

An EXT2 image built with the system, loaded into memory by GRUB as a boot module,
presented as the block device `ram0`, and mounted as the root before any disk is
consulted. It is where `/bin`, `/etc` and `/share` come from. From the kernel's
side it is an ordinary volume on an ordinary block device, read by the same code
that reads a disk.

## 1. Contents

| Path | Contents |
| ---- | -------- |
| `/bin` | The programs of `INITRD_UTILITIES` in the `Makefile`: the shell and its utilities ([`../design/SHELL.md`](../design/SHELL.md)), `init` and `shutdown` ([`../design/INIT.md`](../design/INIT.md)), the session, terminal and desktop utilities ([`../design/SESSION.md`](../design/SESSION.md)), and the window demonstration. |
| `/etc` | `system.conf`, `desktop.conf`, `session.conf`, staged from [`../../etc/`](../../etc/) ([`../design/CONFIG.md`](../design/CONFIG.md)). Covered by the persistent volume when one is attached ([`PERSIST.md`](PERSIST.md)). |
| `/share/defaults/etc` | Read-only copies of the same files, never covered. |
| `/share/icons`, `/share/backgrounds` | The launcher icons and the desktop background, converted from [`../../art/`](../../art/). |
| `/mnt` | Empty: where a disk the machine carries is mounted (Section 5). |
| `/lost+found` | Made by `mke2fs`; unused. |

**The check programs are not on it.** `arg-check`, `exec-check` and the others
are a self-test's apparatus, embedded in the kernel image beside the test that
runs each. Shipping them in `/bin` would ship the test harness to someone who
asked for a shell.

The image is 4,096 blocks of 1 KiB, most of it the background photograph. Slack costs a little ISO space; running short
fails the build the day a program grows.

## 2. Why EXT2, made by `mke2fs`

- **Not a format of this project's own.** A private archive format has no
  specification to cite (`PROJECT_GUIDELINES.md`, Section 2), so nobody else can
  find its mistakes. And the kernel already reads EXT2 through an asserted disk
  driver, block layer, cache, filesystem and VFS; the mount is
  `VfsMountVolume(…, "ext2", …)` and nothing more.
- **Not composed by this repository.** A composer written here shares the
  kernel's reading of the format, so a misreading would be written and then
  asserted against itself. An image from e2fsprogs checks the reader against an
  independent implementation at every boot, in every environment
  ([`EXT2-VERIFICATION.md`](EXT2-VERIFICATION.md) makes the same comparison by
  hand).
- **The cost** is a build dependency, recorded in
  [`../project/TOOLCHAIN.md`](../project/TOOLCHAIN.md) and reported by
  `make toolcheck`; the rule names what is missing rather than failing as an
  unknown command.

```sh
SOURCE_DATE_EPOCH=1789257600 mke2fs -q -F -t ext2 -b 1024 -r 1 \
    -U <fixed> -E hash_seed=<fixed> -L oxys-initrd -d build/initrd build/initrd.img 4096
```

| Option | Reason |
| ------ | ------ |
| `-b 1024` | The block size every EXT2 self-test asserts against, and the one the buffer cache holds. |
| `-r 1` | Revision 1, with the feature words the superblock reader checks. |
| No `-I` | `mke2fs` chooses 256-byte inodes, the layout a foreign volume most likely has; the reader takes the size from the superblock. |
| `-d` | Populates from a directory: no loop device, mount or root privilege. |
| `-F` | Never asks; a build that stops for a question hangs in CI. |
| `-U`, `-E hash_seed` | Fix the UUID and directory hash seed, otherwise random. |
| `-L oxys-initrd` | The boot log names the volume mounted. |

The kernel reports `features compatible 0x38, incompatible 0x2, read-only 0x3`,
matching `dumpe2fs -h`. None needs implementing: the compatible word is advisory,
the read-only word binds only a writer that does not understand it, and
`filetype` is understood.

**Reproducibility.** The UUID and hash seed are fixed on every e2fsprogs.
Timestamps honour `SOURCE_DATE_EPOCH` only from e2fsprogs 1.47.1; on 1.47.0, two
builds of the same tree in different seconds differ in 42 bytes, all in time
fields (`cmp -l`), and builds within the same second are identical.

## 3. Loading

`grub.cfg` loads the image in every menu entry:

```
multiboot2 /boot/oxys.elf
module2 /boot/initrd.img initrd
```

A module tag (Multiboot2, Section 3.6.6, type 3) carries the 32-bit physical
start and end and a zero-terminated name.

- **Found by the exact name `initrd`**, never by position, and never by prefix:
  a prefix match accepts `initrd-debug`.
- **Up to four modules are recorded** (`BOOT_MODULE_MAXIMUM`), and any beyond are
  reported, so the set can be searched by name ([`../design/BOOT.md`](../design/BOOT.md)).
- **Refused**: a tag too short to hold its two addresses (the name would be read
  past its end); an extent whose end does not exceed its start (every length is
  a subtraction); a module beyond the recordable number, noted rather than
  overwriting the last.
- **The frames are reserved** by `PhysicalMemoryInitialise`, like the kernel
  image and the boot information: the memory map reports them available
  (Section 3.6.8). Unreserved, the ramdisk would decay under load as the
  allocator reused its frames.
- **The module is not copied.** A copy would come from the heap, making the
  largest mountable ramdisk depend on the heap left at that moment, and would
  save nothing, since the frames are reserved either way.
- **No page alignment is requested** (Section 3.1.11): reads go through the direct
  map at byte granularity, and `FrameMarkRange` reserves every frame a module
  touches.

## 4. The device

`drivers/ramdisk/ramdisk.c` registers the module's extent as `ram0`. A transfer is
a copy: no state, no timeout, no status. Its value is that the EXT2 code, the
buffer cache and the VFS read the root through the same path as a disk, so every
boot exercises them.

- **An image that is not a whole number of blocks is refused.** Rounding down
  would make the last block's tail whatever GRUB placed next. A build always
  produces whole blocks, so the refusal means something else is already wrong.
- **Nothing else is checked here.** The block layer already refuses bad buffers,
  counts and ranges ([`BLOCK.md`](BLOCK.md)); a second copy of a bound is where
  two bounds disagree.
- **It is writable.** A disk is mounted read-only by default because it belongs
  to the machine's owner and a writable mount marks it unclean
  ([`VFS.md`](VFS.md)). A ramdisk was made by this build and vanishes at power
  off, so nothing is protected by refusing writes, and the shell's redirection
  needs a writable root.

## 5. Mounting

- **The root is `ram0`, by name.** `KernelMountRootVolume` does not take the first
  volume found: on a machine with an EXT2 disk, the root would otherwise depend on
  which driver registered first.
- **Without a ramdisk** (a kernel loaded some other way), the root falls back to
  the first volume found, read-only unless the operator asked otherwise. A ramdisk
  present but unmountable is reported and fallen through; the self-test turns it
  into a failure.
- **The persistent `/etc`** is mounted next, if a disk labelled `oxys-etc` exists
  ([`PERSIST.md`](PERSIST.md)).
- **`/mnt`** then receives the first volume on any other device, read-only unless
  asked otherwise. The ramdisk is skipped by identity: mounting it twice presents
  one volume in two places. The write probe of sub-task 5.8
  (`KernelVfsProbeVolume`) takes its mount point as an argument: `/mnt` with a
  ramdisk, `/` without.

## Verification

`KernelVerifyInitrd` in [`../../kernel/test/storage/initrd.c`](../../kernel/test/storage/initrd.c)
runs **after** the root is mounted, not among the self-tests: its subject is the
root the machine booted with. A test that mounted its own ramdisk would show that
one can be mounted, not that this one was.

| Property asserted | The failure it would catch |
| ----------------- | -------------------------- |
| A module named `initrd` exists and `ram0` is registered. | A `module2` line missing from a menu entry; a module found by position. |
| Block count × block size equals the module's extent. | A geometry one block short, wrong only at the end, where the last file ends. |
| The device is writable. | A ramdisk registered under the disk rule, breaking redirection for no stated reason. |
| The root is mounted, on `ram0`. | A disk at the root; every row below would test the wrong filesystem. |
| **Every utility the kernel image also embeds is byte for byte the embedded copy.** | A wrong offset, a mis-followed indirect block, a length rounded to the block, a short transfer: each returns data, so a presence check passes them all. |
| A read one byte past a file's length delivers nothing. | Size and contents disagreeing. |
| `/bin/echo` is read, loaded, run at privilege level 3, and exits 0. | A chain from module to loader delivering something that is not the program. |
| A file created on the root reads back, is removed, and is gone. | A read-only root, or writes that do not reach the medium. |
| No program run leaves a file open. | A descriptor leaked per command. |

The byte comparison is the one that matters: the `Makefile` puts the same file,
`build/user/<name>.embed.elf`, into the kernel image and onto the ramdisk, so any
difference at boot was introduced by the path between them.

Not asserted: what `echo` printed (nothing captures its output), and endurance
under heavy writing.

A person sees, in the boot log:

```
Boot modules: 1.
  0x6F7000 - 0xAF7000  4096 KiB  initrd
Ramdisk: ram0 is the module initrd at 0x6F7000, 4096 KiB in 8192 blocks of 512 bytes, writable.
EXT2 volume upon ram0: revision 1.0, labelled oxys-initrd, writable.
VFS: the initial ramdisk is mounted at the root.
```

## Limitations

1. The ramdisk stays the root; there is no pivot to a disk volume. That needs a
   way to move a mount and a rule for which volume is the system's.
2. Its contents are fixed at build time; `/etc` escapes this through
   [`PERSIST.md`](PERSIST.md).
3. Its frames are never freed; releasing them would mean unmounting the root.
4. Not bit-reproducible on e2fsprogs before 1.47.1 (Section 2).
5. Only the module named `initrd` is used; `BootInformationFindModule` would find
   another, but nothing asks.
