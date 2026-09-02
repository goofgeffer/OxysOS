# The EXT2 Volume

**Phase**: 5, sub-tasks 5.1 and 5.2, of [`../project/PLAN.md`](../project/PLAN.md).

**Authority**: `PROJECT_GUIDELINES.md`, Sections 2, 3 and 6. Every assertion about
the format carries a citation, and the specifications are registered in
[`../project/REFERENCES.md`](../project/REFERENCES.md).

**Implementation**: [`../../kernel/fs/ext2.c`](../../kernel/fs/ext2.c),
[`../../kernel/include/oxys/ext2.h`](../../kernel/include/oxys/ext2.h).

## 1. Why the superblock comes first

Every other structure of an EXT2 volume is found by arithmetic upon the
superblock. The block-group descriptors lie in the block after it, and which
block that is depends upon the block size it states. An inode is found from the
group it belongs to, which is its number divided by the inodes per group; within
the group it is found by multiplying by the inode size. A block number is
converted to a device address by multiplying by the block size.

Every one of those quantities is stated in the superblock, so a superblock read
wrongly does not produce an error. It produces a filesystem that addresses the
wrong blocks consistently and confidently, for as long as the machine runs.

That is why this sub-task is separate from the rest of Phase 5, and why it
consists of as much validation as parsing.

## 2. Where it is, and how it is read

The superblock begins 1024 bytes into the volume and occupies 1024 bytes. The
first kibibyte is left for a boot sector and belongs to nobody else, so the
superblock's position does not depend upon the block size — which is convenient,
since the block size is one of the things it states.

Upon a device of 512-byte blocks that is blocks 2 and 3. They are read through
the **buffer cache** of [`BUFFER.md`](BUFFER.md) rather than through the block
layer directly. This is the first caller above the cache, and it is the natural
one: the superblock is the block a filesystem reads most often, and every
structure derived from it will be read through the same cache.

## 3. Byte order, and why there is no structure overlaid upon the bytes

Every quantity upon an EXT2 volume is stored least significant byte first,
whatever the machine that wrote it and whatever the machine that reads it. This
kernel runs upon a little-endian processor, so a C structure laid over the raw
bytes would work.

It is not done, for two reasons.

- **It would require packing the structure**, which is a compiler extension. The
  register of extensions in [`../project/CODING-STANDARDS.md`](../project/CODING-STANDARDS.md)
  admits inline assembly, because no conforming alternative to an `IN`
  instruction exists; an alternative to a packed structure does exist and is
  three lines long.
- **It would make the byte order invisible.** Written out, the format's byte
  order is stated in the code that depends upon it: `Ext2ReadWord` says plainly
  that the first byte is the least significant. Overlaid, the same fact is a
  property of the processor the code happens to be compiled for, recorded
  nowhere and true by accident.

So the superblock is read into a buffer of bytes and decoded field by field into
a structure of the processor's own types. The offsets are named in
`kernel/include/oxys/ext2.h` rather than in the parser, because they are the
format and not the parser's opinion of it — the self-test composes a volume from
those same names, and a test that stated the offsets a second time would agree
with a mistaken parser as readily as with a correct one.

## 4. The fields

| Offset | Width | Field | Used for |
| ------ | ----- | ----- | -------- |
| 0 | 4 | `s_inodes_count` | The inode total; with the inodes per group it gives the group count. |
| 4 | 4 | `s_blocks_count` | The block total; with the blocks per group it gives the group count again. |
| 8 | 4 | `s_r_blocks_count` | Blocks reserved to the superuser. |
| 12, 16 | 4 | `s_free_blocks_count`, `s_free_inodes_count` | Reported; asserted not to exceed the totals. |
| 20 | 4 | `s_first_data_block` | The block the superblock lies within: 1 for 1024-byte blocks, 0 otherwise. |
| 24 | 4 | `s_log_block_size` | The block size, as `1024 << s_log_block_size`. |
| 28 | 4 | `s_log_frag_size` | The fragment size, by the same rule. |
| 32, 36, 40 | 4 | `s_blocks_per_group`, `s_frags_per_group`, `s_inodes_per_group` | The geometry of a block group. |
| 44, 48 | 4 | `s_mtime`, `s_wtime` | Reported. |
| 52, 54 | 2 | `s_mnt_count`, `s_max_mnt_count` | Reported. |
| 56 | 2 | `s_magic` | `0xEF53` upon every EXT2 volume. |
| 58 | 2 | `s_state` | 1 clean, 2 errors detected. |
| 60 | 2 | `s_errors` | What an implementation should do upon an error. |
| 62 | 2 | `s_minor_rev_level` | Reported. |
| 64, 68, 72 | 4 | `s_lastcheck`, `s_checkinterval`, `s_creator_os` | Reported. |
| 76 | 4 | `s_rev_level` | 0 or 1; see Section 5. |
| 80, 82 | 2 | `s_def_resuid`, `s_def_resgid` | Reported. |
| 84 | 4 | `s_first_ino` | The first inode a file may use. |
| 88 | 2 | `s_inode_size` | The size of an inode upon this volume. |
| 90 | 2 | `s_block_group_nr` | Which group's copy of the superblock this is. |
| 92, 96, 100 | 4 | `s_feature_compat`, `s_feature_incompat`, `s_feature_ro_compat` | See Section 6. |
| 104 | 16 | `s_uuid` | The volume's identifier. |
| 120 | 16 | `s_volume_name` | The label, padded with zeroes and not terminated when full. |
| 136 | 64 | `s_last_mounted` | The path it was last mounted upon, likewise padded. |

The character fields are copied into buffers one byte longer than the field, for
the reason just given: a field that is entirely full carries no terminator upon
the volume.

## 5. The two revisions

A volume of revision 0 has no field for the inode size or for the first usable
inode, because both were fixed when the format was made: an inode is 128 bytes
and the first one available to a file is 11, inodes 1 to 10 being reserved. A
volume of revision 1 states both, and states the three feature fields, which
revision 0 also lacks.

The parser fills those fields with their fixed values for a revision 0 volume,
so that everything above sees one description of a volume and not two. It refuses
a revision beyond 1 outright: a later revision may place fields where this code
does not expect them, and reading it hopefully is exactly the failure Section 1
describes.

## 6. Incompatible, read-only compatible, and compatible

The three feature fields exist so that a volume can tell an implementation of any
age what it must not attempt. The distinction between them is the whole point:

- **Compatible** (`s_feature_compat`): an implementation that does not know the
  feature may read and write the volume anyway. Recorded and reported here,
  otherwise ignored.
- **Read-only compatible** (`s_feature_ro_compat`): an implementation that does
  not know the feature may read the volume but must not write it, since writing
  would violate an invariant it cannot see. This kernel implements
  `SPARSE_SUPER` and `LARGE_FILE`; a volume declaring anything else is accepted
  **read-only**.
- **Incompatible** (`s_feature_incompat`): an implementation that does not know
  the feature cannot read the volume at all. This kernel implements `FILETYPE`;
  a volume declaring anything else — compression, a journal awaiting recovery, a
  journal device, meta block groups — is **refused**.

A volume not marked as cleanly unmounted is also accepted read-only. It may be
perfectly consistent; nothing here can establish that, and writing to a volume
that may be mid-repair is how a damaged filesystem becomes an unrecoverable one.

## 7. What is refused

| Refused | Why it matters |
| ------- | -------------- |
| No `0xEF53` at offset 56. | It is not an EXT2 volume; everything below would be arithmetic upon somebody else's data. |
| A revision above 1. | Fields may lie elsewhere. |
| `s_log_block_size` above 2. | A block larger than 4096 bytes, which nothing above is prepared to hold. |
| A block size below the device's own block size. | A filesystem block that is not a whole number of device blocks cannot be addressed. |
| `s_first_data_block` disagreeing with the block size. | The superblock's own position, stated twice, disagreeing with itself. Every group calculation starts here. |
| A zero block count, inode count, blocks per group or inodes per group. | A division by zero, or a volume with nothing in it. |
| More blocks or inodes per group than a one-block bitmap has bits. | A group's blocks and inodes are recorded in bitmaps of one block each; a larger group cannot be represented. |
| More free blocks or inodes than the volume holds. | A superblock that contradicts itself. |
| An inode size that is not a power of two between 128 and the block size. | Inodes must tile a block exactly; otherwise one straddles two blocks and every inode after the first group is at the wrong offset. |
| A first usable inode below 11, or beyond the volume. | The reserved inodes would be handed out as ordinary files. |
| A group count derived from the blocks that differs from the one derived from the inodes. | The strongest check available, and the subject of the next paragraph. |
| An incompatible feature this kernel does not implement. | The volume says so itself. |

The group count is derivable twice, from two fields that were written
independently: `ceil((s_blocks_count - s_first_data_block) / s_blocks_per_group)`
and `ceil(s_inodes_count / s_inodes_per_group)`. On a sound volume they agree
exactly. If they do not, either the volume is corrupt or the parser is reading
one of the four fields from the wrong offset — and that second possibility is
precisely the failure that no amount of reading a plausible number can reveal.

## 8. The block group descriptor table

A volume is divided into **block groups**, each holding a fixed number of blocks
and inodes. The group descriptor table names, for every group, where its three
structures begin and how much of the group is free. It is the structure through
which every other structure of the filesystem is found: an inode is located by
dividing its number by `s_inodes_per_group` to obtain a group, reading that
group's descriptor to obtain the block its inode table begins at, and indexing
into that table.

### 8.1 Where the table is

The table begins upon **the first block following the superblock**. The
superblock always occupies the second kibibyte of the volume, so the block
containing it is block 1 upon a volume of 1024-byte blocks and block 0 upon any
larger one — which is exactly what `s_first_data_block` holds. The table
therefore begins at `s_first_data_block + 1`, and `Ext2GroupDescriptorBlock`
computes it that way rather than by a comparison against the block size, since
the volume has already been made to state the same thing twice and to agree with
itself (Section 7).

A descriptor is **32 bytes**. The table may occupy several blocks, so a
descriptor is located by its position within the table and not within a block of
it: descriptor *n* lies at byte `32n`, in block `table + 32n / block_size` at
offset `32n % block_size`.

| Offset | Width | Field | Held as |
| ------ | ----- | ----- | ------- |
| 0 | 4 | `bg_block_bitmap` | `block_bitmap` |
| 4 | 4 | `bg_inode_bitmap` | `inode_bitmap` |
| 8 | 4 | `bg_inode_table` | `inode_table` |
| 12 | 2 | `bg_free_blocks_count` | `free_block_count` |
| 14 | 2 | `bg_free_inodes_count` | `free_inode_count` |
| 16 | 2 | `bg_used_dirs_count` | `used_directory_count` |
| 18 | 2 | `bg_pad` | not read |
| 20 | 12 | `bg_reserved` | not read |

Every block identifier in a descriptor is **absolute** — a block number of the
volume, not of the group. The specification states this expressly, and it is the
one thing about this structure that is easy to assume wrongly: the numbers are
small, they sit beside a group number, and a kernel that added the group's first
block to them would still address real blocks of the volume.

The bytes are decoded field by field, for the reasons Section 3 gives. The
offsets are declared in the header beside the superblock's, so that the self-test
composes a descriptor from the same names the parser reads.

### 8.2 Reading only what is needed

`Ext2ReadBytes` reads a run of bytes from within one filesystem block through the
buffer cache, copying from each device block the run spans. Callers ask for the
bytes they need and no more: a descriptor is 32 bytes and a block pointer is
four. The cache is holding the block regardless, so copying a whole 4096-byte
block onto the kernel stack in order to take four bytes out of it would be both
wasteful and a stack this kernel does not have to spare.

### 8.3 What is refused

A descriptor is six numbers, and every one of them is a plausible number wherever
it is read from. A table read one block early, or a descriptor taken to be 24 or
40 bytes rather than 32, yields block numbers that address real blocks of the
volume — the wrong ones — and a kernel that then wrote an inode would write it
over a file.

| Refused | Why it matters |
| ------- | -------------- |
| A group at or beyond `group_count`. | A read past the end of the table, answered with whatever follows it. |
| A descriptor table that does not fit within the volume. | The same, from the other direction. |
| A bitmap or inode table below `s_first_data_block` or at or beyond the block count. | Nothing of a filesystem lies before the first data block, so an identifier below it is as wrong as one beyond the end. |
| An inode table that begins within the volume and ends beyond it. | Its length is stored nowhere and follows from the inode size; a kernel checking only the first block would read the last inodes of the group from nowhere. |
| Two of the three structures beginning upon the same block. | Each is at least one block long, so no two can share a block. A descriptor read four bytes adrift yields two identical pointers far more often than three plausible ones. |
| More free blocks than the group holds, or more free inodes than `s_inodes_per_group`. | A group that contradicts itself. |
| More directories than the group has inodes in use. | A directory occupies an inode that is in use. |
| Groups whose free counts do not sum to the superblock's totals. | The statement the table makes as a whole; see below. |

The last is the strongest statement that can be made about the table without
reading the bitmaps. The groups account for every free block and every free inode
of the volume, so `sum(bg_free_blocks_count)` must equal `s_free_blocks_count`
and likewise for the inodes. A table read at the wrong offset, or one descriptor
short, yields descriptors that are individually plausible and a sum that is not.

It holds only of a volume marked cleanly unmounted. A volume that was not is
permitted to disagree with itself — that disagreement is what the state means —
and `Ext2ReadSuperblock` has already made it read-only, so
`Ext2VerifyGroupDescriptors` checks the individual descriptors of such a volume
and not the sum.

### 8.4 The last group is short

Every group holds `s_blocks_per_group` blocks except the last, which holds
whatever remains. The division that fixes the group count rounds upward, so the
last group is short whenever the volume is not an exact multiple of the group
size — which is the usual case and not the exception.
`Ext2GroupBlockCount` returns the short count for the last group, and the free
block count of a descriptor is checked against it rather than against
`s_blocks_per_group`.

## 9. Verification

### 9.1 The self-test of the superblock

`KernelVerifyExt2` composes a superblock in the memory-backed block device of
[`BLOCK.md`](BLOCK.md), Section 5, using the offset names from the header, and
asserts the parser against it. Because the volume is composed rather than found,
every field's value is known and can be named.

| Property asserted | The silent failure it would catch |
| ----------------- | --------------------------------- |
| Every parsed field equals the value composed at that offset. | A field read from the wrong offset, which yields a plausible number rather than an error. |
| The block size, the sectors per block and the group count are derived correctly. | An exponent applied as a multiplier, or a group count off by one at the boundary. |
| The revision 1 fields, including a label that is padded and not terminated. | A label read as a terminated string, running into the last-mounted path. |
| A volume of revision 0 is given the fixed inode size and first inode, and no features. | Reading fields that do not exist upon such a volume — the bytes at those offsets belong to something else. |
| Each of the twelve refusals of Section 7 refuses. | A volume this kernel must not address being read anyway. |
| An unimplemented read-only feature yields a readable volume, not a refused one; an unimplemented incompatible feature yields a refused one. | The two fields treated alike, which either locks the kernel out of volumes it could read or lets it write volumes it must not. |
| A volume not cleanly unmounted is accepted read-only. | Writing to a filesystem that may be mid-repair. |
| A device too short to hold a superblock is refused. | A read past the end of a device, answered by whatever the layer beneath does with it. |

The cache is invalidated around each alteration. The self-test writes into the
device's storage directly, beneath both the block layer and the cache, so a cache
holding the previous contents would answer the next read with them, and the
assertion would be made against a volume that no longer exists.

### 9.2 The self-test of the descriptor table

`KernelVerifyGroups` runs against the same composed volume, whose group
descriptor is written from the offset names of the header. It is asserted here
rather than in a self-test of its own because a descriptor can only be reached
through a superblock, and this is where a valid one exists.

| Property asserted | The silent failure it would catch |
| ----------------- | --------------------------------- |
| The table begins at block 2, occupies one block, and the inode table occupies two. | Geometry derived from the superblock by the wrong arithmetic, which would place the table upon a block that holds something else. |
| Group 0 spans 127 blocks, not 8192. | The last group taken to be full length, so a check against it admits a free count larger than the volume. |
| Every field of the descriptor equals the value composed at that offset. | A field read from the wrong offset, which yields a plausible block number rather than an error. |
| A group beyond the count is refused. | A read past the end of the table. |
| A bitmap at block 0, and an inode table at block 200, are refused. | Identifiers outside the volume in both directions. |
| An inode table at block 127 — within the volume, ending beyond it — is refused. | A length check omitted because only the first block was validated. |
| Two structures upon one block are refused. | A descriptor read four bytes adrift. |
| Free counts and a directory count beyond what the group holds are refused. | A descriptor that contradicts itself. |
| A descriptor that every other rule accepts, whose free count disagrees with the superblock's, is refused by the whole-table check. | A table read at the wrong offset or one descriptor short — the failure no individually plausible descriptor can reveal. |

### 9.3 A volume the kernel did not compose

A self-test that builds its own volume proves the parser consistent with itself.
The corroboration must come from a volume built by something else, so three were
made with `mke2fs` and read:

```sh
mke2fs -q -t ext2 -b 1024 -L oxys-root -F ext2.img 16384

qemu-system-x86_64 -machine pc -cpu qemu64 -smp cores=2 -m 512M \
    -cdrom build/oxys.iso \
    -drive file=ext2.img,format=raw,if=ide,index=0,media=disk \
    -display none -serial file:ext2.log
```

The kernel reported:

```
EXT2 volume upon ata0: revision 1.0, labelled oxys-root, writable.
EXT2 volume: 16384 blocks of 1024 bytes (16384 KiB), 15211 free; 4096 inodes of 256 bytes, 4085 free.
EXT2 volume: 2 groups of 8192 blocks and 2048 inodes, first data block 1, first usable inode 11.
EXT2 volume: features compatible 0x38, incompatible 0x2, read-only 0x3, state clean.
```

and, of the first of its two groups:

```
EXT2 group 0: block bitmap at 66, inode bitmap at 67, inode table at 68; 7599 free blocks, 2037 free inodes, 2 directories.
```

Every figure matches `dumpe2fs -h` upon the same image, field for field: the
block and inode counts, both free counts, the geometry of the groups, the inode
size of 256 bytes, and the three feature words — `ext_attr resize_inode dir_index`
as `0x38`, `filetype` as `0x2`, and `sparse_super large_file` as `0x3`. The group
line matches `dumpe2fs` in full, and the whole-table check passed silently upon a
volume of two groups whose free counts sum to the superblock's totals — 7599 and
7612 blocks against 15211, 2037 and 2048 inodes against 4085.

A second image of 4096-byte blocks was read equally, and reported
`first data block 0` as the format requires of any block size but 1024; its one
group reported bitmaps at blocks 6 and 7 and an inode table at block 8, with
18736 free blocks and 19989 free inodes, which `dumpe2fs` states identically. A
disk holding no filesystem at all was refused with *the volume bears no EXT2
magic number*.

## 10. Limitations

1. **Nothing is mounted.** The superblock is read, validated and reported; no
   volume is retained and nothing is opened. Sub-task 5.8 introduces the mount.
2. **The backup superblocks are not consulted.** A volume whose primary
   superblock is damaged is refused, though a copy of it — and of the descriptor
   table — stands in several block groups. Nothing yet falls back upon them.
3. **Nothing is written.** The mount count and the state are read and not
   updated; a volume this kernel reads does not know it was read.
4. **Block sizes above 4096 bytes are refused**, and fragments are not
   implemented at all — no EXT2 implementation has ever used them.
5. **One volume per device.** Partition tables are not read, so a volume must
   begin at the start of its device.
6. **The bitmaps are not read.** The descriptor states where the block and
   inode bitmaps of each group begin, and nothing yet looks at them. Until
   sub-task 5.6 nothing is allocated, so nothing needs to know which blocks are
   in use; the free counts are read and believed.
7. **`META_BG` is not implemented.** A volume declaring it is refused as an
   unimplemented incompatible feature, which is the correct treatment: it moves
   the descriptor table, and this kernel would read it from the wrong place.
8. **The block size may not be smaller than the device's.** A 1024-byte
   filesystem upon a 4096-byte device is a rearrangement this kernel does not
   perform.
