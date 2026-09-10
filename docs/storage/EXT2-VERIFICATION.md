<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# Verification of the EXT2 Implementation

**Corresponding phase**: 5, sub-tasks 5.1 to 5.7 — the eleven self-tests that
assert this kernel's EXT2 support, and what each of them is written against.

**Authority**: `PROJECT_GUIDELINES.md`, Section 2, the testing mandate.

**Asserted by**: `KernelVerifyExt2` in
[`../../kernel/test/verify_ext2.c`](../../kernel/test/verify_ext2.c), and the
five chapters beneath it in
[`../../kernel/test/ext2/`](../../kernel/test/ext2/): the composition of a
volume, the directories, the files, the writing and the probe of whatever volume
the machine actually carries.

**Where this sits**: the structures asserted here are
[`EXT2.md`](EXT2.md) and the operations are
[`EXT2-FILES.md`](EXT2-FILES.md). **Every limitation of this kernel's EXT2
support is enumerated in [`EXT2.md`](EXT2.md), Section 10**, this document's
included; Section 6 below is where a self-test stops being a self-test and
becomes a comparison against a volume `mke2fs` produced, which is the only thing
that catches an assumption shared between this kernel and its own test fixture.

**The eleven sections below were one section of eleven subsections** until this
document was made. Nothing about them changed but their depth: each is now a
section in its own right, in the order they run.

---

## 1. The self-test of the superblock

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
| Each of the twelve refusals of [`EXT2.md`](EXT2.md), Section 7 refuses. | A volume this kernel must not address being read anyway. |
| An unimplemented read-only feature yields a readable volume, not a refused one; an unimplemented incompatible feature yields a refused one. | The two fields treated alike, which either locks the kernel out of volumes it could read or lets it write volumes it must not. |
| A volume not cleanly unmounted is accepted read-only. | Writing to a filesystem that may be mid-repair. |
| A device too short to hold a superblock is refused. | A read past the end of a device, answered by whatever the layer beneath does with it. |

The cache is invalidated around each alteration. The self-test writes into the
device's storage directly, beneath both the block layer and the cache, so a cache
holding the previous contents would answer the next read with them, and the
assertion would be made against a volume that no longer exists.

## 2. The self-test of the descriptor table

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

## 3. The self-test of the inodes

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

## 4. The self-test of the directories

`KernelVerifyDirectories` composes a root directory and one subdirectory within
the same device of memory, laid out as Table 4.3 lays out its sample: entries
aligned upon four bytes, an unused record left where a name was removed, and a
final record whose length runs to the end of the block. The entries are written
from the same offset names the parser reads, so a mistaken offset cannot agree
with itself.

The traversal is asserted **entry by entry** against that layout and not merely
counted, for the reason [`EXT2-FILES.md`](EXT2-FILES.md), Section 1.4 gives: a traversal that has gone wrong
yields fragments of real names rather than an error.

| Property asserted | The silent failure it would catch |
| ----------------- | --------------------------------- |
| The root yields `.`, `..`, `file` and `sub`, each with its inode number, its file type, and the block and offset it stands at. | An entry advanced by the length of its name rather than by its record length, which would read every entry after the first from the middle of another. |
| The unused record between `file` and `sub` is passed over, and the name upon it is not found. | Reporting a file that was deleted. |
| The final record, whose length runs to the end of the block, ends the traversal. | Reading the padding after the last name as a further entry. |
| A name is matched by its whole length: `fil` and `files` do not match `file`, nor does `file` given a length of three. | A comparison stopping at the shorter of the two, which would resolve a path to the wrong file. |
| Twelve paths resolve to the inodes they name, including `/`, `///`, `/.`, `/..`, `/sub/`, `/sub/..`, `/sub/../file` and `//sub///inner`. | Every arithmetic and boundary error in the component walk. |
| Eight paths that name nothing are refused, including a relative path, a component that does not exist, and a file used as a directory or asserted to be one by a trailing separator. | A path resolving to something, which a caller will then act upon. |
| Each of the refusals of [`EXT2-FILES.md`](EXT2-FILES.md), Section 1.6 refuses, the field in question being altered in the composed volume and restored afterwards. | A malformed directory being walked rather than refused. |
| An entry declaring `EXT2_FT_DIR` for an inode that is a regular file is refused. | Directories and inodes describing different filesystems. |
| The same bytes are refused under the sixteen-bit reading and accepted under the eight-bit one, according to the feature flag alone. | The one place in the format where the wrong reading produces no diagnostic of its own. See [`EXT2-FILES.md`](EXT2-FILES.md), Section 1.2. |

The cache is invalidated on both sides of every alteration, for the reason
Section 1 gives: the bytes are written beneath both the block layer and the
cache, and a cache holding the previous contents would answer with them.

## 5. The self-test of file reading

`KernelVerifyFiles` reads the composed file, the composed sparse file, and both
forms of symbolic link.

The composed file holds, at each offset, a byte **derived from that offset**
rather than a constant or a pattern repeating every block. This is the whole
design of the test. Under a constant fill, a reader that returned the right
number of bytes from the wrong block would be indistinguishable from a correct
one; under a pattern repeating every block, so would a reader that resolved the
wrong block of the right file. Resolving the wrong block is the failure this
entire chapter is arranged to catch, and the fill is chosen so that it cannot
hide.

| Property asserted | The silent failure it would catch |
| ----------------- | --------------------------------- |
| The whole 1500-byte file reads as composed, across the boundary between its two blocks and ending part-way through the second. | A range mapped short, or a second block resolved wrongly. |
| A 100-byte run beginning at offset 1000 crosses the boundary at 1024 and returns the right bytes on both sides. | A reader taking the whole run from one block, which would return 24 correct bytes and 76 wrong ones. |
| A run wholly within the second block returns the right bytes. | An offset applied to the file where it belongs to the block, or the reverse. |
| A read crossing the end is shortened to it; a read at or beyond the end returns zero bytes and succeeds; a read of no length returns no bytes. | The end of a file reported as an error, which would oblige every caller to treat the conclusion of its work as a fault. |
| Block 12 of the sparse file reads as data and block 13 reads as zeroes. | A reader returning zeroes for both, or data for both — either of which passes an assertion made upon one alone. |
| A directory is refused. | Entries returned to a caller expecting text. |
| The fast link is recognised as fast, the slow one as slow, and both targets read exactly. | The two forms are read by entirely different code; a volume carrying only the common one leaves half of it unexercised. |
| A target longer than the buffer is refused rather than truncated. | A truncated path names a different file, and may well name a real one. |
| Five paths resolve through the links, including one whose target is relative and one absolute, and one where the link stands within the path. | Every error in the re-entry of the resolver, and in resolving a relative target against the wrong directory. |
| `Ext2ResolvePathNoFollow` returns the link, follows a link within the path, and is overridden by a trailing separator. | The distinction between acting upon a file and upon its name collapsing in either direction. |
| A link altered to name itself is refused. | A resolution that recurs until the stack is gone. |

## 6. A volume the kernel did not compose

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
the machine carries, which is what puts the inode code of [`EXT2.md`](EXT2.md), Section 9 against
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


## 7. Directories the kernel did not compose

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
Section 10.4 with the pointer resolution of [`EXT2.md`](EXT2.md), Section 9.3.
## 8. Files the kernel did not compose

The root directory of every volume is listed at each boot, one path is resolved
upon it, and what that path names is now read: the target of a symbolic link, or
the first sixteen bytes of a regular file. The path is resolved **without**
following a last link, so a link reports itself and its target rather than
silently reporting what it names.

An image was made from a tree holding a file, a directory two deep, and one link
of each form:

```sh
mke2fs -q -F -b 1024 -I 128 -r 1 -d tree/ fs5.img 8192
```

With the probe set to each in turn, the kernel reported:

```
EXT2 path /content.txt resolves to inode 12, regular file of 22 bytes.
EXT2 path /content.txt begins: 0x4F 0x78 0x79 0x73 0x2D 0x4F 0x53 0x20 0x72 0x65 0x61 0x64 0x73 0x20 0x61 0x20 (16 bytes read)

EXT2 path /shortlink resolves to inode 17, symbolic link of 4 bytes.
EXT2 path /shortlink is a target held within its inode: deep

EXT2 path /longlink resolves to inode 16, symbolic link of 71 bytes.
EXT2 path /longlink is a target held in a block: /deep/../deep/../deep/../deep/../deep/../deep/../deep/deeper/buried.txt
```

`debugfs -R "ls -l /"` gives inode 12 to `content.txt`, 17 to `shortlink` and 16
to `longlink`; `stat` reports `Blockcount: 0` for the first link and `2` for the
second, which is the distinction of [`EXT2-FILES.md`](EXT2-FILES.md), Section 2.4 as the volume itself records it.
The sixteen bytes are `Oxys-OS reads a` — `xxd` upon the host gives
`4f7879732d4f53207265616473206120` for the same prefix, byte for byte.

Finally, the probe was set to a path passing **through** a link:

```
EXT2 path /shortlink/deeper/buried.txt resolves to inode 15, regular file of 7 bytes.
EXT2 path /shortlink/deeper/buried.txt begins: 0x62 0x75 0x72 0x69 0x65 0x64 0xA (7 bytes read)
```

`debugfs -R "stat /deep/deeper/buried.txt"` states inode 15, a regular file of 7
bytes, and the bytes are `buried\n`. The link was resolved mid-path, its relative
target `deep` taken against the root that holds it, and two further components
walked from there.


## 9. The self-test of writing

`KernelVerifyWrites` is the first self-test in this project that alters a
filesystem, and the standard it is held to differs from every one before it. Two
things follow.

**The assertions are made about the volume and not only about the operation.**
After each sequence, the free counts of the group and of the superblock must
agree with one another and with what was actually taken — and they are read back
*from the volume*, not from the structure in memory. An allocator that
decremented its own copy and never wrote it back would satisfy every assertion
made against memory while leaving the volume claiming a block it had given away.

**Every write is made to the device of memory.** The volumes upon a real disk
belong to whoever booted this kernel; see Section 10 for what is done there,
and upon whose instruction.

| Property asserted | The silent failure it would catch |
| ----------------- | --------------------------------- |
| Both bitmaps report the volume as it was composed, including the one inode deliberately left free. | A bit index off by one, which reports the state of the block or inode beside the one asked about. |
| A block allocated is in use, the counts fall by one in memory **and upon the volume**, and freeing it restores all three. | Accounting kept in memory and never written back. |
| Freeing something already free is refused, for a block and for an inode. | The second free, which is what permits two owners of one block. |
| The one free inode is allocated, a second allocation is refused, and freeing it restores the volume. | Exhaustion mistaken for success; a free count that drifts from the bitmap. |
| Freeing a reserved inode is refused. | The root directory handed out to a file. |
| A write within a file reaches the volume, and the bytes on either side of it are untouched. | A write that covers more than it was given. |
| Truncation to nothing returns exactly the blocks the file held, and rewriting it takes exactly them back. | This is the conservation check, and it is the strongest assertion here: what a file gives up it must take back, and any leak or double-count appears as a free count that fails to return to where it started. |
| A write beyond the end extends the file and leaves a hole that reads as zeroes. | An extension that allocates the intervening blocks, or one that reports the wrong size. |
| Truncation upward allocates nothing. | A sparse extension that is not sparse. |
| A write into an unoccupied entry of the doubly indirect block allocates **two** blocks. | A level of the walk silently skipped: the difference between one block and two is the whole of whether the indirect block was allocated. |
| `Ext2VerifyGroupDescriptors` still passes after all of it. | The volume no longer accounting for itself, which is what `e2fsck` would report and what nothing else here would. |
| A read-only volume refuses allocation, freeing, and every write of a structure, an inode or a file. | The one judgement that stands between this code and somebody else's data. |

The volume is restored between sequences by emptying the cache **first** and
composing afterwards. That is the opposite of the order every earlier self-test
uses, and the difference matters: `BufferInvalidateDevice` writes dirty buffers
back before discarding them, so composing first would flush the writes of the
test just finished onto the volume just composed, restoring nothing. No self-test
before this one had ever left a dirty buffer behind.

## 10. Writing a volume the kernel did not compose

The strongest evidence available for this sub-task is not a self-test at all. It
is `e2fsck`'s opinion of a volume this kernel has written.

A volume upon a disk belongs to whoever booted this kernel, so the writing is
performed only upon the operator's instruction, given at the GRUB menu, and even
then it is bounded twice: it writes only to a regular file named
`/oxys-write-test`, and it **does not create one**. A volume that does not
already hold that file is left untouched.

An image was made holding that file and one other, and booted from the
`Oxys-OS (EXT2 write self-test)` entry:

```sh
mke2fs -q -F -b 1024 -I 128 -r 1 -d tree/ fs6.img 8192
e2fsck -fn fs6.img      # 13/2048 files, 308/8192 blocks
```

The kernel emptied the file and wrote 8192 bytes into it, each byte derived from
its own offset:

```
EXT2 write test: the command line permits writing to ata0.
EXT2 write test: wrote 8192 bytes to /oxys-write-test (inode 13, 16 sectors); volume now reports 7877 free blocks and 2035 free inodes.
```

Afterwards, upon the host:

```
$ e2fsck -fn fs6.img
Pass 1: Checking inodes, blocks, and sizes
Pass 2: Checking directory structure
Pass 3: Checking directory connectivity
Pass 4: Checking reference counts
Pass 5: Checking group summary information
fs6.img: 13/2048 files (0.0% non-contiguous), 315/8192 blocks
```

**No errors.** Five passes, and in particular Pass 5, which checks the group
summary information — the free block and free inode counts this kernel maintained
and wrote back. That is an independent judgement of the whole allocation path by
the tool whose business it is, and it is worth more than any assertion this
kernel can make about itself.

The block count rose from 308 to 315: the file held one block of twelve bytes and
now holds eight of 1024, which is seven more. The 8192 bytes extracted with
`debugfs dump` match the expected pattern byte for byte over their whole length,
and the other file in the image reads exactly as it did before.

Sub-task 5.7 extended the same gated test with names. Within one boot the kernel
created a directory, created a file within it, wrote to that file, and then
removed both — so a volume that held `/oxys-write-test` before the test holds
exactly the same set of names afterwards, with that one file rewritten:

```
EXT2 write test: created /oxys-made (inode 14) holding within (inode 15).
EXT2 write test: removed both again.
EXT2 write test: wrote 8192 bytes to /oxys-write-test (inode 13, 16 sectors); volume now reports 7877 free blocks and 2035 free inodes.
```

`e2fsck -fn` again reported **no errors**, and this time three of its passes are
the point. Pass 2 checks the directory structure — the records this kernel split
and joined. Pass 3 checks directory connectivity — the `.` and `..` it wrote.
Pass 4 checks reference counts — the link counts it raised and lowered, including
the parent's, which is the one [`EXT2-FILES.md`](EXT2-FILES.md), Section 4.3 describes as impossible to see. The
volume reported `13/2048 files`, exactly as before the test, so the two inodes
created were genuinely returned; `debugfs -R "ls -l /"` lists the same five
entries it listed before; and the 8192 bytes still match byte for byte.


## 11. The self-test of names

`KernelVerifyDirectoryWrites` alters the linked list of records that a directory
is, and the failures of a list are its subject: a record whose length no longer
reaches the next one, two records overlapping, a record left in use that nothing
points past.

**None of them is visible in the operation that caused it.** The name just
inserted is found perfectly well; the directory reads correctly until the
traversal reaches the record that was damaged. So the assertions are made by
traversing the **whole** directory afterwards and counting what comes out, and by
requiring the volume to account for itself at the end.

| Property asserted | The silent failure it would catch |
| ----------------- | --------------------------------- |
| After an insertion the directory yields exactly one entry more than before, and the name resolves as a path. | A record split wrongly, leaving the records after it unreachable or overlapping — invisible in the name just inserted. |
| A name already present is refused, and so is a second insertion of an existing name. | Two files reachable by one path. |
| After removal the directory yields exactly what it did before, and the name is gone. | A join that lost or duplicated a record. |
| Removing a name that is not there, and removing `.` or `..`, are refused. | A caller told it removed something; a directory that no longer knows its parent. |
| Sixty-four insertions and removals of the same name consume **no blocks**. | A directory that grows without limit under ordinary use, the slack of [`EXT2-FILES.md`](EXT2-FILES.md), Section 4.1 never being reused. |
| A created file has one link, no size, and can be written and reached by path. | An inode allocated but never linked, or linked but never written. |
| A second name raises the link count; removing one of two names removes the name and not the file. | An unlink that destroys a file another name still leads to — which leaves that name pointing at an inode that may already have been reissued. |
| Removing the last name frees both the inode and its blocks, and the inode is then free in the bitmap **and** refused as deleted. | A file destroyed in the directory but not on the volume, or an inode reachable after it was freed. |
| A created directory has two links, its parent gains one, and `/made/.`, `/made/..` and `/made/../made` all resolve. | The link-count error of [`EXT2-FILES.md`](EXT2-FILES.md), Section 4.3, and a `..` naming the wrong inode — which would be reachable and would lead out of itself to somewhere else. |
| A directory holding a file is not empty and is not removed; unlinking it as a file and giving it a second name are refused. | Files made unreachable; a cycle in the tree. |
| An emptied directory is removed and the parent's link count returns. | The count that Pass 4 of `e2fsck` checks. |
| The root is not removed. | There is nowhere for a path to begin. |
| After all of it the free counts return to what they were, the root yields what it did, and `Ext2VerifyGroupDescriptors` passes. | Anything leaked or double-counted across the whole sequence. |
| A read-only volume refuses insertion, removal, creation and destruction alike. | The judgement that stands between this code and somebody else's data. |

