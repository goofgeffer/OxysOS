<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# EXT2: the Volume and its Structures

**Phase**: sub-tasks 5.1 to 5.3 of [`../project/PLAN.md`](../project/PLAN.md).
**Source**: [`../../kernel/fs/ext2/`](../../kernel/fs/ext2/) — `core.c` (shared
state, byte order, block transfer), `superblock.c`, `group.c`, `inode.c`, and
`internal.h`; the interface
[`../../kernel/include/oxys/fs/ext2.h`](../../kernel/include/oxys/fs/ext2.h).
**Specifications**: Dave Poirier, *The Second Extended File System: Internal
Layout* (the superblock, Table 3.3 onwards; the group descriptor; the inode,
Tables 3.14 onwards), registered in [`../project/REFERENCES.md`](../project/REFERENCES.md).

The three structures every operation on an EXT2 volume acts through: the
superblock, the block group descriptor table and the inode. What is done with
them (directories, paths, reading, writing, names) is
[`EXT2-FILES.md`](EXT2-FILES.md); the cross-check against volumes made by
e2fsprogs is [`EXT2-VERIFICATION.md`](EXT2-VERIFICATION.md); how volumes are
mounted is [`VFS.md`](VFS.md).

## 1. Reading the superblock

Every other structure is found by arithmetic on the superblock: the descriptors
follow it at a block that depends on the block size it states; an inode's group
is its number divided by the inodes per group; a block's device address is its
number times the block size. A superblock read wrongly therefore produces no
error, only a filesystem that addresses the wrong blocks consistently. Most of
the superblock code is validation for that reason.

- **Position.** 1,024 bytes into the volume, 1,024 bytes long, whatever the block
  size (the first KiB is reserved for a boot sector). On a device of 512-byte
  blocks that is blocks 2 and 3, read through the buffer cache
  ([`BUFFER.md`](BUFFER.md)).
- **Byte order.** Every field is little-endian. The bytes are decoded field by
  field (`Ext2ReadWord` and its kin) rather than overlaid with a structure: an
  overlay needs a packed structure, a compiler extension the register in
  [`../project/CODING-STANDARDS.md`](../project/CODING-STANDARDS.md) admits only
  where no conforming alternative exists; and it would make the byte order a
  property of the processor, true by accident and recorded nowhere.
- **Offsets live in the header**, not the parser, because they are the format.
  The self-test composes volumes from the same names; a test stating them again
  would agree with a wrong parser as readily as a right one.

| Offset | Width | Field | Use |
| ------ | ----- | ----- | --- |
| 0 | 4 | `s_inodes_count` | With inodes per group, gives the group count. |
| 4 | 4 | `s_blocks_count` | With blocks per group, gives it again. |
| 8 | 4 | `s_r_blocks_count` | Blocks reserved to the superuser. |
| 12, 16 | 4 | `s_free_blocks_count`, `s_free_inodes_count` | Maintained; never above the totals. |
| 20 | 4 | `s_first_data_block` | The block holding the superblock: 1 for 1 KiB blocks, else 0. |
| 24 | 4 | `s_log_block_size` | Block size is `1024 << value`. |
| 28 | 4 | `s_log_frag_size` | Fragment size, by the same rule. |
| 32, 36, 40 | 4 | `s_blocks_per_group`, `s_frags_per_group`, `s_inodes_per_group` | Group geometry. |
| 44, 48 | 4 | `s_mtime`, `s_wtime` | Reported. |
| 52, 54 | 2 | `s_mnt_count`, `s_max_mnt_count` | The count is raised at a writable mount. |
| 56 | 2 | `s_magic` | `0xEF53`. |
| 58 | 2 | `s_state` | 1 clean, 2 errors found. |
| 60 | 2 | `s_errors` | Reported. |
| 62 | 2 | `s_minor_rev_level` | Reported. |
| 64, 68, 72 | 4 | `s_lastcheck`, `s_checkinterval`, `s_creator_os` | Reported. |
| 76 | 4 | `s_rev_level` | 0 or 1 (Section 2). |
| 80, 82 | 2 | `s_def_resuid`, `s_def_resgid` | Reported. |
| 84 | 4 | `s_first_ino` | First inode a file may use. |
| 88 | 2 | `s_inode_size` | Size of an inode on this volume. |
| 90 | 2 | `s_block_group_nr` | Which group's copy this is. |
| 92, 96, 100 | 4 | `s_feature_compat`, `s_feature_incompat`, `s_feature_ro_compat` | Section 3. |
| 104 | 16 | `s_uuid` | Identifier. |
| 120 | 16 | `s_volume_name` | Label; zero-padded, unterminated when full. |
| 136 | 64 | `s_last_mounted` | Likewise. |

Character fields are copied into buffers one byte longer than the field, since a
full field carries no terminator.

## 2. Revisions

Revision 0 has no inode size, first inode or feature fields: an inode is 128
bytes and the first free inode is 11. The parser fills those values in for a
revision 0 volume so that everything above sees one description. A revision above
1 is refused, since its fields may lie elsewhere.

## 3. Features

| Field | Meaning of an unknown bit | This kernel implements | Otherwise |
| ----- | ------------------------- | ---------------------- | --------- |
| `s_feature_compat` | Read and write anyway | (none needed) | Reported and ignored. |
| `s_feature_ro_compat` | Read, but writing would break an invariant | `SPARSE_SUPER`, `LARGE_FILE` | Mounted **read-only**. |
| `s_feature_incompat` | Cannot be read at all | `FILETYPE` | **Refused** (compression, journal recovery, journal device, `META_BG`, …). |

A volume not marked cleanly unmounted is also read-only: it may be consistent,
but writing to one that may be mid-repair is how a damaged filesystem becomes an
unrecoverable one. [`PERSIST.md`](PERSIST.md) describes the one exception.

## 4. What the superblock is refused for

| Refused | Why |
| ------- | --- |
| No `0xEF53` at offset 56. | Not an EXT2 volume. |
| Revision above 1. | Fields may lie elsewhere. |
| `s_log_block_size` above 2. | Blocks above 4 KiB, which nothing above can hold. |
| Block size below the device's block size. | A filesystem block must be whole device blocks. |
| `s_first_data_block` disagreeing with the block size. | The superblock's position stated twice, differently; every group calculation starts here. |
| A zero block count, inode count, blocks per group or inodes per group. | Division by zero, or an empty volume. |
| More blocks or inodes per group than one bitmap block has bits. | The group cannot be represented. |
| More free blocks or inodes than exist. | Self-contradiction. |
| An inode size not a power of two between 128 and the block size. | Inodes must tile a block, or every inode after the first group is misplaced. |
| A first inode below 11, or beyond the volume. | Reserved inodes handed out as files. |
| A group count from the blocks that differs from the one from the inodes. | See below. |
| An unimplemented incompatible feature. | The volume says so itself. |

The group count is derived twice, from independently written fields:
`ceil((s_blocks_count − s_first_data_block) / s_blocks_per_group)` and
`ceil(s_inodes_count / s_inodes_per_group)`. On a sound volume they agree
exactly; if not, either the volume is corrupt or one of four fields is being
read from the wrong offset, which no single plausible number could reveal.

## 5. The group descriptor table

A volume is divided into block groups. The descriptor table says, for each group,
where its block bitmap, inode bitmap and inode table begin, and how much is free.

**Location.** The block after the superblock: `s_first_data_block + 1`
(`Ext2GroupDescriptorBlock`). A descriptor is 32 bytes; descriptor *n* is at byte
`32n` of the table, in block `table + 32n / block_size`, offset
`32n % block_size`.

| Offset | Width | Field |
| ------ | ----- | ----- |
| 0 | 4 | `bg_block_bitmap` |
| 4 | 4 | `bg_inode_bitmap` |
| 8 | 4 | `bg_inode_table` |
| 12 | 2 | `bg_free_blocks_count` |
| 14 | 2 | `bg_free_inodes_count` |
| 16 | 2 | `bg_used_dirs_count` |
| 18 | 14 | Padding and reserved; not read |

- **Block numbers are absolute**, not relative to the group. They are small
  numbers next to a group number, and adding the group's first block to them
  still addresses real blocks: the wrong ones.
- **Only the bytes needed are read.** `Ext2ReadBytes` copies a run from within
  one block through the cache; copying a whole 4 KiB block to the stack to take
  four bytes from it would waste a stack the kernel cannot spare.
- **The last group is short** whenever the volume is not a multiple of the group
  size, which is usual. `Ext2GroupBlockCount` returns its true size, and its free
  count is checked against that.

A descriptor is six plausible numbers wherever it is read from. A table read one
block early, or with descriptors of 24 or 40 bytes, names real blocks, and a
kernel that then wrote an inode would write it over a file. So:

| Refused | Why |
| ------- | --- |
| A group at or beyond the count. | A read past the table. |
| A table that does not fit in the volume. | The same, from the other side. |
| A bitmap or inode table below `s_first_data_block` or beyond the last block. | Nothing lies before the first data block. |
| An inode table that starts inside the volume and ends outside it. | Its length is implied by the inode size; checking its first block alone misses its end. |
| Two structures starting on the same block. | Each is at least a block; a descriptor read four bytes adrift gives two equal pointers far more often than three plausible ones. |
| Free counts above what the group holds; more directories than inodes in use. | Self-contradiction. |
| Free counts that do not sum to the superblock's totals. | A table read at the wrong offset or one descriptor short. |

The last check holds only on a cleanly unmounted volume, which is allowed to
disagree with itself only when not clean. `Ext2VerifyGroupDescriptors` checks
each descriptor of an unclean (and therefore read-only) volume, but not the sum.

## 6. The inode

An inode describes one file (format, permissions, owner, times, size, blocks) and
carries no name; names live in directories, which is what makes hard links
possible.

**Finding one.** Inode numbers start at one:

```
group = (number - 1) / s_inodes_per_group
index = (number - 1) % s_inodes_per_group
```

and the inode is at `index * s_inode_size` in the group's inode table. Every
likely mistake (omitting the subtraction, using the block size for the inode size,
taking the group's first block for its table) lands on another valid inode of the
same volume. The inode size is a power of two no larger than a block, so no inode
straddles two blocks and the 128 bytes are read as one run. Only the **first 128
bytes** are decoded, whatever `s_inode_size` states; `mke2fs` defaults to 256.

**Size.** On revision 1, the high 32 bits of a **regular file's** size are at
offset 108 (`i_dir_acl` in revision 0), and are joined here. Only for a regular
file: on a directory the same bytes mean something else, and joining them gives
a directory a size of gigabytes.

**Blocks.** `i_block` holds fifteen numbers: twelve direct, then single, double
and triple indirect. With block size *B*, a block holds *P = B/4* pointers:

| Index | Reached through |
| ----- | --------------- |
| 0 to 11 | `i_block` directly |
| 12 to 11 + *P* | the indirect block |
| 12 + *P* to 11 + *P* + *P*² | the doubly indirect block |
| 12 + *P* + *P*² to 11 + *P* + *P*² + *P*³ | the triply indirect block |
| beyond | refused |

`Ext2InodeBlock` reduces the index by each range it passes, then walks as many
pointer blocks as the level requires, one loop for all three levels.

- **Zero is a hole**, a block never allocated, which reads as zeroes; it is not
  the end of the list. A zero pointer block is a hole covering its whole subtree,
  so `Ext2ReadPointer` returns zero for every entry beneath it without reading.
- **The index is not checked against the size**: a reader bounds itself by the
  size, but a walk of allocated blocks must not be.

| Refused | Why |
| ------- | --- |
| Inode 0, or a number above `s_inodes_count`. | Zero names nothing; it marks an unused directory record. |
| An inode whose group's descriptor is refused. | Its table was located by an untrusted descriptor. |
| A pointer in `i_block` outside the volume. | Checked when the inode is read, before use. Skipped for a fast symbolic link, whose `i_block` holds text ([`EXT2-FILES.md`](EXT2-FILES.md)). |
| A pointer in an indirect block outside the volume. | Checked when fetched, since that block was not read with the inode. |
| An inode with no format and no links. | An unfilled table entry. |
| A deleted inode: `i_dtime` set and no links. | A surviving directory entry leading to a freed inode whose blocks may belong to another file. |
| An index beyond the triply indirect range. | Arithmetic past the decomposition. |

## Verification

`KernelVerifyExt2`, `KernelVerifyGroups` and the inode chapter, in
[`../../kernel/test/storage/ext2.c`](../../kernel/test/storage/ext2.c) and
[`../../kernel/test/storage/ext2/`](../../kernel/test/storage/ext2/). They compose
a volume in the memory device of [`BLOCK.md`](BLOCK.md) from the header's offset
names, so every value is known. The cache is invalidated around each alteration,
since the test writes beneath it.

| Property asserted | The failure it would catch |
| ----------------- | -------------------------- |
| Every superblock field equals the value composed at its offset. | A field read from the wrong offset. |
| Block size, sectors per block and group count are derived correctly. | An exponent used as a multiplier; a group count off by one. |
| A padded, unterminated label reads correctly. | A label running into the last-mounted path. |
| A revision 0 volume gets the fixed inode size and first inode, and no features. | Fields read that the revision does not have. |
| Each refusal of Section 4 refuses. | A volume that must not be addressed, read anyway. |
| An unknown read-only feature gives a read-only volume; an unknown incompatible feature, a refusal. | The two fields treated alike. |
| An unclean volume is read-only; a device too short for a superblock is refused. | Writing mid-repair; a read past the device. |
| The descriptor table is at block 2 and one block long; the inode table two blocks. | Geometry misderived. |
| The short last group is 127 blocks, not 8,192. | A free count checked against a full group. |
| Every descriptor field equals the composed value. | A field read from the wrong offset. |
| Each descriptor refusal of Section 5 refuses, including one caught only by the whole-table sum. | A table read adrift, which no single descriptor reveals. |
| Inode 2 is a directory with the composed mode, links and block. | Format bits taken from the wrong end of `i_mode`. |
| Inode 11 is found in the **second** block of the table, every field as composed. | A reader that never leaves the table's first block. |
| Indices 0, 11, 12, 11 + *P*, 12 + *P* + 5 and 12 + *P* + *P*² + 3 resolve to the composed blocks. | Each range boundary, and each level's divisor. |
| Index 13, the first doubly indirect index, and an index under an absent subtree are holes, the last without reading. | A hole taken for the end or an error. |
| An index beyond the triple range, inode 0, an inode beyond the count, and an unfilled inode are refused. | Arithmetic past the end; numbers used as indices. |
| An out-of-volume pointer is refused in `i_block` at read and in an indirect block at fetch. | Either check missing. |

The same code is checked against volumes made by `mke2fs` at every boot (the
initial ramdisk, [`INITRD.md`](INITRD.md)) and by hand
([`EXT2-VERIFICATION.md`](EXT2-VERIFICATION.md)).

## Limitations

These cover all of this kernel's EXT2 support, including
[`EXT2-FILES.md`](EXT2-FILES.md).

1. Backup superblocks and descriptor tables are never consulted.
2. Times are not maintained. `s_mtime`, `s_wtime`, `i_atime`, `i_mtime` and
   `i_ctime` are kept as found, and a created file has times of zero, although
   the kernel now has a clock ([`../devices/TIME.md`](../devices/TIME.md)).
3. Block sizes above 4 KiB are refused; fragments are not implemented (no EXT2
   ever used them).
4. One volume per device, starting at its first block: partition tables are not
   read.
5. Only the first 128 bytes of an inode are decoded: no nanosecond times, no
   extended attributes. `i_faddr`, `i_osd1` and `i_osd2` are not read.
6. `META_BG` is refused as an unimplemented incompatible feature.
7. The filesystem block may not be smaller than the device's.
8. Created files have uid and gid 0.
9. A directory never shrinks; freed records become slack for later names.
10. No rename: a move is a link and an unlink, with a moment in between.
11. No atomicity across the several structures one write touches; recovery is
    `e2fsck`. EXT2 has no journal.
12. No preallocation; `s_prealloc_blocks` is read and ignored.
13. A symbolic link's target is at most 255 bytes, and at most eight links are
    followed per path (POSIX's minimum). Each level is a stack frame with its own
    buffer.
14. An indexed directory's hash tree is not used; lookup is linear.
15. No read-ahead or caching above the buffer cache; each read re-walks the
    indirect blocks (from cache).
16. No orphan list (`s_last_orphan`), so the VFS refuses to unlink an open file
    ([`VFS.md`](VFS.md)). The field is preserved across writes.
