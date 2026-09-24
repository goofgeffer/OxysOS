<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# `docs/storage/` — From a Medium to a Caller

One stack, from a sector to a path and an open file. Each layer exists to serve
the one above it, which is why these are grouped apart from
[`../devices/`](../devices/README.md). Read them bottom upwards.

| Document | Subject | Phase |
| -------- | ------- | ----- |
| [`DISK.md`](DISK.md) | The ATA disk by programmed I/O; and how the kernel names storage it cannot reach. | 4.4 |
| [`AHCI.md`](AHCI.md) | The AHCI disk by bus mastering: the handoff, the ports, the command list, one slot. | 4.7 |
| [`SDCARD.md`](SDCARD.md) | The SD card and eMMC: the host controller, the card's command set, the capacity encodings. | 4.8 |
| [`INITRD.md`](INITRD.md) | The initial ramdisk: the `mke2fs` image GRUB loads, its device, and the root it becomes. | 7.7 |
| [`BLOCK.md`](BLOCK.md) | The block layer: registering a device, and what is refused before a driver is reached. | 4.5 |
| [`BUFFER.md`](BUFFER.md) | The buffer cache: lookup, eviction, write-back, invalidation. | 4.6 |
| [`EXT2.md`](EXT2.md) | The EXT2 superblock, group descriptors and inodes; **every limitation of the EXT2 support**. | 5.1–5.3 |
| [`EXT2-FILES.md`](EXT2-FILES.md) | EXT2 directories, paths, reading, writing, truncation and names. | 5.4–5.7 |
| [`EXT2-VERIFICATION.md`](EXT2-VERIFICATION.md) | Checking the EXT2 code against `mke2fs`, `dumpe2fs`, `debugfs` and `e2fsck`. | 5.1–5.8 |
| [`VFS.md`](VFS.md) | The virtual filesystem: mounts, nodes, resolution, open files, pipes. | 5.8, 8.6 |
| [`PERSIST.md`](PERSIST.md) | The persistent `/etc` on a disk labelled `oxys-etc`. | 9 |

Two rules hold throughout:

- **Failures here are silent.** A wrong read returns data. So the self-tests
  assert relationships (the second block of a two-block read is the next block; a
  held block is not read again; a dirty buffer evicted reaches its device), not
  merely success.
- **This is the part of the kernel that can destroy data.** Tests write only to
  devices made of memory; the disk tests write only when a GRUB entry asks, and
  restore what they wrote ([`DISK.md`](DISK.md)).
