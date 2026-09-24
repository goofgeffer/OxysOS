<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# EXT2: Checking Against e2fsprogs

**Phase**: sub-tasks 5.1 to 5.8 of [`../project/PLAN.md`](../project/PLAN.md).
**Source**: the probe in `KernelVfsProbeVolume`,
[`../../kernel/kernel.c`](../../kernel/kernel.c), and
[`../../kernel/test/storage/ext2/`](../../kernel/test/storage/ext2/).
**Specifications**: as [`EXT2.md`](EXT2.md); the e2fsprogs tools `mke2fs`,
`dumpe2fs`, `debugfs` and `e2fsck`.

How this kernel's EXT2 code is checked against an implementation it did not
write. The self-tests of [`EXT2.md`](EXT2.md) and [`EXT2-FILES.md`](EXT2-FILES.md)
compose their own volumes, so they share the kernel's reading of the format: a
misreading would be composed into the fixture and asserted against itself. A
volume made by `mke2fs`, and a volume this kernel wrote judged by `e2fsck`, are
the only checks that catch such an assumption.

The initial ramdisk makes the reading half of this automatic: it is made by
`mke2fs` and read at every boot ([`INITRD.md`](INITRD.md)). The procedures below
cover what the ramdisk does not: other block sizes, deep indirection, and writes.

## 1. What the kernel reports

At every boot, for each volume found, the kernel reports the superblock and the
first group, lists the root directory, and probes one path: a symbolic link
reports its target and form; a regular file, its first sixteen bytes. The path is
resolved **without** following a final link. The volume on a disk is mounted at
`/mnt` ([`VFS.md`](VFS.md)).

```
EXT2 volume upon ata0: revision 1.0, labelled oxys-root, writable.
EXT2 volume: 16384 blocks of 1024 bytes (16384 KiB), 15211 free; 4096 inodes of 256 bytes, 4085 free.
EXT2 volume: 2 groups of 8192 blocks and 2048 inodes, first data block 1, first usable inode 11.
EXT2 volume: features compatible 0x38, incompatible 0x2, read-only 0x3, state clean.
EXT2 group 0: block bitmap at 66, inode bitmap at 67, inode table at 68; 7599 free blocks, 2037 free inodes, 2 directories.
EXT2 inode 2: mode 0x41ED (directory), 40960 bytes, 3 links, 82 sectors, first block 580.
EXT2 root blocks: 580 616 640 664 688 712 736 760 784 808 832 856 881 ...
EXT2 directory 2 holds 46 entries.
EXT2 path /lost+found resolves to inode 11, directory of 12288 bytes.
```

## 2. Reading a volume the kernel did not make

Make an image, attach it to a board with an IDE disk, and compare the log with
`dumpe2fs` and `debugfs` on the host.

```sh
mke2fs -q -t ext2 -b 1024 -L oxys-root -F ext2.img 16384
qemu-system-x86_64 -machine pc -cpu qemu64 -smp cores=2 -m 512M \
    -cdrom build/oxys.iso \
    -drive file=ext2.img,format=raw,if=ide,index=0,media=disk \
    -display none -serial file:ext2.log
dumpe2fs -h ext2.img
debugfs -R "stat <2>" ext2.img
debugfs -R "ls -l /" ext2.img
```

| Compare | Against | What agreement shows |
| ------- | ------- | -------------------- |
| The volume lines | `dumpe2fs -h`: counts, free counts, group geometry, inode size, the three feature words | Every superblock offset. |
| The group line | `dumpe2fs`, group 0 | The descriptor offsets. Free counts summing to the totals across groups exercises the whole-table check. |
| `EXT2 inode 2` and `EXT2 root blocks` | `debugfs stat <2>`: mode, size, links, block count, block list | The inode offsets and pointer resolution. |
| The entry count and each entry's inode | `debugfs ls -l /` | Every record length in every block of the root. |
| The probed path | `debugfs stat PATH`, and `xxd` of the file | Resolution and reading. |

Variations that reach the harder paths:

| Image | Made with | Reaches |
| ----- | --------- | ------- |
| 4 KiB blocks | `mke2fs -b 4096 …` | `first data block 0` and its geometry. |
| A root of about 900 entries | `mke2fs -d root/ …` from a populated directory | The indirect block: index 12 comes from a pointer block `debugfs` lists as `(IND)`, which the kernel never prints. |
| A root of about 9,000 entries | `mke2fs -N 16384 -d bigroot/ … 65536` | The doubly indirect block (index 268 on 1 KiB blocks), and a traversal across both boundaries without losing a record. |
| A tree with a file, a deep directory, and a fast and a slow link | `mke2fs -I 128 -r 1 -d tree/ …` | Both link forms (`debugfs stat` shows `Blockcount: 0` for the fast one) and a path through a link, e.g. `/shortlink/deeper/buried.txt`. |
| A disk with no filesystem | — | The refusal `the volume bears no EXT2 magic number`. |

The probe path is a constant in `kernel/kernel.c`; change it for a run to probe a
particular path.

## 3. Writing a volume, judged by `e2fsck`

The *Oxys-OS (EXT2 write self-test)* GRUB entry permits writing to the volume at
`/mnt`. Even then the test writes only to an existing regular file named
`/oxys-write-test`, and **never creates it**: a volume without that file is left
untouched. It empties the file and writes 8,192 bytes, each derived from its
offset; creates `/oxys-made` holding `within`, writes to it, and removes both.

```sh
mkdir tree && printf 'twelve bytes' > tree/oxys-write-test
mke2fs -q -F -b 1024 -I 128 -r 1 -d tree/ fs.img 8192
e2fsck -fn fs.img                      # note files and blocks in use
# boot the write self-test entry with fs.img attached, as in Section 2
e2fsck -fn fs.img
debugfs -R "dump /oxys-write-test out.bin" fs.img
```

The log reports:

```
EXT2 write test: the command line permits writing to ata0.
EXT2 write test: created /oxys-made (inode 14) holding within (inode 15).
EXT2 write test: removed both again.
EXT2 write test: wrote 8192 bytes to /oxys-write-test (inode 13, 16 sectors); volume now reports 7877 free blocks and 2035 free inodes.
```

A pass is: **`e2fsck -fn` reports no errors in any of its five passes**; the file
count is unchanged (the two created inodes were returned); the block count rose
by exactly the blocks the file grew by; `out.bin` matches the offset pattern for
all 8,192 bytes; and the other files read as before. The passes that matter:

| `e2fsck` pass | Judges |
| ------------- | ------ |
| 1 | Inodes, block pointers and sizes. |
| 2 | Directory records split and joined by insertion and removal. |
| 3 | The `.` and `..` written for a new directory. |
| 4 | Link counts, including the parent's, which nothing inside the kernel can see. |
| 5 | The free counts in every descriptor and the superblock, and the bitmaps. |

The same judgement applies to the persistent `/etc` volume, which
`tools/etc-disk.sh check` runs `e2fsck` over ([`PERSIST.md`](PERSIST.md)).

## Verification

This document is itself a verification procedure. Its runs are recorded in
[`../project/TESTING-RECORD.md`](../project/TESTING-RECORD.md).

## Limitations

1. The comparisons of Section 2 are made by a person reading two outputs; nothing
   automates them beyond the ramdisk's byte comparison.
2. The write test needs a prepared image and a GRUB entry; `make verify` does not
   run it.
