<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# EXT2: Directories, Files and Names

**Phase**: sub-tasks 5.4 to 5.7 of [`../project/PLAN.md`](../project/PLAN.md).
**Source**: [`../../kernel/fs/ext2/directory.c`](../../kernel/fs/ext2/directory.c),
[`path.c`](../../kernel/fs/ext2/path.c), [`file.c`](../../kernel/fs/ext2/file.c),
[`alloc.c`](../../kernel/fs/ext2/alloc.c), [`name.c`](../../kernel/fs/ext2/name.c).
**Specifications**: Dave Poirier, *The Second Extended File System: Internal
Layout* (directory entries, Tables 4.1–4.3; the bitmaps; symbolic links);
IEEE Std 1003.1-2017 (path resolution, `SYMLOOP_MAX`).

What is done with the structures of [`EXT2.md`](EXT2.md): reading directories and
resolving paths through them, reading files, writing and truncating them, and
creating and destroying names. Every limitation of the EXT2 support is listed in
[`EXT2.md`](EXT2.md).

## 1. Directories

A directory is an ordinary file whose data is a sequence of entries, each joining
a name to an inode number. Its blocks are resolved like any file's; the work here
is reading those bytes. Because names live only in directories, one file may have
several names, and removing a name need not remove the file. The root is inode 2.

### 1.1 The entry (Table 4.1)

| Offset | Width | Field |
| ------ | ----- | ----- |
| 0 | 4 | `inode`; **0 means the record is unused** |
| 4 | 2 | `rec_len`: distance to the next record |
| 6 | 1 | `name_len` |
| 7 | 1 | `file_type` (Table 4.2) |
| 8 | 0–255 | `name`, unterminated |

The records of a block form a linked list: each states the distance to the next,
not its own length. A removed record is absorbed by lengthening the one before
it; the first record of a block, having none before it, is marked unused (inode
0) and left where it is. The last record of a block runs to the block's end, and
the next block starts a new list. Records are 4-byte aligned, `rec_len` is at
least the record's size, and **no record spans two blocks**.

### 1.2 The two readings of offset 6

Revision 0 stored a 16-bit name length at offset 6. The high byte was always zero
and was later reclaimed as the file type. Which reading applies is stated **only**
by `EXT2_FEATURE_INCOMPAT_FILETYPE`, not by the revision. Read wrongly, `.` (name
length 1, type directory) becomes a name of 513 bytes, or every file on an older
volume becomes of no type. This is why the file type is an *incompatible*
feature. The kernel decides from the flag alone.

### 1.3 File type and inode format

The entry's type numbering is unrelated to the format bits of `i_mode`:

| Entry type | Value | `i_mode` format |
| ---------- | ----- | --------------- |
| `EXT2_FT_UNKNOWN` | 0 | — |
| `EXT2_FT_REG_FILE` | 1 | `0x8000` |
| `EXT2_FT_DIR` | 2 | `0x4000` |
| `EXT2_FT_CHRDEV` | 3 | `0x2000` |
| `EXT2_FT_BLKDEV` | 4 | `0x6000` |
| `EXT2_FT_FIFO` | 5 | `0x1000` |
| `EXT2_FT_SOCK` | 6 | `0xC000` |
| `EXT2_FT_SYMLINK` | 7 | `0xA000` |

`Ext2FileTypeOfMode` writes the correspondence out, and every path resolution
checks that the entry agrees with the inode it names: the two are written at
different times by different code, and disagreement means the directories and the
inodes describe different filesystems.

### 1.4 Traversal

`Ext2DirectoryNext` advances a cursor (block index, offset) and yields one entry
at a time, passing over:

1. **A record naming inode 0.** It holds space, not a name; reading its name
   would report a deleted file. The interior nodes of an indexed directory are
   disguised this way, which is why a linear traversal reads such a directory
   correctly.
2. **A hole.** It reads as zeroes, and a record length of zero cannot be advanced
   past.
3. **The tail of a block**, never seen: the last record's length carries the
   cursor to the end.

A directory is the first variable-length structure: an entry is as long as its
record length says. One length misread, or one entry advanced by its name length
instead of its record length, and every later entry in the block is read from the
middle of another: fragments of real names, not an obvious error.

### 1.5 Paths

`Ext2ResolvePath` starts at inode 2 and looks up each component in the directory
the previous one named.

- **Absolute paths only.** A working directory belongs to a process; the VFS
  resolves relative paths against it ([`VFS.md`](VFS.md)).
- **Repeated separators are one; a trailing separator asserts a directory.**
- **`.` and `..` are ordinary entries**, present on the volume (the root's `..`
  names the root), so the ordinary lookup resolves them.
- **A component that is not a directory is refused** before it is searched,
  separating "does not exist" from "a file used as a directory".
- **Names match by their whole length and bytes.** The format gives no meaning to
  case or encoding. A component is looked up where it stands in the path, by
  address and length, without copying.

**Symbolic links.** A link is followed by resolving its target to an inode and
continuing the original path from there, which needs no buffer for a spliced path.
A relative target is resolved against the directory holding the link.

| | A link inside the path | A link as the last component |
| --- | --- | --- |
| `Ext2ResolvePath` | Followed | Followed |
| `Ext2ResolvePathNoFollow` | Followed | Returned as is |

A trailing separator overrides the second column: `/link/` asks for a directory,
so the link is followed. At most `EXT2_SYMLINK_DEPTH_MAXIMUM` (8) links are
followed per resolution; a link naming itself is a valid file, and only a bound
terminates it.

### 1.6 What is refused

| Refused | Why |
| ------- | --- |
| A record length below 8, not a multiple of 4, or reaching past its block. | A cursor that cannot advance, misaligns the rest, or claims another block. |
| A name longer than its record less 8, or than 255 bytes. | Read out of the next record; the second is the sign of the wrong offset-6 reading. |
| An in-use record naming an inode beyond the volume, or bearing no name. | A name leading nowhere; a name that cannot be looked for. |
| A name containing `/` or a null byte. | Reachable by no path, or equal to its own prefix. The format allows it; this kernel cannot address it correctly. |
| A block whose records stop short of its end by 8 bytes or more. | A record length that stops short, stranding what follows. |
| A directory whose size is zero or not a whole number of blocks. | Every directory holds `.` and `..`; the last block would end mid-record. |
| An inode that is not a directory, read as one. | Caught before its bytes are read as entries. |
| An entry whose type contradicts the inode's format. | Section 1.3. |
| A relative path, or one longer than 4,096 bytes. | Nothing to resolve against; an unterminated string walked until it faults. |
| More than 8 links in one resolution, or a link whose target cannot be read. | A cycle; a target read by the rules of Section 2. |

## 2. Reading a file

A read serves one block per turn:

```
index  = offset / block_size
within = offset % block_size
take   = min(block_size - within, remaining)
```

`take` makes the first and last blocks partial and the rest whole without three
cases. The index goes to `Ext2InodeBlock` ([`EXT2.md`](EXT2.md)).

- **A hole reads as zeroes.** That is what the file contains there.
- **The end of the file is not a failure.** A read crossing it is shortened; a
  read at or past it returns zero bytes and succeeds. Every reader reaches the
  end, and an error there would be indistinguishable from an unreadable volume.
- **A directory is refused** as a stream: its bytes are records.

**Symbolic links** store their target in one of two places:

| Form | Target | Read |
| ---- | ------ | ---- |
| Fast (target under 60 bytes) | The 60 bytes of `i_block` | From the fifteen decoded words, little-endian |
| Slow | Data blocks | As any file |

The form is decided by `i_blocks` (less any extended attribute block), not by the
size, as Linux decides it: the two agree unless the inode carries an attribute
block, which `i_blocks` counts. `Ext2ReadInode` does not check a fast link's
`i_block` words as block numbers, because they are text (`sub` read as a pointer
is block `0x00627573`); the decision rests on the mode, sector count and attribute
block, all parsed first.

| Refused | Why |
| ------- | --- |
| A target read from an inode that is not a link. | Data or pointers read as a path. |
| A link with no target. | Resolving the empty path reaches whatever is current. |
| A target longer than the caller's buffer. | A truncated path names another file, possibly a real one. |
| A target containing a null byte. | Shorter than the file says. |
| A fast link of more than 60 bytes. | No room for what it claims. |

## 3. Writing

A wrong read returns wrong bytes once. A wrong write destroys data, cannot be
undone, and is usually silent: a block given to two files reads correctly for
both until one writes. Four rules govern every function here.

1. **A read-only volume is not written.** Every function checks first, so the
   judgement of [`EXT2.md`](EXT2.md) is enforced in one place.
2. **A resource is marked used before it is referenced.** If the machine stops in
   between, marking first leaks the block until a check reclaims it; referencing
   first lets the block be allocated again to a second file. A leak is a cost;
   sharing spreads.
3. **Nothing reaches the medium until the cache writes it back.** Writes dirty
   buffers; a caller that needs the medium consistent calls `BufferSync`.
4. **Every structure is read before it is written, and only parsed fields change.**
   Composing a superblock from the parsed structure would zero every field this
   kernel does not parse (journal identifiers, hash seed, orphan list head).
   `Ext2WriteSuperblock` maintains the two free counts, the state and the mount
   count, and writes everything else back as read.

### 3.1 Bitmaps and allocation

One bit per block and per inode of a group, **1 = used**, least significant bit
first within a byte; the inode bitmap starts at inode 1. Setting a set bit or
clearing a clear bit is **refused**: the first means two owners, the second is the
double free that permits two owners.

A block is allocated by finding a clear bit, setting it, decrementing the free
counts of the group and the superblock, and writing all three back.

- **The search is bounded by the group's true extent**, not the bitmap's size;
  the bits beyond a short last group are whatever the volume's maker left.
- **A hint** (usually the file's previous block) chooses the group to search
  first. It keeps a file's blocks together, and it is the whole allocation policy.
- **A group whose count claims free blocks its bitmap lacks** stops the
  allocation: continuing elsewhere leaves the contradiction for the next caller.
- **Inodes below `s_first_ino` are never issued**, even if a volume left one
  clear; otherwise the root could be handed to a file.

### 3.2 Growing a file

`Ext2InodeBlockAllocate` is `Ext2InodeBlock` with holes filled: where it finds
zero it allocates, zeroes a new pointer block, and writes the pointer back. It
zeroes:

1. **Every new pointer block**; otherwise its old contents are read as pointers
   to other files' blocks.
2. **A new data block the write does not wholly cover**; otherwise the rest shows
   the previous owner's data. It therefore **reports whether it allocated**,
   rather than leaving the caller to infer it.

A write that starts beyond the end leaves a hole: a sparse file.

### 3.3 Truncation

Downward, every block beyond the new size is freed, and every pointer block left
empty. A subtree wholly below the new size is kept without being walked, so
trimming one block from a large file does not read all its indirection. Upward,
nothing is allocated: the file grows by a hole.

### 3.4 Crash consistency

Allocating one block touches the bitmap, the descriptor, the superblock and the
inode. They cannot be made atomic without a journal, which EXT2 lacks (ext3 is
EXT2 with one). Rule 2 does not remove the window; it chooses the side that leaks
over the side that corrupts. Recovery is `e2fsck`.

| Refused | Why |
| ------- | --- |
| Any write to a read-only volume. | Rule 1. |
| Setting a set bit or clearing a clear one. | Two owners; a double free. |
| Freeing a block or inode outside the volume, or an inode below `s_first_ino`. | Stray arithmetic; inode 2 is the root. |
| Allocating where a group's count and bitmap disagree, or none is free. | Self-contradiction; refused before searching. |
| Writing or truncating a directory as bytes. | Its records are a structure (Section 4). |
| A block index beyond the triply indirect range. | Past the decomposition. |

## 4. Names

### 4.1 Insertion

A record is longer than its name needs when a later record was removed or it is
the last of its block. A name of *n* bytes needs `round_up(8 + n, 4)`; each block
is searched for either an unused record long enough (taken whole) or a used record
with enough excess (shortened, with the new record in the remainder). Only when no
block has room is a block added, holding one record spanning it. Nothing is ever
moved, so creating and removing one name repeatedly reuses the same space.

**A name already present is refused**: two records with one name make the path
ambiguous.

### 4.2 Removal

The record before is lengthened to cover it; a first record is marked unused in
place (Section 1.1), which is why traversal must skip unused records rather than
read the names still in them. `.` and `..` are never removed: without them every
path through the directory fails.

### 4.3 Directories and link counts

A new directory needs all three, or it is not a directory:

1. `.` naming itself and `..` naming its parent, the second running to the end of
   the block.
2. A link count of **two** (its `.` and its parent's entry).
3. The parent's link count **raised by one** for the child's `..`.

The third is easy to omit and invisible: a parent short by one may be freed while
a child still names it. `e2fsck` Pass 4 checks it. The parent is checked against
`EXT2_LINK_MAXIMUM` before anything is allocated, so a refusal leaves nothing
half-made.

A directory is removed only when it holds nothing but `.` and `..`; its block and
inode are freed and the parent's count lowered. The root is never removed.

### 4.4 Destroying a file

`i_links_count` counts the names leading to a file; the file is destroyed only
when it reaches zero. The name is removed first (stopping afterwards leaks an
inode) and then the blocks before the inode (freeing the inode first would leak
blocks with no record of which).

- A **fast symbolic link** is not truncated: its `i_block` is text, not pointers.
- A **directory** may not be unlinked as a file or given a second name: two paths
  to one directory make a cycle in a structure that must be a tree.
- A destroyed inode keeps its mode and pointers (recovery tools read them); only
  `i_dtime` and the zero link count mark it, and `Ext2ReadInode` refuses it.
- **`i_dtime` is `EXT2_DELETION_TIME_UNKNOWN` (`UINT32_MAX`).** `e2fsck` reads a
  value below `s_inodes_count` as the next inode of the orphan list, not a time; a
  small constant makes every freed inode look like a corrupt orphan list. No
  volume has an inode that high, and it is the last second the field can express.

| Refused | Why |
| ------- | --- |
| A name already present. | An ambiguous path. |
| A name of no length, over 255 bytes, or containing `/` or a null. | Unreachable or ambiguous. |
| A name for inode 0 or beyond the volume. | Leading nowhere. |
| Removing `.`, `..`, or a name not present. | The directory loses itself; a caller told it removed something. |
| `Ext2CreateFile` making a directory, or a file with no format. | No `.`/`..` and a wrong parent count; an inode that looks unfilled. |
| A second name for a directory; unlinking one as a file; `Ext2RemoveDirectory` on a file. | A cycle; invariants left unmaintained. |
| Removing a non-empty directory, or the root. | Files reachable by no path; nowhere for paths to begin. |
| Any of these on a read-only volume. | Rule 1. |

## Verification

The chapters of [`../../kernel/test/storage/ext2/`](../../kernel/test/storage/ext2/)
run against a volume composed in memory. Directories and files are asserted entry
by entry and byte by byte, because their failures produce plausible data. Writes
are asserted against the volume read back, not the structure in memory: an
allocator that updated only its own copy would satisfy every in-memory assertion.

`KernelVerifyDirectories`:

| Property asserted | The failure it would catch |
| ----------------- | -------------------------- |
| The root yields `.`, `..`, `file`, `sub`, each with its inode, type, block and offset. | Advancing by name length, reading every later entry from the middle of another. |
| The unused record between `file` and `sub` is skipped and its name not found. | A deleted file reported. |
| The final record ends the traversal. | Padding read as an entry. |
| `fil` and `files` do not match `file`, nor `file` with length 3. | A comparison stopping at the shorter name. |
| Twelve paths resolve, including `/`, `///`, `/.`, `/..`, `/sub/..`, `/sub/../file`, `//sub///inner`. | Errors in the component walk. |
| Eight paths are refused, including a relative path, a missing component, and a file used as a directory. | A path resolving to something wrong. |
| Each refusal of Section 1.6 refuses; an entry typed directory for a regular file is refused. | A malformed directory walked. |
| The same bytes are refused under the 16-bit reading and accepted under the 8-bit one, by the flag alone. | The offset-6 confusion. |

`KernelVerifyFiles` (the composed file holds at each offset a byte derived from
that offset, so a read of the wrong block cannot pass):

| Property asserted | The failure it would catch |
| ----------------- | -------------------------- |
| A 1,500-byte file reads whole across its block boundary. | A short range; a wrong second block. |
| A run from 1,000 to 1,100 is right on both sides of 1,024. | A run taken from one block. |
| A run inside the second block is right. | File and block offsets confused. |
| A read crossing the end is shortened; at or past it, zero bytes and success; a zero-length read, zero bytes. | End of file reported as an error. |
| In the sparse file, block 12 is data and block 13 zeroes. | Zeroes or data for both. |
| A directory is refused as a file. | Records returned as text. |
| Fast and slow links are recognised and their targets read exactly; a too-long target is refused. | Half the link code untested; a truncated path. |
| Five paths resolve through links, relative and absolute, mid-path; the no-follow form behaves per Section 1.5; a self-naming link is refused. | Re-entry errors; wrong base directory; unbounded recursion. |

`KernelVerifyWrites`:

| Property asserted | The failure it would catch |
| ----------------- | -------------------------- |
| Both bitmaps read as composed, including the one free inode. | A bit index off by one. |
| An allocated block is used and the counts fall by one in memory and on the volume; freeing restores all three. | Accounting never written back. |
| A double free is refused, for a block and for an inode; freeing a reserved inode is refused. | Two owners; the root handed out. |
| The one free inode is allocated, a second allocation refused, and a free restores it. | Exhaustion mistaken for success. |
| A write inside a file leaves the bytes around it untouched. | A write covering more than it was given. |
| Truncating to nothing returns exactly the file's blocks, and rewriting takes exactly them back. | Any leak or double count. |
| A write past the end leaves a hole reading zeroes; truncating upward allocates nothing. | A sparse file that is not sparse. |
| A write into an empty doubly indirect entry allocates two blocks. | A level skipped. |
| `Ext2VerifyGroupDescriptors` still passes; a read-only volume refuses every write. | A volume that no longer accounts for itself. |

Between sequences the cache is emptied **before** the volume is recomposed:
`BufferInvalidateDevice` writes dirty buffers back, so the other order would flush
the finished test's writes over the new volume.

`KernelVerifyDirectoryWrites` traverses the whole directory after each change,
because a damaged list is invisible in the operation that damaged it:

| Property asserted | The failure it would catch |
| ----------------- | -------------------------- |
| After an insertion the directory yields one entry more, and the name resolves. | A bad split, stranding or overlapping later records. |
| A duplicate name is refused. | Two files by one path. |
| After removal the directory yields what it did before. | A join that lost or duplicated a record. |
| Removing a missing name, `.` or `..` is refused. | A false success; a directory without its parent. |
| 64 insertions and removals of one name use no blocks. | Slack never reused. |
| A created file has one link and no size, and can be written and resolved. | An inode allocated but not linked. |
| A second name raises the count; removing one of two keeps the file. | An unlink destroying a file another name reaches. |
| Removing the last name frees inode and blocks, and the inode is then refused. | A freed inode still reachable. |
| A created directory has two links, its parent gains one, and `/made/.`, `/made/..`, `/made/../made` resolve. | The link-count error; a wrong `..`. |
| A non-empty directory is not removed, unlinked or given a second name; an empty one is removed and the parent's count restored; the root is not removed. | Unreachable files; cycles; Pass 4 errors. |
| Free counts return to their start, the root lists as before, and the descriptors verify; a read-only volume refuses all of it. | Anything leaked across the sequence. |

## Limitations

Listed once for all EXT2 support, in [`EXT2.md`](EXT2.md).
