<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# The EXT2 Directory and the Operations upon a File

**Corresponding phase**: 5, sub-tasks 5.4 to 5.7 — the directory and the
resolution of a path through it, the reading of a file, the writing and
truncation of one, and the creation and destruction of the names that reach it.

**Authority**: `PROJECT_GUIDELINES.md`, Sections 2 and 6.

**Implemented by**:
[`../../kernel/fs/ext2/directory.c`](../../kernel/fs/ext2/directory.c),
[`../../kernel/fs/ext2/path.c`](../../kernel/fs/ext2/path.c),
[`../../kernel/fs/ext2/file.c`](../../kernel/fs/ext2/file.c),
[`../../kernel/fs/ext2/alloc.c`](../../kernel/fs/ext2/alloc.c),
[`../../kernel/fs/ext2/name.c`](../../kernel/fs/ext2/name.c).

**Asserted by**: `KernelVerifyExt2` and the five chapters of
[`../../kernel/test/storage/ext2/`](../../kernel/test/storage/ext2/), which
[`EXT2-VERIFICATION.md`](EXT2-VERIFICATION.md) sets out.

**Where this sits**: the structures these operations act upon — the superblock,
the block group descriptor table and the inode — are
[`EXT2.md`](EXT2.md), which is also where **every limitation of this
kernel's EXT2 support is enumerated**, this one included. They are kept in one
list because they are properties of the format and of this kernel's handling of
it as a whole, and a reader assembling them from four documents would be doing
work the list exists to have done for them.

---

## 1. The directory

An inode describes a file entirely without naming it. A **directory** is what
supplies the name: an ordinary file, with an ordinary inode and ordinary blocks,
whose data happens to be a sequence of entries associating a name with an inode
number. Nothing in the format treats a directory's blocks specially — they are
resolved by the same `Ext2InodeBlock` of [`EXT2.md`](EXT2.md), Section 9.3 — and the whole of the work
here is the interpretation of the bytes those blocks hold.

That separation is not incidental. It is why one file may bear several names,
why removing a name need not remove the file, and why a name is a property of
the directory that holds it rather than of the file it leads to.

The root directory is inode 2, which the format reserves for it (Table 3.14).

### 1.1 The entry

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

### 1.2 The two readings of offset 6

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

This kernel decides from the flag alone, and the self-test of [`EXT2-VERIFICATION.md`](EXT2-VERIFICATION.md), Section 5
asserts that it does so by presenting the same bytes under both readings.

### 1.3 The file type, and the format it must agree with

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

### 1.4 Traversal

`Ext2DirectoryNext` advances a cursor — a block index and an offset within it —
and produces one entry at a time. Three things are passed over rather than
produced:

1. **A record naming inode 0.** It holds space and not a name. A traversal that
   read its name rather than its inode number would report a file that was
   deleted. It is also how the interior nodes of an indexed directory are
   disguised, which is why a linear traversal reads such a directory correctly;
   see [`EXT2.md`](EXT2.md), Section 10, limitation 14.
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

### 1.5 Resolving a path

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

#### Following a symbolic link

Sub-task 5.4 refused a link standing within a path and returned one standing at
the end unresolved, following one requiring the file reading that did not then
exist. It exists now (Section 2.4), and both are resolved.

A link is followed by **resolving its target to an inode and continuing the
original path from that point** — not by splicing the target into the path and
starting again. The two are equivalent, and this one needs no buffer to hold the
spliced path, which matters when each level of the resolution already carries a
target of its own.

A relative target is resolved against **the directory holding the link**, which
is the whole of the difference between a relative target and an absolute one, and
the reason the resolver must be able to begin somewhere other than the root.

Two entry points are offered, and the distinction is the one POSIX draws between
acting upon a file and acting upon its name:

| | A link within the path | A link as the last component |
| --- | --- | --- |
| `Ext2ResolvePath` | Followed | Followed |
| `Ext2ResolvePathNoFollow` | Followed | Returned as it stands |

A trailing separator overrides the second column. `"/link/"` asserts a directory,
and a link is not one, so the path is asking for what the link names whichever
entry point was called.

At most `EXT2_SYMLINK_DEPTH_MAXIMUM` links are followed in one resolution. The
format offers no protection against a link naming itself and cannot: such a link
is a valid file whose contents happen to be its own name. Eight is the depth
POSIX requires an implementation to allow.

### 1.6 What is refused

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
| More than `EXT2_SYMLINK_DEPTH_MAXIMUM` symbolic links followed in one resolution. | A link naming itself, directly or around a cycle. The format permits it, and nothing but a depth bound terminates the resolution. |
| A symbolic link whose target cannot be read. | Refused by the rules of Section 2.5 rather than followed to somewhere else. |

## 2. Reading a file

Everything to this point **locates** things: a superblock, a descriptor, an
inode, a block of a file, a name within a directory. This is the first section
that produces the contents of a file, and it is the shortest piece of work in
the chapter precisely because the locating was done properly. The whole of it is
the arithmetic of a byte range against a block size, and one call per block to
`Ext2InodeBlock`, which has existed since [`EXT2.md`](EXT2.md), Section 9.3.

### 2.1 The range

A read is given an offset within the file and a length. Each turn of the loop
serves the part of the request lying within one block:

```
index  = offset / block_size          which block of the file
within = offset % block_size          where in that block
take   = min(block_size - within, remaining)
```

`take` is what makes the first and last blocks of a range partial and every
block between them whole, without those three cases being written separately.
The block index goes to `Ext2InodeBlock`, which resolves it through however many
levels of indirection it needs; nothing here knows or cares how many that was.

### 2.2 A hole reads as zeroes

`Ext2InodeBlock` yields zero for a block the file never had allocated. The read
fills that part of the buffer with zeroes rather than refusing.

This is not leniency. The contents of a file at a hole **are** zeroes, by
definition rather than by accident, and a reader cannot distinguish a hole from a
block that was written with zeroes — which is the entire point of one. A kernel
that refused would be unable to read most of the files a system holds.

### 2.3 The end of the file is not a failure

A read that would cross the end of the file is shortened to it. A read beginning
at or beyond the end yields no bytes and **succeeds**.

Every reader arrives at the end of a file; it is how reading concludes. A kernel
reporting it as an error would oblige each of them to treat the ordinary
conclusion of its work as a fault, and would leave them unable to distinguish it
from a volume that could not be read. The count reports it instead, so the return
value means what it says: false is a failure, and zero bytes is the end.

A directory is refused outright. Its bytes are entries, it is read by traversing
it (Section 1.4), and a caller reading it as a stream would receive record
lengths and inode numbers as though they were text.

### 2.4 Symbolic links, and the two places a target lives

A symbolic link is a file whose contents are a path. The specification's
Symbolic Links chapter puts the optimisation plainly: *"For all symlink shorter
than 60 bytes long, the data is stored within the inode itself; it uses the
fields which would normally be used to store the pointers to data blocks."*

Sixty bytes is fifteen pointers of four. So there are two forms:

| Form | Where the target is | How it is read |
| ---- | ------------------- | -------------- |
| Fast | The sixty bytes of `i_block` | Recovered from the fifteen decoded words, least significant byte first |
| Slow | Blocks of the volume | Read as any other file is, by Section 2.1 |

**Which form a link is must be decided by `i_blocks` and not by the size.** The
two agree upon every link a filesystem creates, the one being the reason for the
other; they part when the inode carries an extended attribute block, which
`i_blocks` counts and which is not data. A test upon the size alone would then
read the target out of pointers to a block that exists. The test is therefore
that the sectors the inode declares, less those of an extended attribute block,
are zero — which is how Linux distinguishes them.

#### The defect this exposed in `EXT2.md`, Section 9

`Ext2ReadInode` validated all fifteen words of `i_block` as block numbers,
refusing an inode naming a block the volume does not hold. That check is correct
for every file but a fast symbolic link, where those words are **text**. The
target `sub` read as a pointer is the word `0x00627573` — a block some millions
beyond the end of any volume — so every fast symbolic link on every real volume
was refused, and the diagnosis named a block pointer that was not one.

The validation is now skipped for such an inode, the decision resting upon the
mode, the sector count and the extended attribute block, all of which are parsed
before the words are examined. The words are decoded either way; only their
interpretation differs.

This was found by the self-test of [`EXT2-VERIFICATION.md`](EXT2-VERIFICATION.md), Section 5 failing on the first volume that
carried a fast symbolic link. It is worth recording because it is the
characteristic shape of a defect in this chapter: the code was correct for every
case it had been given, and wrong for a case the format permits and every real
volume contains.

### 2.5 What is refused

| Refused | Why it matters |
| ------- | -------------- |
| A read of a directory. | Its bytes are entries; a caller reading them as a stream has mistaken what it holds. |
| A target read for an inode that is not a symbolic link. | The bytes would be data, or block pointers, read as a path. |
| A symbolic link bearing no target. | It names nothing. The format does not forbid it; resolving the empty path would reach whatever happened to be current. |
| A target longer than the caller's buffer. | Refused rather than truncated: a truncated path names a different file, and may well name a real one. |
| A target holding a null byte. | It would be a path shorter than the file says it is, and everything above treats it as terminated. |
| A fast link whose size exceeds sixty bytes. | It declares no blocks and has no room for the target it claims. |

## 3. Writing

Everything before this reads. What follows alters a volume, and the difference is
not one of degree. A read that goes wrong returns the wrong bytes to one caller,
and can be retried. A write that goes wrong destroys somebody's data, cannot be
undone, and is ordinarily **silent**: a block allocated to two files reads
correctly for both of them until one of them writes, at which point the damage
appears in a file that was never touched.

Three disciplines follow, and they govern every function in this section.

### 3.1 The three disciplines

**A volume that may not be written is not written.** [`EXT2.md`](EXT2.md), Section 6 marks a volume
read-only where it declares a read-only compatible feature this kernel lacks, or
where it was not cleanly unmounted. Every function here asks before doing
anything at all, so the judgement is made in one place rather than by each caller
remembering it.

**A resource is marked used before it is referred to.** Allocation writes the
bitmap first and links the block or inode into its owner afterwards. The order
matters at exactly one moment — a machine that stops between the two — and the
two orders fail differently:

| Order | If the machine stops between | Consequence |
| ----- | ---------------------------- | ----------- |
| Bitmap first (this one) | The block is marked used and nothing refers to it | A **leak**: space is lost until a check reclaims it |
| Reference first | Something refers to a block the bitmap calls free | **Sharing**: the block is allocated again, to a second file |

A leak is a cost. Sharing is a fault that spreads, and it is the failure that
does not announce itself. The cheaper-looking order is the wrong one.

**Nothing reaches the medium until the cache writes it back.** Every write here
is made into a buffer which is then marked dirty, so a caller that needs the
volume consistent upon the medium must call `BufferSync`.

And one rule that is not a discipline so much as a consequence of them:
**every structure is read before it is written, and only the fields this kernel
parses are altered within it.** Composing 1024 bytes of superblock from the
parsed structure would write zeroes over every field this kernel does not
parse — the journal identifiers, the hash seed, the mount options, the head of
the orphan list — and would present the volume to the system that made it as
though those fields had never been set.

The fields `Ext2WriteSuperblock` maintains are accordingly a short list: the two
free counts, the state, the write time, and — since sub-task 5.8, the mount being
the only thing that alters it — the mount count. An operation that alters none of
them writes them all back unchanged, which the read-modify-write makes a no-op
rather than a risk.

### 3.2 The bitmaps

One bit stands for each block of a group and each inode of it, **1 meaning used**
and 0 free. The first of the group is bit 0 of byte 0 and the ninth is bit 0 of
byte 1 — least significant bit first within a byte, which is not the order a
diagram of a byte suggests and is the order the format states. The inode bitmap
begins at inode 1, numbers beginning at one and bits at zero, exactly as when an
inode is located within its table.

These are the first structures of the volume this kernel reads that it did not
need in order to read a file. Until now nothing had to know which blocks were in
use, because nothing allocated one; the free counts were read and believed. That
ends here, and the counts must now agree with the bitmaps or the volume describes
nothing.

Setting a bit already set, or clearing one already clear, is **refused rather
than performed**. Setting one already set means two owners believe they hold the
same block. Clearing one already clear means a block is being freed twice, and
the second free is precisely what allows it to be allocated to two files at once.
Neither announces itself at the moment it occurs, which is why both are stopped
at the moment they occur.

### 3.3 Allocation

A block is allocated by finding a free bit, setting it, decrementing the free
counts of the group and of the superblock, and writing both back.

The search is bounded by the group's **true extent** and not by the size of the
bitmap. The last group is short whenever the volume is not an exact multiple of
the group size ([`EXT2.md`](EXT2.md), Section 8.4), and the bits beyond its blocks are set by whatever
made the volume; a search that trusted those bits would issue a block the volume
does not hold upon a volume that happened to leave them clear.

A hint names a block the caller would like to be near — ordinarily the previous
block of the same file. Beginning the search in that block's group is the whole
of this kernel's allocation policy. It keeps a file's blocks together, which is
what makes reading it sequential, and it costs one division.

Where a group's descriptor claims free blocks and its bitmap holds none, the
allocation is **refused rather than continued in another group**. The volume
contradicts itself; moving on would leave the contradiction in place for the next
caller to meet, and would turn a detectable fault into a slow one.

An inode below `s_first_ino` belongs to the filesystem and is never issued. Such
an inode is ordinarily marked used already, so this is a second line and not the
first — but a volume that left one clear would otherwise have its root directory
handed out to a file.

### 3.4 Growing a file

`Ext2InodeBlockAllocate` is `Ext2InodeBlock` ([`EXT2.md`](EXT2.md), Section 9.3) with the holes filled
in. The decomposition of an index into levels is performed a second time rather
than shared, because the two walks differ at every step: one reads a pointer and
accepts zero as a hole, the other must allocate where it finds zero, zero the
block if it is a block of pointers, and write the pointer back into whatever
holds it.

Two things are zeroed, for two different reasons:

1. **A newly allocated block of pointers**, always. An unzeroed one is read as
   pointers to whatever the block last held — and those are real blocks belonging
   to real files.
2. **A newly allocated data block that the write does not wholly cover.** The
   part not written would otherwise become the previous owner's data appearing as
   this file's contents. Where the write covers the whole block this is skipped,
   every byte being about to be replaced.

The allocation therefore **reports whether it allocated**, rather than leaving the
caller to infer it from the offsets. Inferring it is how a caller gets it wrong,
and the cost of getting it wrong is disclosing another file's contents.

A write beginning beyond the end of the file leaves a hole between the two, which
is how a sparse file is made and costs nothing.

### 3.5 Truncation

Truncation downward frees every block beyond the new size and every block of
pointers left holding nothing. The subtree walk frees a table only when nothing
remains in it, which is what makes a truncation to zero return every block while
a truncation into the middle of an indirect range keeps the table still holding
the earlier half.

A subtree lying **wholly below** the new size is retained entire and is not
walked. Without that, truncating one block from a large file would read every
pointer block the file has, which is the whole of its indirection for the sake of
one block.

Truncation **upward allocates nothing**. The file grows by a hole, which is what
every Unix does and is why truncation is the cheap way to create a large sparse
file: nothing is allocated and nothing written but the size.

### 3.6 What is not promised: crash consistency

Allocating one block touches the bitmap, the group descriptor, the superblock and
the inode — four writes that must all happen or none. **This kernel cannot make
them atomic, and does not pretend to.** A machine that stops partway through
leaves a volume that is internally inconsistent in one of the ways Section 3.1
describes, and the recovery is `e2fsck`.

This is not an oversight to be corrected later within EXT2. It is what a journal
exists to provide and what EXT2, having none, does not have; ext3 is precisely
ext2 with one added. The ordering discipline of Section 3.1 does not remove the
window — it chooses which side of it the damage falls on, and chooses the side
that leaks rather than the side that corrupts.

### 3.7 What is refused

| Refused | Why it matters |
| ------- | -------------- |
| Any write to a read-only volume. | The judgement was made once, when the superblock was read, and is enforced in one place. |
| Setting a bitmap bit already set, or clearing one already clear. | Two owners of one block, or the second free that permits two owners. |
| Freeing a block outside the volume, or an inode the volume does not hold. | Arithmetic that has strayed, marking a bit that stands for something else. |
| Freeing an inode below `s_first_ino`. | It belongs to the filesystem; inode 2 is the root directory. |
| Allocating when a group's free count disagrees with its bitmap. | The volume contradicts itself, and another group would leave that in place. |
| Allocating when the volume reports no free block or inode. | Refused before the search rather than after it. |
| Writing or truncating a directory as a stream of bytes. | Its entries are a structure; sub-task 5.7 alters them properly. |
| A block index beyond what fifteen pointers can address. | Arithmetic past the end of the decomposition. |

## 4. Names: creating and destroying files

Sub-task 5.6 could write a file but not name one. An inode could be allocated and
filled, and nothing could reach it: a file that no directory names is reachable
by no path, and the only record that it existed at all is a bit in the inode
bitmap. This section closes that gap, and in doing so is the first place where
the two halves of the format — the inode table and the directories — must be kept
in step with one another.

### 4.1 Insertion: the slack a removal leaves

A record is longer than the name it holds whenever a name has been removed from
before it, or when it is the last of its block and runs to the end. That excess
is where the next name goes.

Inserting a name of *n* bytes needs a record of `round_up(8 + n, 4)`. Each block
of the directory is searched for one of two things:

| Found | Taken by |
| ----- | -------- |
| A record naming inode 0 that is long enough | Using it whole |
| A record in use whose length exceeds what its own name needs, by enough | Shortening it to what it needs, and laying the new record in the remainder |

Only when no block has room is a block allocated, the directory extended by it,
and the new block given a single record spanning its whole length — which is the
shape of an empty block, so the ordinary search then finds room in it.

Nothing is ever moved. That is the property the linked list exists to provide,
and it is why creating and removing the same name repeatedly reuses one piece of
space rather than growing the directory without limit.

**A name already present is refused.** A directory holding one name twice names
two files by the same path, and which is found depends on which record the
traversal reaches first — so the duplicate does not merely waste space, it makes
the path ambiguous.

### 4.2 Removal: joining two records

A record is removed by lengthening the record before it to cover it. The bytes of
the removed record stay exactly where they are, unreachable, until an insertion
takes them.

Where the record is the **first of its block** there is nothing before it to
lengthen. It is marked unused where it stands, by setting its inode number to
zero and touching nothing else — which is the format's own discipline, and
precisely why a traversal must pass over such a record rather than reading the
name still lying in it (Section 1.4). The two facts are the same fact seen from
each end.

`.` and `..` may not be removed. A directory without the first no longer knows
itself, and without the second no longer knows its parent; the resolver finds
both by looking (Section 1.5), so removing either makes every path through the
directory fail.

### 4.3 Creating a directory, and the link counts

A directory is not merely an inode with a different mode. Three things must be
true together, or what results is a directory that is not one:

1. It holds `.` naming itself and `..` naming its parent, the second record
   running to the end of the block so that the list of that block ends.
2. Its own link count is **two** — its `.` and the entry the parent holds — and
   not one.
3. The parent's link count **rises by one**, for the `..` now standing in the
   child.

The third is the one that is easy to omit and impossible to see. A parent whose
count is short by one is a directory that may be freed while a child still names
it, and nothing reports that until the freed blocks are given to another file.
`e2fsck` Pass 4 checks exactly this, which is why [`EXT2-VERIFICATION.md`](EXT2-VERIFICATION.md), Section 10 is worth more
than any assertion this kernel makes about itself.

The parent is checked against `EXT2_LINK_MAXIMUM` **before** anything is
allocated, so a refusal leaves no half-made directory on the volume.

### 4.4 Destroying a file: what the link count is for

`i_links_count` says how many names lead to a file. Removing a name lowers it;
the file itself is destroyed only when it reaches zero, since until then some
path still reaches it.

The order is the order of the discipline in Section 3.1, applied to a different
pair:

| Step | If the machine stops after it |
| ---- | ----------------------------- |
| The name is removed first | An inode nothing names — a leak a check reclaims |
| (the reverse) | A name leading to a freed inode, quite possibly reissued |

Within the destruction, the blocks go before the inode: an inode freed while its
blocks were still marked used would leak them with nothing left to say which they
were.

Two particulars. A **fast symbolic link** is not truncated, because the words of
`i_block` are its target and not pointers; truncating one would read the text as
blocks of the volume and free them. And a **directory** may not be unlinked as a
file, nor given a second name — two paths to one directory make a cycle in what
the format requires to be a tree, and a resolver walking `..` from within it
would have no single answer.

### 4.5 Destroying a directory

A directory is removed only when it holds nothing but `.` and `..`. Removing one
that held anything would leave every file within it reachable by no path — an
inode with a link count above zero that nothing names, which is exactly the state
a check has to repair by hand.

Its block is freed, its inode returned, and the parent's link count reduced by
the one the child's `..` held. The root is never removed, whatever it is named by.

### 4.6 A deleted inode is refused

Destroying a file sets `i_dtime` and leaves `i_links_count` at zero. **It does
not clear the mode or the block pointers** — nothing overwrites them, and a
recovery tool reads them for exactly that reason.

So nothing distinguishes a destroyed inode from a live file but those two fields,
and `Ext2ReadInode` now refuses one that has them. No name leads to such an
inode, so nothing above has any lawful way to reach it: an attempt to read one
means a directory entry survives that should not, and the blocks it names have
been given to somebody else. Reading it would serve another file's data under the
dead file's name.

This was added when the self-test of [`EXT2-VERIFICATION.md`](EXT2-VERIFICATION.md), Section 11 asserted that an inode freed
with its last name could no longer be read, and found that it could.

#### The deletion time may not be a small number

`i_dtime` means two things upon an EXT2 volume, and which of them is meant is
decided by magnitude alone.

Of an inode that has been freed it is the time of the deletion, which is what it
is defined as. Of an inode upon the **orphan list** — files whose last name went
while something still held them open — it is the *number of the next inode in
that list*, the list being threaded through this field rather than being given a
structure of its own. A check cannot ask which meaning is intended, so `e2fsck`
reads a value below `s_inodes_count` as a link and anything above it as a time.

This kernel has no clock and so cannot state when a file was deleted; what it
must state is *that* it was, which is what a non-zero `i_dtime` means. It
originally recorded the constant 1, and every inode it had ever freed was
therefore reported by `e2fsck` as the member of a corrupted orphan list naming
inode 1 — upon volumes that were in fact intact. The value is now
`EXT2_DELETION_TIME_UNKNOWN`, which is `UINT32_MAX`: no volume has an inode
numbered above `s_inodes_count`, so it cannot be read as a link, and it is the
last second the field can express, which is a defensible way of saying that the
moment is not known.

It was found in sub-task 5.8, by running `e2fsck` over a volume the filesystem
layer had created and destroyed files upon. Nothing within this kernel could see
it: the operation reported success and the volume read back correctly. Only a
tool that knew what the field meant could tell.

### 4.7 What is refused

| Refused | Why it matters |
| ------- | -------------- |
| A name already present in the directory. | Two files by one path; which is found depends on record order. |
| A name of no length, longer than 255 bytes, or holding a separator or null byte. | Reachable by no path, or ambiguous with another. |
| A name given to inode 0, or to an inode beyond the volume. | A name leading nowhere. |
| Removing `.` or `..`. | The directory would no longer know itself or its parent. |
| Removing a name that is not there. | Distinguished from success, so a caller is not told it removed something. |
| Creating a directory with `Ext2CreateFile`. | It would lack `.` and `..`, and its parent's link count would not account for it. |
| Creating a file with no format in its mode. | An inode of no type is what an unfilled table entry looks like. |
| A second name for a directory. | A cycle in what must be a tree. |
| Unlinking a directory, or removing a file with `Ext2RemoveDirectory`. | Each would leave the other's invariants unmaintained. |
| Removing a directory that is not empty. | Everything within it becomes reachable by no path. |
| Removing the root. | There is nowhere for a path to begin. |
| Any of the above upon a read-only volume. | The one judgement standing between this code and somebody else's data. |

