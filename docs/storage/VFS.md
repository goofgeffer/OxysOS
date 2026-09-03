# The Virtual Filesystem Layer

**Corresponding phase**: 5, sub-task 5.8, which completes the phase.
**Authority**: `PROJECT_GUIDELINES.md`, Sections 2 and 4.
**Implemented by**: [`../../kernel/fs/vfs.c`](../../kernel/fs/vfs.c),
[`../../kernel/fs/ext2_vfs.c`](../../kernel/fs/ext2_vfs.c),
[`../../kernel/include/oxys/vfs.h`](../../kernel/include/oxys/vfs.h),
[`../../kernel/include/oxys/ext2_vfs.h`](../../kernel/include/oxys/ext2_vfs.h).
**Asserted by**: `KernelVerifyVfs` in [`../../kernel/kernel.c`](../../kernel/kernel.c).

---

## 1. What was missing

Sub-tasks 5.1 to 5.7 built an EXT2 implementation that can do everything the
format admits: read a superblock, find an inode, resolve a path, read a file,
allocate blocks, write, truncate, create a name, destroy a file. What it could
not do was be *used*.

Three things were absent, and they are absent together rather than by
coincidence.

**Nothing was retained.** Every operation of Section 12 of
[`EXT2.md`](EXT2.md) takes a device and a superblock and gives them back. A
caller that wished to read two files read the superblock twice. There was no
object that meant "this volume, open".

**Nothing was open.** A file was acted upon by naming it: read this path at this
offset, write that path at that one. There was no position that advanced, so
reading a file sequentially meant the caller keeping the offset and the size, and
two readers of one file could not exist without each keeping its own.

**There was one volume.** `Ext2ResolvePath` begins at inode 2 of the device it
is given. A machine with two disks had two unrelated trees and no path that
reached both.

This layer supplies the three. It is the last sub-task of Phase 5 because it is
the one that turns a format into a filesystem, and it is where Phase 6 begins:
the system calls of sub-task 6.7 are these operations with a user's arguments
copied in.

## 2. The shape

```
                       VfsOpen, VfsRead, VfsResolve, VfsMountVolume
                                        |
     +--------------------------------- | ---------------------------------+
     |               kernel/fs/vfs.c    v                                   |
     |                                                                      |
     |   mount table        node cache        open file table               |
     |   4 mounts           64 nodes          32 descriptors                |
     |   one tree           one identity      one position each             |
     |                      per file                                        |
     +----------------------------------|-----------------------------------+
                                        |  VfsFilesystemOperations
     +----------------------------------|-----------------------------------+
     |            kernel/fs/ext2_vfs.c  v                                   |
     |   the translation: node <-> inode, mount <-> superblock,             |
     |   neutral type <-> i_mode, refusal <-> code                          |
     +----------------------------------|-----------------------------------+
                                        |
                          kernel/fs/ext2.c   (the format)
                                        |
                     drivers/block/buffer.c  (the cache)
                     drivers/block/block.c   (the device)
                     drivers/ata/ata.c       (the disk)
```

The layer knows nothing of EXT2 and `ext2.c` knows nothing of the layer.
`ext2_vfs.c` is the only file in the project that knows both, and it is short
because everything in it is translation.

That division is why `ext2_vfs.c` is a file of its own rather than a chapter of
`ext2.c`. The two answer different questions. `ext2.c` answers what the format
is: where a structure lies, how its bytes are ordered, what makes a volume
contradict itself. `ext2_vfs.c` answers how that format is presented as one
filesystem among several. Declaring the binding in `ext2.h` would have made every
consumer of the format compile against the filesystem layer as well, and the
direction of the dependency would no longer be legible from the includes.

## 3. The operations a filesystem supplies

`VfsFilesystemOperations` is sixteen function pointers. A filesystem is
registered under a name — `VfsRegisterFilesystem("ext2", ...)` — and a mount asks
for a type by that name, which is what makes the layer virtual rather than a
wrapper around the one filesystem that exists.

Four of them are not optional: `mount`, `unmount`, `read_node` and `lookup`.
Without them nothing can be reached at all, so their absence is refused at
registration rather than at the first call. Every other entry may be null, and
the layer refuses the operation with `VFS_ERROR_UNSUPPORTED` rather than calling
through a null pointer — which is what allows a read-only filesystem, or a
filesystem with no directories, to be added later without a change here.

Two contracts run through all of them and are stated once rather than at each:

- **A name is given by its address and its length** and is not terminated, so
  that one component of a path may be used where it stands. Nothing in this
  layer copies a path or a component out of the caller's string.
- **A count reports what was transferred**, not what was asked. A partial write
  is a partial write: those bytes are upon the volume and the file's size
  accounts for them.

## 4. Resolution

`VfsWalk` takes a starting directory, a path, **and a length**, and produces the
node the path names. The length is what makes everything else in this file
buffer-free: the parent of a path is resolved by walking the prefix of that path
where it stands, so `VfsResolveParent`, which every operation that alters a
directory needs, copies nothing.

The rules are those of POSIX.1-2017, Section 4.13, and each is applied where it
arises:

| Rule | Where |
| ---- | ----- |
| A path beginning with a separator resolves from the root. | The start of `VfsWalk`. |
| Successive separators are equivalent to one. | The skip that precedes each component. |
| A component that is not the last must be a directory. | Tested before the lookup, so that the diagnosis names the component and not what follows it. |
| A trailing separator asserts that what the path names is a directory. | Recorded per component and applied at the end, since only the last one asserts anything. |
| A symbolic link met in the path is replaced by its target. | Section 4.2. |

`.` and `..` are **not interpreted**. Every EXT2 directory holds both as ordinary
entries, and the `..` of a volume's root names that root; the ordinary lookup
therefore resolves them, and a layer that interpreted them would be
second-guessing the volume. The one exception is `..` leaving a *mounted* volume,
which is a fact of the tree and not of the volume, and is Section 5.3.

### 4.1 Two resolvers, and why both remain

`Ext2ResolvePath` of sub-task 5.4 is not used by this layer and is not retired.
The two resolve different things. `Ext2ResolvePath` resolves a path within one
volume, needs no mount, and is what the reports and self-tests of sub-tasks 5.4
to 5.7 exercise the format with. `VfsWalk` resolves a path within the *tree*: it
crosses mount points, and it produces nodes from the cache so that what it
returns has an identity. Retiring the first would leave the format untestable
except through the layer above it, which is precisely the coupling this design
avoids.

### 4.2 Symbolic links

A link is followed by resolving its target, which re-enters `VfsWalk`. A relative
target is resolved against the directory holding the link — which the walker is
still holding at that moment, and which is the whole of the difference between a
relative target and an absolute one — and an absolute target from the root.

The recursion is what the depth bound is for. Each frame carries a target buffer
of 256 bytes and the bound is eight, so the whole cost of the arrangement is two
kibibytes of stack, and a link that names itself stops rather than consuming the
stack. Eight is the depth POSIX requires an implementation to allow.

A link standing as the **last** component is followed where the caller asked for
the file and left alone where it asked for the name, which is the distinction
between `stat` and `lstat` and between acting upon a file and acting upon its
name. A trailing separator overrides it: such a path asserts a directory, a link
is not one, so it is asking for what the link names.

## 5. The mount

### 5.1 Found through the node, never through the path

There is no string prefix matching anywhere in this layer. A mount is found
through the **node it covers**: `VfsNode` carries a `mounted` pointer, and
resolution substitutes the covering mount's root for the node the moment it
reaches one. The path a mount was made at is retained for a report and for
nothing else.

This is the central decision of the design, and prefix matching is the obvious
alternative that appears to work. It fails in three ways that have no remedy
within it:

1. **A symbolic link whose target crosses a mount point** is not a path any
   prefix describes. The resolution continues from wherever the target leads,
   and no string was ever composed that a prefix could be matched against.
2. **`..` leaving a mounted volume** must arrive at the parent of the mount
   point, and a prefix has no way to know it has left. Section 5.3.
3. **One directory reached by two routes** would be matched against one prefix
   and not the other.

Every one of the three is silent. The path resolves, to the wrong file.

### 5.2 Crossing into a volume

When a lookup produces a node that carries `mounted`, the node is released and
the mounted volume's root is taken in its place. The directory beneath is hidden
entirely for as long as the mount stands — not merged with what covers it, which
is what a mount means and what makes the covered directory's contents
unreachable rather than shadowed name by name.

### 5.3 Leaving a volume by `..`

The `..` of a volume's root names that root. That is what the volume says and it
is correct whenever the volume stands alone.

Where the volume is mounted within another, the parent of its root is the
directory the mount covers — a fact of the tree that only this layer holds. So
before `..` is looked up at all, the walker steps from a mounted root to the node
that mount covers, and looks up `..` there. It is written as a loop rather than
a single step: this kernel does not stack mounts, but a bound written as a loop
does not become wrong when it does.

### 5.4 What a mount refuses

| Refused | Because |
| ------- | ------- |
| The first mount anywhere but `/`. | Until the root stands there is no tree for a path to resolve within, so a mount elsewhere would have nowhere to attach. |
| A device already mounted. | Two mounts of one device would hold two superblocks of one volume, and each would allocate blocks without regard to what the other had taken. |
| A point already covered. | This kernel does not stack mounts. |
| A point that is not a directory. | There would be nothing to resolve through. |
| A type not registered. | There is nothing to read the volume with. |

### 5.5 What an unmount refuses

An unmount is refused where **anything upon the volume is still held**: an open
descriptor, a node somebody is resolving through, or another volume mounted
within it. The test is one rule — a node of this mount in use by anything but the
mount itself — and the three cases are instances of it, the inner mount holding
the directory it covers.

The alternative is to withdraw it regardless, and the failure would then appear
at the next read of a descriptor addressing a volume that no longer exists,
which is a fault reported far from its cause.

The order of the withdrawal matters in one place. The mount's own node references
go **before** the filesystem is told to release the volume: releasing a node
calls the filesystem's `release_node`, which is entitled to look at the volume's
description, and doing it afterwards would be reading a description the
filesystem had just given back.

## 6. Nodes

A node is the identity of a file within the kernel. Two callers that reach the
same file by any route hold the same node.

**That is a correctness requirement and not a convenience.** Were each caller to
hold a copy of the file's description, a write through one that extended the file
would leave the other's copy holding the old size and the old block pointers, and
the next write through that copy would restore them — truncating the file and
orphaning every block the first write had allocated. Nothing would report it.

The same identity is what makes the link counts of a directory correct. Creating
a directory raises the parent's link count for the `..` written into the child;
the creation is performed through the parent's *node*, so every holder of that
directory sees the new count, and the self-test asserts exactly this by reading
the root's link count before and after.

### 6.1 Nodes in use, not nodes recently used

A node whose last reference goes is released at once. This is a table of nodes in
use and not a cache of nodes recently used, and the distinction is deliberate: a
retained node is a description of a file that may since have been destroyed and
its inode reissued to another file, and nothing here would know.

The cost is that opening the same file twice in succession reads its inode twice.
Those reads are served by the buffer cache of sub-task 4.6, so the cost is the
decoding and not the medium. It is the right trade for a kernel that has no
invalidation protocol, and Section 11 records it as a limitation rather than as a
design.

The root node of every mount, and the node every mount covers, are held for the
life of the mount. That is what keeps them from being released and is why
`VfsMountIsBusy` counts the root's references against one rather than against
zero.

## 7. The open file

`VfsFile` is a node, a position and the flags it was opened with. The position
belongs to the open file and not to the node, which is why two descriptors upon
one file read independently of one another while writing to the same bytes.

Two things about it are decisions rather than mechanism.

**An appending write goes to the end of the file as it stands at that moment**,
and not to where the position happens to be. That is the whole purpose of the
flag: two writers appending to one file must not overwrite one another, which
they would were the offset taken from a position each had advanced on its own.

**The position of a directory descriptor is the filesystem's own cookie**, not a
byte offset. It is kept in the same field because it is the same thing — where
the next read begins — and because a caller that seeks a directory to a cookie it
was given earlier is doing what `seekdir` means. The EXT2 binding packs the block
index of the traversal into the high half and the offset within that block into
the low half; both are bounded far below what they are given, and an index that
nevertheless exceeded it is refused rather than truncated, a truncated index
naming a different block of the same directory and reporting entries twice.

A seek beyond the end of a file is permitted, as POSIX requires: writing there
leaves a hole, which is how a sparse file is made. A seek before the beginning is
refused, there being nothing there. The two directions are computed separately so
that neither the sum nor the difference can wrap, and a negative offset is
negated in the unsigned domain, `INT64_MIN` having no positive counterpart in the
signed one.

## 8. The mark a mount leaves upon a volume

A volume mounted for writing is marked as **not cleanly unmounted for as long as
it is open**, and the mark is forced to the medium before anything else is
written to the volume.

The order is the whole point. A machine that stops while the volume is open
leaves that mark behind, so the next mount reads a volume that was not cleanly
unmounted, `Ext2ReadSuperblock` makes it read-only, and it stays read-only until
a check has been run over it. A kernel that marked the volume upon *unmounting*
would record only the mounts that ended well — which are exactly the ones that
need no record.

The mark is made by **clearing** the bit that says the volume was cleanly
unmounted, and not by setting the bit that says errors were found in it. They are
distinct bits of one field and they say different things: a volume that is merely
open is intact, and a kernel that recorded it as faulty would have `e2fsck`
report errors upon a disk that has none, and would erase the record of a volume
that genuinely had some by overwriting the field rather than masking it.

The mount count is raised at the same moment. It is half of what tells a check
that a volume is due for one — `s_max_mnt_count` states how many mounts may pass
between checks and this states how many have — and `Ext2WriteSuperblock` was
extended in this sub-task to write it, there having been nothing before that
altered it.

### 8.1 A stranger's disk is mounted read-only

The root volume of a machine this kernel is booted upon is mounted **read-only**
unless the operator chose the entry of the GRUB menu that permits writing.

A kernel that mounted a stranger's disk for writing would mark it as not cleanly
unmounted merely by having been booted, and every such disk would then demand a
check before its owner could mount it again. That is a real cost imposed for
nothing, and it is imposed by the very mechanism that exists to protect them.

## 9. What the layer refuses

### 9.1 The codes

`VfsError` is sixteen values and a description in words is kept beside it. The
words are what a diagnostic prints; the code exists because Phase 6 must return a
refusal to a user program, which cannot be given a pointer into the kernel's
read-only data. Both are set at every refusal, in one statement, so that neither
can say something the other does not.

### 9.2 Read-only is tested once

A mount that may not be written is refused in one place — `VfsWritable`, against
the node's mount — rather than by each operation remembering to test it. A volume
the filesystem judges unwritable is read-only whatever was asked for at the
mount, and a read-only mount of a writable volume is read-only because it was
asked for; the mount records the disjunction of the two.

### 9.3 A file something holds is not destroyed

`VfsUnlink` and `VfsRemoveDirectory` refuse a file anything else holds.

POSIX would keep such a file alive until its last descriptor closed. That
requires a list of files that have no name and are not yet gone, together with
the discipline that empties that list after a machine has stopped — which is the
orphan list `i_dtime` is threaded through and which `e2fsck` exists to reclaim.
This kernel has neither.

The alternative to refusing is not "slightly wrong". `Ext2Unlink` frees the inode
and every block of the file the moment the last name goes, so a descriptor still
reading it would be reading blocks that had been given to somebody else, and the
inode would be reissued to another file while the first was still open. That is
silent, and it is corruption rather than a surprise.

The test is that the node has a reference beyond the one the resolution itself
took. A mount point is caught by the same test, its mount holding the node as the
directory it covers, so a mount point cannot be unlinked either — which is right
for the same reason.

### 9.4 A name and the file it names lie upon one volume

`VfsLink` refuses two paths that resolve to different mounts. A directory entry
names an inode of the volume the directory belongs to, and there is no number one
volume could write that would name a file upon another.

## 10. Verification

`KernelVerifyVfs` asserts the layer against two volumes composed within the two
memory-backed block devices, so the verification needs no disk and touches
nobody's data. Every property below is asserted at each boot; the procedure is
`make verify`, described in [`../project/TESTING.md`](../project/TESTING.md).

The second volume is a **copy of the first with one field altered** — the owner
of `/file` — rather than a second composition. That is what allows an assertion
to say *which* volume a path reached, which is the only way the mount can be
tested at all: two volumes that were identical would make every crossing
assertion vacuous, and two volumes that differed everywhere would make it unclear
which difference the assertion had detected.

### 10.1 Resolution

| Property asserted | The silent failure it would catch |
| ----------------- | --------------------------------- |
| Each of `/`, `/file`, `/sub`, `/sub/inner` resolves to the inode the composition gave it, of the format it gave it. | A walk that lost a component, or found the right file by the wrong route. |
| The size of a file is reported as the volume states it. | The two halves of the size joined wrongly, which bounds every read at the wrong place. |
| `//sub//inner` resolves as `/sub/inner`, and `/sub/` resolves while `/file/` is refused. | Repeated separators read as empty components; a trailing separator that asserts nothing. |
| `/.`, `/..`, `/sub/..` and `/sub/../file` all resolve, the `..` of the root naming the root. | `.` and `..` interpreted rather than looked up — which would disagree with the volume the moment a volume disagreed with the interpretation. |
| A symbolic link is followed as the last component, within a path, and in both its fast and slow forms; and is *not* followed where the name was asked for. | `stat` and `lstat` conflated; a link in one of the two forms unreadable. |
| A path is refused for the reason that distinguishes it: absent, not a directory, relative, empty. | A resolver that reached the right conclusion by the wrong route, which will reach a wrong one elsewhere. |
| A record whose inode number is zero is neither resolved nor listed. | The bytes of a removed name read as a file that was deleted. |

### 10.2 The open file

| Property asserted | The silent failure it would catch |
| ----------------- | --------------------------------- |
| The whole of a file is read through a descriptor and matches contents derived from the offset. | A read that returned the right count from the wrong block — which a constant fill would not catch. |
| The position advances by exactly what was transferred. | A position that drifts, so that a sequential read silently skips or repeats. |
| A read at the end transfers nothing and is not a failure. | Every caller obliged to treat the conclusion of its work as a fault. |
| Seeks from all three origins arrive where they were sent, and a read after a seek begins there. | A seek that is computed but not applied. |
| A seek beyond the end is permitted; one before the beginning is refused. | Sparse files made impossible; a position that wrapped. |
| Two descriptors upon one file have two positions, and moving one does not move the other. | The position kept upon the node rather than the open file, which would make two readers of one file impossible. |
| A directory is not read as a stream; a file is not opened as a directory; an open asking neither to read nor to write is refused; a link is not opened where the open refused to follow one. | Entries read as bytes; an open that could do nothing accepted, so that the discovery is deferred to the first read. |
| A descriptor closed twice is refused. | A node released twice, freeing it beneath its remaining holder. |

### 10.3 Directories

| Property asserted | The silent failure it would catch |
| ----------------- | --------------------------------- |
| The root lists exactly its six entries and the subdirectory exactly its three. | A traversal that lost or repeated a record, which the count catches and a search for one name does not. |
| What a directory lists is what resolves, and what it does not list does not resolve. | A listing and a lookup that disagree — two readings of one structure. |

### 10.4 Alteration

| Property asserted | The silent failure it would catch |
| ----------------- | --------------------------------- |
| A file is created, written, closed, reopened, and read back identically. | A write that reached the wrong block, or a size that did not account for it. |
| An exclusive creation of a file that exists is refused, with the code that says so. | A creation that silently opened what was there. |
| An appending write goes to the end and not to the position. | Two appenders overwriting one another. |
| Truncation to nothing and upward into a hole both work, and the hole reads as zeroes. | Blocks freed that were not, or a hole read as whatever the blocks last held. |
| An open that truncates discards the contents. | A file that keeps data the caller believed it had discarded. |
| A second name raises the link count, both names lead to one file, and removing one leaves the other. | An unlink that destroys a file another name still leads to. |
| A directory may not be given a second name. | A cycle in what must be a tree. |
| A new directory bears two links and **its parent gains one**. | The count of Section 13.3 of [`EXT2.md`](EXT2.md) — a parent short by one may be freed while a child still names it, and nothing reports it until the freed blocks are given away. |
| Removing the directory returns the parent's link. | The same, in the other direction. |
| A directory holding names is not removed; a directory is not unlinked as a file. | Everything within a directory made reachable by no path. |
| A file that is open is not destroyed, and is destroyed once nothing holds it. | Section 9.3: blocks freed beneath a reader. |

### 10.5 The mount

| Property asserted | The silent failure it would catch |
| ----------------- | --------------------------------- |
| Nothing resolves and no mount may be made away from `/` before a root stands. | A tree with no root that appears to work until a path is resolved. |
| A mount upon something that is not a directory, of a device already mounted, or of a type not registered, is refused with the code that says so. | Two superblocks of one volume, each allocating without regard to the other. |
| The mount point names the first volume's directory before the mount and the second volume's root after it. | A mount recorded but not applied. |
| A path crossing the mount point reaches the **second** volume, and one that does not reaches the first. | The crossing applied to the wrong node, or to every node. |
| What the mount covers is entirely unreachable while it stands. | A mount that merges rather than covers. |
| `..` from the root of the mounted volume leaves it, and a path that returns and crosses again reaches the second volume once more. | Section 5.3 — the failure a layer matching paths by prefix would have, and the one nothing else here would catch. |
| A read-only mount refuses a write and a creation. | Somebody's volume altered by a kernel that was told not to. |
| The root is not withdrawn while a volume stands within it, nor while a file upon it is open. | Descriptors addressing a volume that no longer exists. |
| What the mount covered reappears exactly as it was when the mount is withdrawn. | A covering that damaged what it covered. |

### 10.6 The mark, and what was left behind

| Property asserted | The silent failure it would catch |
| ----------------- | --------------------------------- |
| After a writable mount, the state read **back out of the medium** has the clean bit clear and the error bit still clear, and the mount count is raised. | A mark that never reached the disk, which would protect nothing in the one circumstance it exists for; and a volume falsely recorded as faulty. |
| After a clean withdrawal the clean bit is set again. | A volume that demands a check after every ordinary use. |
| **No node is left held and no descriptor left open** after everything is closed and withdrawn. | A resolution that failed to release what it held — which exhausts a fixed table long before a machine has done any real work, and does so silently until it does. |
| The volume still describes itself consistently: the superblock is read afresh and `Ext2VerifyGroupDescriptors` passes. | Anything leaked or double-counted across the whole sequence. |

The descriptor table is verified **after** the unmount rather than during the
mount, because a mounted volume is marked unclean and is therefore permitted to
disagree with itself — which is what an unclean state means.

### 10.7 Corroboration upon a volume this kernel did not make

The self-test establishes the layer consistent with itself. The corroboration is
a `mke2fs` image, and it is recorded in
[`../project/TESTING.md`](../project/TESTING.md), Section 12. In summary: the
kernel mounted the image at `/` and listed its root with the inode numbers, types
and names `debugfs` gives; a writable mount left the volume marked not clean with
a mount count of one, and `e2fsck -fn` found no structural error in it; a
read-only mount left the volume byte for byte as it was; 5000 bytes were written
through a descriptor and read back through the same descriptor after a seek, and
`debugfs` extracted them from the image matching byte for byte; the volume was
then withdrawn and reported clean; and the whole was repeated upon a volume of
4096-byte blocks.

**That corroboration found a defect in sub-task 5.7.** It is Section 11.1.

## 11. Corrections this sub-task made elsewhere

### 11.1 The deletion time was read as an orphan-list link

Destroying a file records a deletion time in `i_dtime`, and this kernel, having
no clock, recorded the constant 1.

`i_dtime` means two things upon an EXT2 volume. Of an inode that has been freed
it is the time of the deletion, which is what it is defined as. Of an inode upon
the **orphan list** — files whose last name went while something still held them
open — it is the *number of the next inode in that list*, the list being threaded
through this field rather than being given a structure of its own. A check cannot
ask which meaning is intended and distinguishes them by magnitude: `e2fsck` reads
a value below `s_inodes_count` as a link and anything above it as a time.

So every inode this kernel had ever freed was reported by `e2fsck` as the member
of a corrupted orphan list naming inode 1, upon volumes that were in fact intact.
It is now `EXT2_DELETION_TIME_UNKNOWN`, which is `UINT32_MAX`: no volume has an
inode numbered above `s_inodes_count`, so it cannot be read as a link, and it is
the last second the field can express, which is a defensible way of saying that
the moment is not known.

It was found by running `e2fsck` over a volume this layer had created and
destroyed files upon, and it is exactly the class of defect the corroboration
exists for — the operation reported success, the volume read back correctly, and
only a tool that knew what the field meant could see it.

### 11.2 The mount count was never written

`Ext2WriteSuperblock` wrote the free counts, the state and the write time, those
being everything sub-task 5.6 altered. The mount is the first thing that alters
the mount count, so the field is now written as well.

## 12. Limitations

1. **There is no working directory, so no relative path is resolved.** Every path
   given to this layer must be absolute. A working directory is a property of a
   process and there are none before Phase 6. A relative *symbolic link target*
   is resolved, against the directory holding the link, that directory being
   known.
2. **The open file table is global.** In sub-task 6.9 it becomes per-process: a
   descriptor is an index into a process's own table, and the description it
   names is shared between the processes a fork produced. Nothing here assumes
   otherwise; the table is simply global while there is one thread of control.
3. **Nothing is cached between one use and the next.** A node whose last
   reference goes is released, so opening the same file twice reads its inode
   twice. See Section 6.1: it is the right trade for a kernel with no
   invalidation protocol, and it costs the decoding rather than the medium.
4. **A file that is open cannot be unlinked.** Section 9.3. POSIX would keep it
   alive until the last close; this kernel refuses instead.
5. **Nothing is renamed.** `Ext2Unlink` and `Ext2DirectoryInsert` exist, so a
   rename is two operations with a window between them in which the file has two
   names or none. An atomic rename is what `rename()` promises and is not offered
   here, and it also crosses two directories at once, which nothing above has yet
   needed.
6. **The refusal codes of the EXT2 binding are approximate.** The implementation
   beneath distinguishes its refusals in words and not by code — a failed lookup
   and a directory whose records are malformed are both `false` with a different
   sentence behind them — so the code assigned is the likeliest of the outcomes
   the operation admits, and the sentence is the authority. Where the distinction
   matters to a caller the binding establishes it beforehand instead: an existing
   name is found before a creation is attempted, and a directory's emptiness
   before its removal, so that `VFS_ERROR_EXISTS` and `VFS_ERROR_NOT_EMPTY` are
   exact. A volume with no room is distinguished from a volume that failed by
   consulting the free counts after the failure, which is a heuristic and is right
   whenever the volume is genuinely full.
7. **Permissions are recorded and not enforced.** A node carries its mode, its
   owner and its group, and nothing consults them. There are no users until
   Phase 6 and nothing to check a request against.
8. **The times are not maintained.** Reading a file does not update `i_atime` and
   writing one does not update `i_mtime`, there being no clock; a file this
   kernel creates bears a time of zero. This is the limitation of
   [`EXT2.md`](EXT2.md) restated, the layer adding nothing that would remedy it.
9. **A mount point is not remembered across an unmount and a remount**, and a
   mount is found by its path only for the purpose of withdrawing it. Two mounts
   made at paths that resolve to one directory would be refused by the covering
   test rather than by comparing the paths, which is right; but `VfsUnmount`
   compares the path as it was given, so a volume mounted at `/sub` cannot be
   withdrawn by naming `/sub/`.
10. **Nothing is mounted from a command line.** `VfsMountRoot` takes the first
    device carrying a volume it can mount. A `root=` parameter and an initial
    ramdisk to fall back upon both belong to Phase 7.
11. **The tables are fixed**: four filesystem types, four mounts, sixty-four
    nodes, thirty-two descriptors. A layer that drew its own structures from the
    heap could exhaust it, and would do so at exactly the moment something needed
    to write a diagnostic to a file. Only the filesystems' private descriptions —
    a superblock, an inode — are allocated, and those are bounded by these
    tables.
12. **Nothing here is safe against concurrent access.** From sub-task 6.13 the
    mount table, the node table and the open file table each require a lock, and
    a node's reference count must be adjusted atomically.
