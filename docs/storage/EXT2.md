# The EXT2 Volume and its Structures

**Phase**: 5, sub-tasks 5.1 to 5.3, of [`../project/PLAN.md`](../project/PLAN.md)
— the superblock, the block group descriptor table and the inode, which are what
every operation upon a volume acts through.

**The other two documents.** This one was 1,562 lines and is now three:

| Document | Subject |
| -------- | ------- |
| **`EXT2.md`** (this one) | The format itself: why the superblock is read first, its byte order and its fields, the two revisions, the three classes of feature flag, what a volume is refused for, the block group descriptor table, and the inode. **Section 10 enumerates every limitation of this kernel's EXT2 support**, the other two documents' included. |
| [`EXT2-FILES.md`](EXT2-FILES.md) | What is done with those structures: the directory and the resolution of a path through it, the reading of a file, the writing and truncation of one, and the creation and destruction of names. |
| [`EXT2-VERIFICATION.md`](EXT2-VERIFICATION.md) | The eleven self-tests, and the six of them that assert against a volume `mke2fs` produced rather than one this kernel composed. |

The limitations were **not** divided among the three. They are properties of the
format and of this kernel's handling of it as a whole — several bear upon two
documents at once and one refers to another by number — and a reader assembling
them from three lists would be doing the work the list exists to have done for
them.

**Authority**: `PROJECT_GUIDELINES.md`, Sections 2, 3 and 6. Every assertion about
the format carries a citation, and the specifications are registered in
[`../project/REFERENCES.md`](../project/REFERENCES.md).

**Implementation**: [`../../kernel/fs/ext2/`](../../kernel/fs/ext2/), which holds
nine translation units and the private header between them:
[`core.c`](../../kernel/fs/ext2/core.c) (the shared state, the byte order and the
block-level transfer),
[`superblock.c`](../../kernel/fs/ext2/superblock.c),
[`group.c`](../../kernel/fs/ext2/group.c),
[`inode.c`](../../kernel/fs/ext2/inode.c),
[`file.c`](../../kernel/fs/ext2/file.c),
[`alloc.c`](../../kernel/fs/ext2/alloc.c) (the two bitmaps),
[`directory.c`](../../kernel/fs/ext2/directory.c) (the record and its traversal),
[`path.c`](../../kernel/fs/ext2/path.c) (a path resolved to an inode),
[`name.c`](../../kernel/fs/ext2/name.c) (the names, and the files they reach), and
[`internal.h`](../../kernel/fs/ext2/internal.h).
The public interface is
[`../../kernel/include/oxys/ext2.h`](../../kernel/include/oxys/ext2.h) and is
unchanged by the division.

This was a single file of 4,325 lines until the review that followed sub-task
6.10. The sections of this document correspond to those units and always did —
which is the reason the division was possible without rewriting anything, and the
sign that the file had been several things for some time.
[`../design/ARCHITECTURE.md`](../design/ARCHITECTURE.md), Section 2.2, records
the rule the division establishes.

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

## 9. The inode

An inode describes one file: its format, its permissions, its owner, its times,
its size, and the blocks holding its data. It carries no name. Names live in
directories alone, which is what makes a hard link possible and what makes
sub-task 5.4 a separate piece of work.

### 9.1 Finding one

Inode numbers begin at **one**; indices begin at zero. The group holding an inode
and its index within that group's table are therefore

```
group = (number - 1) / s_inodes_per_group
index = (number - 1) % s_inodes_per_group
```

and the inode lies at `index * s_inode_size` within the table whose first block
the group's descriptor gives. Three pieces of arithmetic, and every plausible
mistake in them — omitting the subtraction, using the block size where the inode
size belongs, taking the group's first block for its inode table — lands upon
some other inode **of the same volume**. That inode is a valid inode. It simply
belongs to a different file, and nothing in the machine can tell.

The superblock has already been made to state an inode size that is a power of
two no larger than a block (Section 7), so a whole number of inodes occupies a
block and no inode straddles two. That is what allows the 128 bytes to be read as
one run.

An inode occupies the **first 128 bytes** of whatever `s_inode_size` states.
A revision 1 volume may state more — `mke2fs` now defaults to 256 — and the bytes
beyond the 128th belong to extensions this kernel does not read.

### 9.2 The size, in two halves

A revision 1 volume keeps the high 32 bits of a **regular file's** size in the
field a revision 0 volume calls `i_dir_acl`, at offset 108. The two halves are
joined here so that nothing above must remember to.

They are joined only for a regular file. Upon a directory the same bytes mean
something else entirely, and a kernel that joined them regardless would give a
directory a size of some gigabytes and read it until it fell off the volume.

### 9.3 Resolving a block of the file

`i_block` holds fifteen block numbers. The first twelve name blocks of the file
directly. The thirteenth names a block of pointers, the fourteenth a block of
pointers to blocks of pointers, and the fifteenth one level deeper again. With a
block size of *B* a block holds *P = B/4* pointers, so the ranges are

| Index range | Reached through |
| ----------- | --------------- |
| 0 to 11 | `i_block` directly |
| 12 to 11 + *P* | the indirect block |
| 12 + *P* to 11 + *P* + *P*² | the doubly indirect block |
| 12 + *P* + *P*² to 11 + *P* + *P*² + *P*³ | the triply indirect block |
| beyond | refused; fifteen pointers cannot address it |

`Ext2InodeBlock` reduces the index by each range it lies beyond, so that what
remains is the offset within the range it lies in, and the level is then the
number of pointer blocks to walk. One loop performs the walk for all three
levels, dividing the offset by the span of one entry at each step; the span at
the deepest level is one block, so the last step indexes directly.

**A zero is a hole, not an end.** In the original implementation a zero entry
terminated the list; in a sparse file it means a block that was never allocated,
which reads as zeroes. A resolver that mistook a hole for the end of the file
would be wrong upon most of the files a system holds. A zero *pointer block* is a
hole occupying the whole subtree beneath it — none of the blocks it would have
named exist — so `Ext2ReadPointer` returns zero for any entry of a table block of
zero, without reading anything, and the same function serves all three levels.

**The index is not checked against the size.** A caller reading a file bounds
itself by the size; a caller walking the blocks a file has allocated does not,
and conflating the two here would prevent the second.

### 9.4 What is refused

| Refused | Why it matters |
| ------- | -------------- |
| Inode 0, or a number beyond `s_inodes_count`. | Zero is not an inode: a directory entry bearing it names nothing, which is how a deleted entry is recorded. |
| An inode whose group's descriptor is refused. | The table it lies in was located by a descriptor that is not trustworthy. |
| Any of the fifteen pointers addressing a block the volume does not hold. | Checked when the inode is read, before any of them is used. |
| A pointer within an indirect block addressing such a block. | Cannot be caught when the inode is read, the block holding it not having been read then, so it is checked where it is fetched. |
| An inode with no format and no links. | A table entry that was never filled. The bytes past the table are zeroes upon a fresh volume, and a kernel that accepted them would report a file of no type and no blocks rather than the mistake that produced it. |
| A block index beyond what fifteen pointers can address. | Arithmetic that has run past the end of the decomposition. |

## 10. Limitations

1. **Discharged in sub-task 5.8.** A volume is mounted, retained and opened; see
   [`VFS.md`](VFS.md). What follows here concerns the format alone.
2. **The backup superblocks are not consulted.** A volume whose primary
   superblock is damaged is refused, though a copy of it — and of the descriptor
   table — stands in several block groups. Nothing yet falls back upon them.
3. **The mount time is still not written**, there being no clock. The state and
   the mount count are: a volume mounted for writing has the bit that says it was
   cleanly unmounted cleared, and the count raised, before anything else is
   written to it, and the bit is set again when the mount is withdrawn. See
   [`VFS.md`](VFS.md), Section 8. `s_mtime` and `s_wtime` are read and never
   brought up to date, so a check cannot say *when* the volume was last opened,
   only that it was and how many times.
4. **Block sizes above 4096 bytes are refused**, and fragments are not
   implemented at all — no EXT2 implementation has ever used them.
5. **One volume per device.** Partition tables are not read, so a volume must
   begin at the start of its device.
6. **Nothing here is opened.** The operations of this document act upon a file
   by naming it: there is no descriptor and no position that advances. Both are
   supplied by the layer above, in [`VFS.md`](VFS.md), Section 7; they are
   deliberately not properties of the format, an open file being a property of
   the kernel that has the file open.
7. **The extended fields of a large inode are not read.** Only the first 128
   bytes of an inode are decoded, whatever `s_inode_size` states. The
   nanosecond times and the extended attributes that a 256-byte inode carries
   are not available.
8. **`i_faddr`, `i_osd1` and `i_osd2` are not read.** Fragments were never
   implemented by any EXT2, and the operating-system dependent fields hold the
   high halves of the user and group identifiers upon Linux, which this kernel
   has no use for until it has users.
9. **Discharged in sub-task 5.6.** The bitmaps are read, and blocks and inodes
   are allocated from them and returned to them; see [`EXT2-FILES.md`](EXT2-FILES.md), Section 3.2.
10. **`META_BG` is not implemented.** A volume declaring it is refused as an
   unimplemented incompatible feature, which is the correct treatment: it moves
   the descriptor table, and this kernel would read it from the wrong place.
11. **The block size may not be smaller than the device's.** A 1024-byte
   filesystem upon a 4096-byte device is a rearrangement this kernel does not
   perform.
12. **A directory never shrinks.** A block added to a directory is never given
   back, even when every name in it has been removed; the space becomes slack
   that a later insertion reuses ([`EXT2-FILES.md`](EXT2-FILES.md), Section 4.1). Reclaiming it would mean
   discovering that a block holds nothing but unused records and removing it
   from the middle of the file, which is work `e2fsck -D` exists to do offline
   and which no EXT2 implementation does while running.
20. **Nothing is renamed.** A name may be created and removed, so a rename is two
   operations with a window between them in which the file has two names or
   none. An atomic rename is what `rename()` promises and is not offered here.
21. **The times are not maintained.** `i_atime`, `i_mtime` and `i_ctime` are
   written as they stand and never brought up to date, there being no clock this
   kernel can convert to the seconds since 1970 that the format wants. A file
   this kernel creates bears a time of zero.
22. **The owner is not recorded.** A created file is given uid and gid 0, there
   being no users until Phase 6 and nothing to take them from.
18. **A sequence of writes is not atomic.** Allocating one block touches four
   structures, and a machine that stops partway leaves the volume inconsistent;
   see [`EXT2-FILES.md`](EXT2-FILES.md), Section 3.6. This is what a journal provides and what EXT2 does not have.
   The recovery is `e2fsck`.
19. **Allocation does not preallocate.** A file written a block at a time takes
   one block at a time, where `s_prealloc_blocks` exists precisely so that an
   implementation may take several and reduce fragmentation. The field is read
   and ignored.
13. **A symbolic link's target may not exceed 255 bytes**, and at most eight are
   followed in resolving one path. Neither bound is a property of the format,
   which limits a target only by the size of the file holding it and says
   nothing about following one. Both are bounds upon this kernel: a link is
   followed by re-entering the resolver, so each is a stack frame carrying a
   target buffer of its own. Eight is the depth POSIX requires an
   implementation to allow; 255 accommodates every target a system is likely to
   hold and is not the 4096 a path may reach.
14. **The hash index of an indexed directory is not used.** A directory that
   carries one is read as the linked list it also is: the index is a
   *compatible* feature precisely because its interior nodes are disguised as
   records naming inode 0, which this traversal passes over. Every lookup is
   therefore linear in the entries of the directory, which is what the index
   exists to avoid, and a directory of many thousands of names is searched a
   record at a time. Correct, and slow in exactly the case the format provides
   for.
15. **Relative paths are not resolved** from outside, there being no working
   directory to resolve them against until there are processes in Phase 6. A
   relative *symbolic link target* is resolved, against the directory holding
   the link, that directory being known. The layer above inherits the limitation
   rather than remedying it; see [`VFS.md`](VFS.md), Section 12.1.
16. **A read is not cached above the buffer cache**, and there is no read-ahead.
   Each read resolves its block pointers afresh, so reading a large file
   sequentially re-walks the indirect blocks for every block of it. They will be
   in the buffer cache, so the cost is the walk and not the disk; it becomes
   worth addressing when something reads a large file often, which is not before
   Phase 7.
17. **`i_atime` is not updated by a read.** There is no clock to set it from, so
   a file this kernel reads does not record that it was read. The same holds of
   `i_mtime` upon a write; see limitation 21.

23. **`s_last_orphan` is neither read nor written**, so this kernel keeps no list
   of files whose last name has gone while something still holds them open. That
   is why the layer above refuses to unlink a file that is open rather than
   keeping it alive until the last descriptor closes; see [`VFS.md`](VFS.md),
   Section 9.3. The field is preserved across every write, `Ext2WriteSuperblock`
   reading the superblock before altering the fields this kernel maintains.
