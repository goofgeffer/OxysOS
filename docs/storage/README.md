<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# `docs/storage/` — From a Medium to a Caller

Ten documents describing one stack, bottom upwards. They are grouped apart from
[`../devices/`](../devices/) because each exists to serve the one above it, and
because the whole stack is what the filesystem of Phase 5 is written against. The
stack is complete as of sub-task 5.8: a sector at the bottom, a path and an open
file at the top. Three of the ten are the EXT2 volume, which
[`EXT2.md`](EXT2.md) heads.

The tenth is [`INITRD.md`](INITRD.md), and it belongs here rather than beside the
boot documents for the reason everything else here does: its subject is a medium
and a volume upon it. It is the first thing in this stack whose medium is not
hardware at all, and the first that puts the whole stack to work at every boot
rather than only upon a machine that happens to carry a disk.

| Document | Subject | Implementation | Phase |
| -------- | ------- | -------------- | ----- |
| [`DISK.md`](DISK.md) | The ATA disk in programmed input/output mode: the task file, the two addressing modes, where a channel actually answers, the identification of what answered, the cache flush that makes a write durable, and what storage the driver cannot reach and how it says so. | [`../../drivers/ata/`](../../drivers/ata/) | 4.4 |
| [`AHCI.md`](AHCI.md) | The AHCI disk by first-party direct memory access: the handoff from the firmware, the ports an adaptor implements, the command list and the region descriptors that name the caller's own pages to the device, and the single command slot this driver keeps. | [`../../drivers/ahci/ahci.c`](../../drivers/ahci/ahci.c) | 4.7 |
| [`SDCARD.md`](SDCARD.md) | The SD card and the embedded MultiMediaCard: the host controller upon the bus, the second command set of the card behind it, the two encodings of a card's capacity, and the transfer through the buffer data port. | [`../../drivers/sdhci/sdhci.c`](../../drivers/sdhci/sdhci.c) | 4.8 |
| [`INITRD.md`](INITRD.md) | The initial ramdisk: the EXT2 image `mke2fs` builds beside the kernel, the Multiboot2 module the boot loader carries it in, the frames that are reserved for it, the device it is presented as, and the root it is mounted as — so that `/bin/ls` exists upon a machine this project composed no volume for. | [`../../drivers/ramdisk/ramdisk.c`](../../drivers/ramdisk/ramdisk.c) | 7.7 |
| [`BLOCK.md`](BLOCK.md) | The generic block-device layer: what a driver supplies to register a device, and what the layer refuses before any driver is reached. | [`../../kernel/block/block.c`](../../kernel/block/block.c) | 4.5 |
| [`BUFFER.md`](BUFFER.md) | The buffer cache: how a block is found, what is discarded when the store is full, and when a modified block reaches its device. | [`../../kernel/block/buffer.c`](../../kernel/block/buffer.c) | 4.6 |
| [`EXT2.md`](EXT2.md) | The EXT2 volume and its structures: the superblock, the block group descriptor table, and the inode with the direct and indirect pointers that name a file's blocks. **Section 10 enumerates every limitation of this kernel's EXT2 support**, the two documents below included. | [`../../kernel/fs/ext2/superblock.c`](../../kernel/fs/ext2/superblock.c), [`../../kernel/fs/ext2/group.c`](../../kernel/fs/ext2/group.c), [`../../kernel/fs/ext2/inode.c`](../../kernel/fs/ext2/inode.c) | 5.1 to 5.3 |
| [`EXT2-FILES.md`](EXT2-FILES.md) | What is done with those structures: the directory entries that turn a name into an inode number, the resolution of a path, the reading of a file, the allocation, writing and truncation that alter a volume, and the insertion and removal of the names by which a file is reached. | [`../../kernel/fs/ext2/directory.c`](../../kernel/fs/ext2/directory.c), [`../../kernel/fs/ext2/path.c`](../../kernel/fs/ext2/path.c), [`../../kernel/fs/ext2/file.c`](../../kernel/fs/ext2/file.c), [`../../kernel/fs/ext2/alloc.c`](../../kernel/fs/ext2/alloc.c), [`../../kernel/fs/ext2/name.c`](../../kernel/fs/ext2/name.c) | 5.4 to 5.7 |
| [`EXT2-VERIFICATION.md`](EXT2-VERIFICATION.md) | The eleven self-tests of the EXT2 implementation, and the six that assert against a volume `mke2fs` produced rather than one this kernel composed — which is the only thing that catches an assumption shared between the kernel and its own fixture. | [`../../kernel/test/storage/ext2/`](../../kernel/test/storage/ext2/) | 5.1 to 5.7 |
| [`VFS.md`](VFS.md) | The virtual filesystem layer: the operations a filesystem supplies in order to be mounted, the mount table that joins several volumes into one tree, the node cache that gives one file one identity however many callers reach it, the resolution of a path across mount points and through symbolic links, the open file with a position that advances, and the mark a mount leaves upon a volume it has open. | [`../../kernel/fs/vfs/`](../../kernel/fs/vfs/), [`../../kernel/fs/ext2_vfs.c`](../../kernel/fs/ext2_vfs.c) | 5.8 |
| [`PERSIST.md`](PERSIST.md) | The persistent `/etc`, of 2026-09-23: a volume labelled `oxys-etc` upon a disk of its own, found by its whole label, mounted over the ramdisk's `/etc` and seeded without overwriting, marked clean when a stopped machine left it open, written back when a file upon it is closed, and released at the power call. |

## The two rules that run through all of them

**A failure here is silent.** A disk driver that reads the wrong sector returns
data, and data that arrived cannot be distinguished from data that is correct
until something interprets it. Every self-test in this stack asserts a
*relationship* — that the second block of a two-block read is the block that
follows, that a block already held is not read again, that a dirty buffer
evicted under pressure reached its device — rather than merely that an operation
succeeded.

**This is the part of the kernel that can destroy something.** The disk
self-test reads unconditionally and writes only when the operator has asked for
it at the GRUB menu, restoring what it wrote; the block and buffer layers are
asserted against a device made of memory, so that the verification needs no disk
and touches nobody's data. See [`DISK.md`](DISK.md), Section 6.
