# The EXT2 Volume

**Phase**: 5, sub-tasks 5.1 to 5.4, of [`../project/PLAN.md`](../project/PLAN.md).

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

## 10. The directory

An inode describes a file entirely without naming it. A **directory** is what
supplies the name: an ordinary file, with an ordinary inode and ordinary blocks,
whose data happens to be a sequence of entries associating a name with an inode
number. Nothing in the format treats a directory's blocks specially — they are
resolved by the same `Ext2InodeBlock` of Section 9.3 — and the whole of the work
here is the interpretation of the bytes those blocks hold.

That separation is not incidental. It is why one file may bear several names,
why removing a name need not remove the file, and why a name is a property of
the directory that holds it rather than of the file it leads to.

The root directory is inode 2, which the format reserves for it (Table 3.14).

### 10.1 The entry

Table 4.1 gives the record:

| Offset | Width | Field |
| ------ | ----- | ----- |
| 0 | 4 | `inode` — the inode this name leads to. **Zero means the record is not in use.** |
| 4 | 2 | `rec_len` — the displacement from the start of this record to the start of the next. |
| 6 | 1 | `name_len` — the bytes of name that follow. |
| 7 | 1 | `file_type` — the format of the file, as Table 4.2 numbers it. |
| 8 | 0–255 | `name` — not terminated. |

The entries of one block form a **linked list**, each stating the displacement
to the next rather than its own length. That is what allows a name to be removed
without moving anything: the record before it absorbs its space by having its own
`rec_len` lengthened, and the space is reclaimed when something else is inserted.
Where the first record of a block is removed there is no record before it to
lengthen, so the record remains where it is with its inode number set to zero and
everything else about it — its name included — untouched.

The list runs to the **end of the block and no further**. The last record of a
block states the displacement to the end of that block rather than stopping after
its name, and the next block begins a new list. The specification states this as
three rules: records are aligned upon four bytes, `rec_len` is at least the
length of the record it describes, and **no record may span two blocks**.

### 10.2 The two readings of offset 6

This is the one place in the format where the same two bytes have two lawful
meanings and where reading the wrong one produces no diagnostic of its own.

Revision 0 held a **sixteen-bit** name length at offset 6. Since no
implementation ever permitted a name beyond 255 bytes the upper byte was always
zero, and it was later reclaimed as the file type. Which reading applies is
stated by `EXT2_FEATURE_INCOMPAT_FILETYPE` and by **nothing else** — not by the
revision, a revision 1 volume being free to omit the feature.

This is also the reason the file type is an *incompatible* feature rather than a
compatible one. The Linux documentation puts it plainly: a kernel unaware of it
"would think a filename was longer than 256 characters". Concretely, the entry
`.` bears a name length of 1 and a file type of `EXT2_FT_DIR`; read as one
sixteen-bit quantity those two bytes are `1 + 256 × 2 = 513`, a name that cannot
fit in a record of twelve bytes. Read the other way about, every entry of a
volume that states no file type acquires a type equal to the high byte of its
name length, which is zero, so every file is declared to be of no type.

This kernel decides from the flag alone, and the self-test of Section 11.5
asserts that it does so by presenting the same bytes under both readings.

### 10.3 The file type, and the format it must agree with

Table 4.2 numbers the types, and the numbering is **unrelated** to the file
formats `i_mode` holds in its high four bits:

| Entry type | Value | `i_mode` format |
| ---------- | ----- | --------------- |
| `EXT2_FT_UNKNOWN` | 0 | — |
| `EXT2_FT_REG_FILE` | 1 | `EXT2_S_IFREG`, `0x8000` |
| `EXT2_FT_DIR` | 2 | `EXT2_S_IFDIR`, `0x4000` |
| `EXT2_FT_CHRDEV` | 3 | `EXT2_S_IFCHR`, `0x2000` |
| `EXT2_FT_BLKDEV` | 4 | `EXT2_S_IFBLK`, `0x6000` |
| `EXT2_FT_FIFO` | 5 | `EXT2_S_IFIFO`, `0x1000` |
| `EXT2_FT_SOCK` | 6 | `EXT2_S_IFSOCK`, `0xC000` |
| `EXT2_FT_SYMLINK` | 7 | `EXT2_S_IFLNK`, `0xA000` |

Nothing about either numbering derives from the other, so `Ext2FileTypeOfMode`
writes the correspondence out. The specification requires the two to **agree**,
and this kernel checks that they do wherever it resolves a path: the entry and
the inode are written at different times by different code, and a volume upon
which they disagree is one whose directories and inodes no longer describe the
same filesystem. The check costs nothing there, the inode having just been read.

### 10.4 Traversal

`Ext2DirectoryNext` advances a cursor — a block index and an offset within it —
and produces one entry at a time. Three things are passed over rather than
produced:

1. **A record naming inode 0.** It holds space and not a name. A traversal that
   read its name rather than its inode number would report a file that was
   deleted. It is also how the interior nodes of an indexed directory are
   disguised, which is why a linear traversal reads such a directory correctly;
   see Section 12, limitation 14.
2. **A block the directory never had allocated.** Reading a hole yields zeroes,
   and a record length of zero cannot be advanced past; passing over the block is
   both the correct reading of a hole and the only one that terminates.
3. **The padding after the last name of a block**, which is not passed over so
   much as never seen: the last record's length carries the cursor to the end of
   the block, and the next block begins a new list.

A directory is the first structure of the volume whose contents are **variable
rather than fixed**. A superblock lies at a known offset, a descriptor is 32
bytes and an inode is 128; an entry is as long as its record length says, and the
next one begins wherever that lands. Every mistake in reading it is therefore
self-propagating — one record length taken from the wrong offset, or one entry
advanced by the length of its name rather than by its record length, and every
entry after it in the block is read from the middle of something else. What comes
out is not obviously wrong. It is fragments of real names, and a lookup that
fails to find a file that is there is indistinguishable from a file that is not.

### 10.5 Resolving a path

`Ext2ResolvePath` begins at inode 2 and takes the components of the path in
turn, looking each up in the directory the previous one named.

- **Only an absolute path is resolved.** A relative one is resolved against a
  working directory, which is a property of a process and not of a volume, and
  there are no processes until Phase 6.
- **Repeated separators are one**, and a **trailing separator asserts that what
  the path names is a directory**: `/sub/` names a directory or it names nothing.
- **`.` and `..` are not treated specially.** Every EXT2 directory holds them
  upon the volume as ordinary entries, the `..` of the root naming the root
  itself, so the ordinary lookup resolves them. A kernel that interpreted them
  here would be second-guessing the volume rather than reading it.
- **A component that is not a directory is refused** before it is searched, which
  distinguishes the two failures a caller cares about: a path whose components do
  not exist, and a path that treats a file as though it were a directory.
- **A name is matched by its whole length.** Names are compared by their bytes
  and their length alone; the format attributes no meaning to case, to an
  encoding, or to any character but the separator.

The lookup is given the address and the length of a component rather than a
terminated string, so one component of a path is looked up **where it stands**
without first being copied into a buffer of its own.

### 10.6 What is refused

| Refused | Why it matters |
| ------- | -------------- |
| A record length below eight. | The header itself does not fit, and a cursor cannot be advanced past it — the traversal would not terminate. |
| A record length that is not a multiple of four. | Every record after it in the block is left unaligned. |
| A record reaching beyond the block that holds it. | Contradicts the rule that no entry spans two blocks; the bytes it would claim belong to another block entirely. |
| A name longer than the record less eight. | The name would be read out of the record that follows. |
| A name longer than 255 bytes. | Reachable only under the sixteen-bit reading, and the sign that the wrong reading is in use. |
| An in-use record naming an inode beyond `s_inodes_count`. | A name leading nowhere. |
| An in-use record bearing no name. | A name that cannot be looked for. |
| A name holding the separator or a null byte. | Reachable by no path, and equal to its own prefix once terminated. The format permits such a name; this kernel cannot address it correctly, and saying so is better than resolving a path to the wrong file. |
| A block whose records leave fewer than eight bytes unaccounted for at its end. | Something wrote a record length that stops short, and the entries beyond it are unreachable. |
| A directory whose size is not a whole number of blocks. | Its final block ends in the middle of a record. |
| A directory of no size. | Every directory holds at least its own entry and its parent's. |
| An inode that is not a directory. | Caught before its bytes are interpreted as entries. |
| An entry whose file type contradicts the format of the inode it names. | The directories and the inodes no longer describe the same filesystem. |
| A relative path, or one longer than 4096 bytes. | The first has nothing to be resolved against; the second is how a string that was never terminated is refused rather than walked until it meets something that faults. |
| A symbolic link standing within a path. | Following it means reading the file its data is, which is sub-task 5.5. Met as the last component it is returned as it stands, the caller having asked for the link and not for what it names. |

## 11. Verification

### 11.1 The self-test of the superblock

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

### 11.2 The self-test of the descriptor table

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

### 11.3 The self-test of the inodes

The composed volume carries an inode table of its own. Inode 2 is the root
directory, as the format reserves it, with a single direct block. Inode 11 is a
regular file whose fifteen pointers reach every level of the indirection: twelve
direct blocks, an indirect block whose first and last entries are used and whose
second is a hole, a doubly indirect block, and a triply indirect block. Every
block it names lies within the 128 blocks the volume holds.

| Property asserted | The silent failure it would catch |
| ----------------- | --------------------------------- |
| Inode 2 is a directory of three links and one block, with the permissions composed. | The format bits taken from the wrong end of `i_mode`, or the mode read as a word. |
| Inode 11 is found. It is index 10, and eight inodes of 128 bytes fill a block of 1024, so it lies in the **second** block of the table. | An inode reader that never crosses out of the first block of the table — which passes every assertion about inode 2. |
| Every field of inode 11 equals the value composed at that offset. | A field read from the wrong offset, which yields a plausible number. |
| Indices 0 and 11 resolve to the first and last direct blocks. | The direct range taken as eleven or thirteen entries. |
| Index 12 resolves through the indirect block; index 11 + *P* resolves to its last entry. | The boundary the whole decomposition turns upon; one entry adrift here yields a real block of the volume. |
| Index 13 is a hole, and so is the first index of the doubly indirect range. | A hole mistaken for the end of the file, or for an error. |
| Index 12 + *P* + 5 resolves two levels down; index 12 + *P* + *P*² + 3 resolves three. | A level of the walk dividing by the wrong span. |
| An index whose subtree is absent at the top is a hole, reached without reading any block. | A resolver that refused rather than reporting a hole, or that read a pointer block numbered zero. |
| An index beyond the triply indirect range is refused. | Arithmetic that has run past the end of the decomposition. |
| Inode 0 and an inode beyond the count are refused. | Numbers treated as indices. |
| An inode of the table that was never filled is refused. | The zeroes past the table read as a file. |
| A direct pointer outside the volume is refused when the inode is read; a pointer within an indirect block outside the volume is refused when it is fetched. | Two checks that must both exist, since neither can be performed where the other is. |

### 11.4 The self-test of the directories

`KernelVerifyDirectories` composes a root directory and one subdirectory within
the same device of memory, laid out as Table 4.3 lays out its sample: entries
aligned upon four bytes, an unused record left where a name was removed, and a
final record whose length runs to the end of the block. The entries are written
from the same offset names the parser reads, so a mistaken offset cannot agree
with itself.

The traversal is asserted **entry by entry** against that layout and not merely
counted, for the reason Section 10.4 gives: a traversal that has gone wrong
yields fragments of real names rather than an error.

| Property asserted | The silent failure it would catch |
| ----------------- | --------------------------------- |
| The root yields `.`, `..`, `file` and `sub`, each with its inode number, its file type, and the block and offset it stands at. | An entry advanced by the length of its name rather than by its record length, which would read every entry after the first from the middle of another. |
| The unused record between `file` and `sub` is passed over, and the name upon it is not found. | Reporting a file that was deleted. |
| The final record, whose length runs to the end of the block, ends the traversal. | Reading the padding after the last name as a further entry. |
| A name is matched by its whole length: `fil` and `files` do not match `file`, nor does `file` given a length of three. | A comparison stopping at the shorter of the two, which would resolve a path to the wrong file. |
| Twelve paths resolve to the inodes they name, including `/`, `///`, `/.`, `/..`, `/sub/`, `/sub/..`, `/sub/../file` and `//sub///inner`. | Every arithmetic and boundary error in the component walk. |
| Eight paths that name nothing are refused, including a relative path, a component that does not exist, and a file used as a directory or asserted to be one by a trailing separator. | A path resolving to something, which a caller will then act upon. |
| Each of the refusals of Section 10.6 refuses, the field in question being altered in the composed volume and restored afterwards. | A malformed directory being walked rather than refused. |
| An entry declaring `EXT2_FT_DIR` for an inode that is a regular file is refused. | Directories and inodes describing different filesystems. |
| The same bytes are refused under the sixteen-bit reading and accepted under the eight-bit one, according to the feature flag alone. | The one place in the format where the wrong reading produces no diagnostic of its own. See Section 10.2. |

The cache is invalidated on both sides of every alteration, for the reason
Section 11.1 gives: the bytes are written beneath both the block layer and the
cache, and a cache holding the previous contents would answer with them.

### 11.5 A volume the kernel did not compose

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

The root inode is read and its blocks resolved at every boot, upon every device
the machine carries, which is what puts the inode code of Section 9 against
volumes this kernel did not compose. Two further images were made whose root
directory is large enough to need the indirect blocks, `mke2fs -d` populating
them from a directory of files:

```sh
mke2fs -q -t ext2 -b 1024 -L oxys-root -F -d root/ ext2-dir.img 16384
mke2fs -q -t ext2 -b 1024 -N 16384 -L oxys-big-root -F -d bigroot/ ext2-bigdir.img 65536
```

Of the first, whose root holds 900 entries in 40 blocks, the kernel reported:

```
EXT2 inode 2: mode 0x41ED (directory), 40960 bytes, 3 links, 82 sectors, first block 580.
EXT2 root blocks: 580 616 640 664 688 712 736 760 784 808 832 856 881 ...
```

`debugfs -R "stat <2>"` states the same inode — mode 0755, size 40960, 3 links,
block count 82 — and the same blocks, `(0):580, (1):616, … (11):856, (IND):880,
(12):881`. The thirteenth number is the one that matters: index 12 was reached by
reading the indirect block at 880, which the kernel never sees as a block of the
file and never prints.

Of the second, whose root holds 9000 entries in 500 blocks and therefore reaches
the doubly indirect block:

```
EXT2 inode 2: mode 0x41ED (directory), 512000 bytes, 3 links, 1006 sectors, first block 772.
EXT2 root blocks: 772 786 787 788 789 790 791 792 793 794 795 796 798 ... [268]=1056
```

`debugfs` states `(0):772, (1-11):786-796, (IND):797, (12-267):798-1053,
(DIND):1054, (IND):1055, (268-499):1056-1287`. The prefix matches, and so does
the block at index 268 — which was reached by following the doubly indirect block
at 1054 to the indirect block at 1055 and taking its first entry.


### 11.6 Directories the kernel did not compose

The root directory is now listed at every boot, upon every device the machine
carries, and one path is resolved upon it. The names in that listing were written
by `mke2fs` and not by this project, which is what makes it corroboration rather
than consistency.

An image was made from a populated directory of 46 entries, whose root therefore
occupies two blocks, holding a subdirectory `sub` with a `deeper` beneath it:

```sh
mke2fs -q -F -b 1024 -I 128 -r 1 -d tree/ disk.img 8192
```

The kernel reported:

```
EXT2 inode 2: mode 0x41ED (directory), 2048 bytes, 4 links, 4 sectors, first block 292.
EXT2 root blocks: 292 331
EXT2 entry: inode 2, directory, 12 bytes at block 292 offset 0: .
EXT2 entry: inode 2, directory, 12 bytes at block 292 offset 12: ..
EXT2 entry: inode 11, directory, 20 bytes at block 292 offset 24: lost+found
EXT2 entry: inode 12, regular file, 16 bytes at block 292 offset 44: README
EXT2 entry: inode 13, regular file, 40 bytes at block 292 offset 60: entry-with-a-fairly-long-name-1
...
EXT2 directory 2 holds 46 entries.
EXT2 path /lost+found resolves to inode 11, directory of 12288 bytes.
```

`debugfs -R "ls -l /"` upon the same image lists 46 entries and gives the same
inode number to each name — 11 for `lost+found`, 12 for `README`, 13 for the
first of the long names, and so on in the order shown. The offsets are the ones
the record lengths imply: `.` and `..` occupy twelve bytes each, `lost+found`
twenty, `README` sixteen, and a name of 31 bytes forty. `debugfs -R "stat
<11>"` confirms `lost+found` as a directory of 12288 bytes.

The count is the assertion that matters most. Forty-six entries is not a number
the kernel could reach by accident: it requires every record length in both
blocks to be read correctly, the traversal to cross from block 292 to block 331
at exactly the right point, and the final record of each block to end that
block's list rather than yield an entry.

To exercise path resolution over several components, the probe path was set to
`//sub/deeper/../deeper/buried` for one boot:

```
EXT2 path //sub/deeper/../deeper/buried resolves to inode 56, regular file of 2 bytes.
```

`debugfs -R "stat /sub/deeper/buried"` states inode 56, a regular file of 2
bytes. The path was written with a doubled leading separator and with a `..` that
returns to the directory it came from, so the resolution passed through five
lookups to reach a file three components deep.

A third image was made whose root holds 9000 entries in 530 blocks, which is far
beyond the twelve direct pointers and beyond the 268 an indirect block adds:

```
EXT2 inode 2: mode 0x41ED (directory), 542720 bytes, 3 links, 1068 sectors, first block 448.
EXT2 root blocks: 448 462 463 464 465 466 467 468 469 470 471 472 474 ... [268]=732
EXT2 directory 2 holds 9003 entries.
```

`debugfs -R "ls -l /"` lists 9003 entries — the 9000 files, `.`, `..` and
`lost+found`. The traversal therefore crossed both the direct-to-indirect
boundary at index 12 and the indirect-to-doubly-indirect boundary at index 268
without losing or repeating a single record, which is the integration of
Section 10.4 with the pointer resolution of Section 9.3.
## 12. Limitations

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
6. **Nothing reads a file's contents.** An inode is retrieved, its blocks are
   resolved and a name is turned into an inode number; turning the blocks of a
   file into bytes is sub-task 5.5.
7. **The extended fields of a large inode are not read.** Only the first 128
   bytes of an inode are decoded, whatever `s_inode_size` states. The
   nanosecond times and the extended attributes that a 256-byte inode carries
   are not available.
8. **`i_faddr`, `i_osd1` and `i_osd2` are not read.** Fragments were never
   implemented by any EXT2, and the operating-system dependent fields hold the
   high halves of the user and group identifiers upon Linux, which this kernel
   has no use for until it has users.
9. **The bitmaps are not read.** The descriptor states where the block and
   inode bitmaps of each group begin, and nothing yet looks at them. Until
   sub-task 5.6 nothing is allocated, so nothing needs to know which blocks are
   in use; the free counts are read and believed.
10. **`META_BG` is not implemented.** A volume declaring it is refused as an
   unimplemented incompatible feature, which is the correct treatment: it moves
   the descriptor table, and this kernel would read it from the wrong place.
11. **The block size may not be smaller than the device's.** A 1024-byte
   filesystem upon a 4096-byte device is a rearrangement this kernel does not
   perform.
12. **No directory is written.** Names may be looked up and none may be created
   or removed, which is sub-task 5.7. An entry remembers the block and the
   offset it was read from against that work, since inserting or removing a name
   means altering the record before it.
13. **A symbolic link is not followed.** Met as the last component of a path it
   is returned as it stands; met within a path it is refused. Following one
   means reading the file its data is, which is sub-task 5.5, and doing so
   safely means bounding the depth against a link that names itself.
14. **The hash index of an indexed directory is not used.** A directory that
   carries one is read as the linked list it also is: the index is a
   *compatible* feature precisely because its interior nodes are disguised as
   records naming inode 0, which this traversal passes over. Every lookup is
   therefore linear in the entries of the directory, which is what the index
   exists to avoid, and a directory of many thousands of names is searched a
   record at a time. Correct, and slow in exactly the case the format provides
   for.
15. **Relative paths are not resolved**, there being no working directory to
   resolve them against until there are processes in Phase 6.
