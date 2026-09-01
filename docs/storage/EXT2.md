# The EXT2 Volume

**Phase**: 5, sub-task 5.1, of [`../project/PLAN.md`](../project/PLAN.md).

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

## 8. Verification

### 8.1 The self-test

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

### 8.2 A volume the kernel did not compose

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

Every figure matches `dumpe2fs -h` upon the same image, field for field: the
block and inode counts, both free counts, the geometry of the groups, the inode
size of 256 bytes, and the three feature words — `ext_attr resize_inode dir_index`
as `0x38`, `filetype` as `0x2`, and `sparse_super large_file` as `0x3`.

A second image of 4096-byte blocks was read equally, and reported
`first data block 0` as the format requires of any block size but 1024. A disk
holding no filesystem at all was refused with *the volume bears no EXT2 magic
number*.

## 9. Limitations

1. **Nothing is mounted.** The superblock is read, validated and reported; no
   volume is retained and nothing is opened. Sub-task 5.8 introduces the mount.
2. **The backup superblocks are not consulted.** A volume whose primary
   superblock is damaged is refused, though a copy of it stands in several block
   groups. Using them requires the group descriptors of sub-task 5.2.
3. **Nothing is written.** The mount count and the state are read and not
   updated; a volume this kernel reads does not know it was read.
4. **Block sizes above 4096 bytes are refused**, and fragments are not
   implemented at all — no EXT2 implementation has ever used them.
5. **One volume per device.** Partition tables are not read, so a volume must
   begin at the start of its device.
6. **The block size may not be smaller than the device's.** A 1024-byte
   filesystem upon a 4096-byte device is a rearrangement this kernel does not
   perform.
