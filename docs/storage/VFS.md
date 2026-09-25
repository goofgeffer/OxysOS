<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# The Virtual Filesystem Layer

**Phase**: sub-task 5.8 of [`../project/PLAN.md`](../project/PLAN.md); pipes from
sub-task 8.6.
**Source**: [`../../kernel/fs/vfs/`](../../kernel/fs/vfs/) — `vfs.c` (state,
refusals, accounting), `node.c` (Section 5), `path.c` (Section 3), `mount.c`
(Section 4), `file.c` (Sections 6 and 7), `namespace.c` (Section 8), `pipe.c`
(Section 9), `internal.h`; the EXT2 binding
[`../../kernel/fs/ext2_vfs.c`](../../kernel/fs/ext2_vfs.c); the interfaces
[`../../kernel/include/oxys/fs/vfs.h`](../../kernel/include/oxys/fs/vfs.h) and
[`ext2_vfs.h`](../../kernel/include/oxys/fs/ext2_vfs.h).
**Specifications**: IEEE Std 1003.1-2017, Section 4.13 (pathname resolution),
`open()`, `lseek()`, `link()`, `unlink()`, `pipe()`.

The layer that turns a filesystem format into a filesystem: it retains mounted
volumes, joins them into one tree, gives every file one identity, and holds open
files with positions. The system calls are these operations with a program's
arguments validated and copied ([`../design/PRIVILEGE.md`](../design/PRIVILEGE.md),
[`../design/LIBC.md`](../design/LIBC.md)); nothing of this layer is reimplemented
above it.

## 1. Structure

```
      VfsOpen, VfsRead, VfsResolve, VfsMountVolume, VfsPipeCreate, ...
   +------------------------ kernel/fs/vfs/ ---------------------------+
   |  mount table      node table       open file table     pipes      |
   |  4 mounts         64 a chunk       32 a chunk          8 a chunk  |
   |  one tree         one identity     one position each   one page   |
   |                   grown from the heap, first chunk static          |
   +------------------------------+------------------------------------+
                                  |  VfsFilesystemOperations
   +---------------- kernel/fs/ext2_vfs.c -----------------------------+
   |  node <-> inode, mount <-> superblock, type <-> i_mode,           |
   |  refusal <-> code                                                 |
   +------------------------------+------------------------------------+
                        kernel/fs/ext2/  ->  buffer cache  ->  block layer
```

The layer knows nothing of EXT2, and `kernel/fs/ext2/` knows nothing of the
layer. `ext2_vfs.c` is the only file that knows both, and holds only
translation. Putting the binding in `ext2.h` would make every user of the format
compile against the layer, hiding the direction of the dependency.

## 2. What a filesystem supplies

`VfsFilesystemOperations` is sixteen function pointers, registered under a name
(`VfsRegisterFilesystem("ext2", …)`); a mount asks for a type by name.

- `mount`, `unmount`, `read_node` and `lookup` are required, and their absence is
  refused at registration. Any other entry may be null; the operation is then
  refused with `VFS_ERROR_UNSUPPORTED` rather than called through a null pointer.
- **A name is an address and a length**, unterminated, so a component is used
  where it stands in the path and nothing is copied.
- **A count reports what was transferred**, not what was asked.

## 3. Resolution

`VfsWalk` takes a starting directory, a path **and a length**, and produces the
node the path names. The length lets `VfsResolveParent` walk a path's prefix in
place, so no operation that changes a directory copies a path.

| Rule (POSIX.1-2017, Section 4.13) | Applied |
| --------------------------------- | ------- |
| A leading separator starts at the root. | At the start. |
| Repeated separators are one. | Before each component. |
| A non-final component must be a directory. | Before the lookup, so the diagnosis names that component. |
| A trailing separator asserts a directory. | At the end. |
| A symbolic link is replaced by its target. | Below. |

- **Paths given to this layer are absolute.** The system-call layer joins a
  relative path to the process's working directory first
  ([`../design/SHELL.md`](../design/SHELL.md)).
- **`.` and `..` are looked up as ordinary entries**, which EXT2 stores; the one
  exception is leaving a mounted volume (Section 4).
- **Symbolic links** re-enter `VfsWalk`: a relative target from the directory
  holding the link, an absolute one from the root. Each level carries a 256-byte
  target buffer and the depth bound is `VFS_SYMLINK_DEPTH_MAXIMUM` (8, POSIX's
  minimum), so the recursion costs at most 2 KiB of stack and a self-naming link
  stops. A final link is followed when the file is wanted and not when the name
  is (`stat` against `lstat`); a trailing separator forces following.
- `Ext2ResolvePath` ([`EXT2-FILES.md`](EXT2-FILES.md)) remains, resolving within
  one volume without mounts, so the format can be tested without this layer.

## 4. Mounts

**A mount is found through the node it covers, never by path prefix.** A
`VfsNode` carries a `mounted` pointer; when a lookup produces such a node, the
walk releases it and continues from the mounted volume's root. The path a mount
was made at is kept only for reports and for withdrawal. Prefix matching fails,
silently, on a link whose target crosses a mount (no string was ever composed to
match), on `..` leaving a mounted volume, and on one directory reached by two
routes.

- **Covering hides.** The covered directory is unreachable while the mount
  stands, not merged.
- **Leaving by `..`.** A volume's root's `..` names that root. Before looking up
  `..` at a mounted root, the walk steps to the node the mount covers (in a loop,
  so stacked mounts would stay correct), and looks up `..` there.

| A mount is refused when | Because |
| ----------------------- | ------- |
| It is the first mount and not at `/`. | There is no tree to attach to. |
| The device is already mounted. | Two superblocks of one volume would allocate independently. |
| The point is already covered. | Mounts do not stack. |
| The point is not a directory. | Nothing to resolve through. |
| The type is not registered. | Nothing to read the volume with. |

**An unmount is refused while anything on the volume is held**: an open file, a
node being resolved through, or a mount within it (which holds the node it
covers). Otherwise a later read would address a volume that no longer exists.
The mount's own node references are released **before** the filesystem releases
the volume, since releasing a node may consult the volume's description.

### 4.1 The mark a writable mount leaves

A volume mounted for writing is marked **not cleanly unmounted** for as long as it
is open, and the mark reaches the medium before anything else is written. A
machine that stops mid-mount leaves the mark, and the next mount is read-only
until checked ([`EXT2.md`](EXT2.md)). Marking at unmount would record only the
mounts that ended well.

The mark **clears the clean bit**; it does not set the error bit. An open volume
is intact, and recording it as faulty would make `e2fsck` report errors on a
healthy disk and overwrite the record of real ones. The mount count is raised at
the same time. The clean bit is set again at a clean unmount.

### 4.2 Which volume goes where

| Mount | Volume | Writable | Why |
| ----- | ------ | -------- | --- |
| `/` | `ram0`, by name | Yes | The initial ramdisk ([`INITRD.md`](INITRD.md)): built by this build, gone at power-off. |
| `/etc` | The volume labelled exactly `oxys-etc` | Yes | This system's own disk ([`PERSIST.md`](PERSIST.md)). Mounted before `/mnt`. |
| `/mnt` | The first other volume found | Only if the GRUB entry permitting writes was chosen | A stranger's disk: a writable mount would mark it unclean merely by booting. |

The root is named, not searched for, so that a machine with an EXT2 disk does not
get it at the root by driver registration order. Without a ramdisk, the root
falls back to `VfsMountRoot`, the first volume found, under the stranger's rule.

## 5. Nodes

A node is a file's identity in the kernel: every route to one file yields the same
node. This is correctness, not economy. With copies, a write that grew the file
through one would leave another holding the old size and block pointers, and its
next write would restore them, truncating the file and orphaning the new blocks.
The same identity keeps a directory's link count right: creating a subdirectory
raises the parent's count through the parent's node, which every holder sees.

**Nodes in use, not nodes recently used.** A node is released when its last
reference goes. A retained node could describe a file since destroyed and its
inode reissued, and nothing here would know. Opening a file twice therefore reads
its inode twice, from the buffer cache. Every mount holds its root and the node
it covers for its lifetime, which is why `VfsMountIsBusy` compares the root's
references against one.

## 6. Open files

`VfsFile` is a node, a position and the open flags. The position belongs to the
open file, so two descriptors on one file read independently. An open file
counts its holders (`VfsHold` adds, `VfsClose` removes; the last releases it), so
a descriptor survives `fork`, `execve` and `dup2` as one file with one position.

- **An appending write goes to the end as it is at that moment**, not to the
  position, so two appenders never overwrite each other.
- **A directory's position is the filesystem's cookie.** The EXT2 binding packs
  the block index into the high half and the offset into the low half; an index
  too large to pack is refused, since a truncated one names another block and
  repeats entries.
- **A seek past the end is allowed** (a later write leaves a hole); before the
  start is refused. The two directions are computed separately so that neither
  wraps, and a negative offset is negated in the unsigned domain, because
  `INT64_MIN` has no positive counterpart.

## 7. Writing back

On every writable mount except the root, the buffer cache is synchronised when a
file opened for writing is closed and when `link`, `unlink`, `mkdir` or `rmdir`
returns (`VfsMountIsDurable`). Otherwise an edit reported saved would be lost
with an emulator's closed window. The root is memory, lasting no longer than the
cache, so syncing it would be wasted. [`PERSIST.md`](PERSIST.md).

## 8. Refusals

`VfsError` holds each refusal as a code, for a program, and as words, for a
diagnostic; both are set in one statement so they cannot disagree.

- **Read-only is tested once**, in `VfsWritable`, against the node's mount. A
  mount is read-only if it was asked to be or the filesystem judged the volume
  unwritable.
- **A file anything holds is not destroyed.** `VfsUnlink` and
  `VfsRemoveDirectory` refuse a node with references beyond the resolution's own.
  POSIX would keep the file alive until its last close, which needs the orphan
  list this kernel lacks; and `Ext2Unlink` frees the inode and blocks at the last
  name, so a reader would read blocks given to someone else. A mount point is
  refused by the same test.
- **A name and its file lie on one volume.** `VfsLink` refuses paths on different
  mounts: an entry can only name an inode of its own volume.

## 9. Pipes

A pipe is an open file with no node: `VfsFile` holds a `pipe` or a `node`, never
both. Everything a descriptor does (holder counts, `dup2`, release at a process's
end) is built on the open file, so a pipe end that were not one would need all of
it twice. `VfsRead`, `VfsWrite`, `VfsClose`, `VfsSeek`, `VfsTell`,
`VfsReadDirectory` and `VfsFileAttributes` check which before touching the node,
and the busy scan of an unmount skips files with no node.

`VfsPipeCreate` finds both file slots before claiming either, so a table with one
slot left that cannot grow refuses rather than making a read end with no write
end. A pipe's buffer is one page, from a table grown in chunks of
`VFS_PIPE_CHUNK` (8). Who sleeps and who wakes whom is in the header of
[`../../kernel/fs/vfs/pipe.c`](../../kernel/fs/vfs/pipe.c). A write with no
reader is refused as `VFS_ERROR_BROKEN_PIPE` (`EPIPE`) and sends `SIGPIPE`; a
sleep ended by a signal is `VFS_ERROR_INTERRUPTED` (`EINTR`).

## Verification

`KernelVerifyVfs` and its companions in
[`../../kernel/test/storage/vfs.c`](../../kernel/test/storage/vfs.c) use two
volumes composed in the two memory devices. The second is a copy of the first
with one field changed (the owner of `/file`), so an assertion can tell which
volume a path reached.

| Property asserted | The failure it would catch |
| ----------------- | -------------------------- |
| `/`, `/file`, `/sub`, `/sub/inner` resolve to their composed inodes and formats; sizes are as stated. | A lost component; a size joined wrongly. |
| `//sub//inner` equals `/sub/inner`; `/sub/` resolves and `/file/` is refused. | Empty components; a trailing separator asserting nothing. |
| `/.`, `/..`, `/sub/..`, `/sub/../file` resolve; the root's `..` is the root. | `.` and `..` interpreted instead of read. |
| Links are followed last, mid-path, fast and slow, and not followed when the name is asked for. | `stat` and `lstat` conflated. |
| Each refusal (absent, not a directory, relative, empty) names its own reason; an unused record neither resolves nor lists. | The right result by the wrong route; a deleted name read. |
| A file read through a descriptor matches its offset-derived contents; the position advances by what moved; a read at the end moves nothing and succeeds. | Wrong block; drifting position; end reported as error. |
| Seeks from all three origins land where sent; past the end is allowed, before the start refused. | A seek computed but not applied; a wrapped position. |
| Two descriptors on one file keep two positions. | The position kept on the node. |
| A directory is not read as a stream; a file is not opened as a directory; an open asking neither read nor write is refused; a double close is refused. | Records read as bytes; an open that can do nothing; a node released twice. |
| The root lists exactly six entries and the subdirectory three, and what lists is what resolves. | A lost or repeated record; listing and lookup disagreeing. |
| A file is created, written, closed, reopened and read back; an exclusive create of an existing file is refused. | A write to the wrong block; a create that opened what was there. |
| An appending write goes to the end; truncation down and up (a hole, reading zeroes) and truncating open both work. | Appenders overwriting; data kept that was discarded. |
| A second name raises the link count and removing one keeps the file; a directory cannot get a second name. | An unlink destroying a reachable file; a cycle. |
| A new directory has two links and its parent gains one, returned at removal; a non-empty directory is not removed. | A parent freed while a child names it. |
| An open file is not destroyed, and is once released. | Blocks freed under a reader. |
| Before a root nothing resolves and no other mount is allowed; bad mounts are refused with their codes. | A tree without a root; two superblocks of one volume. |
| A mount point shows the second volume after mounting; crossing paths reach it and others do not; the covered directory is unreachable. | A mount recorded but not applied, or merged. |
| `..` from the mounted root leaves it, and returning crosses again. | The prefix-matching failure. |
| A read-only mount refuses writes and creation; the root cannot be withdrawn while a mount or open file stands on it; the covered directory reappears intact. | A stranger's volume altered; descriptors to a vanished volume. |
| After a writable mount, the state **read from the medium** has the clean bit clear, the error bit clear and the mount count raised; after unmount, clean again. | A mark that never reached the disk; a volume falsely marked faulty. |
| Afterwards no node is held and no file open, and the re-read volume's descriptors verify. | A leak that exhausts a table; accounting drift. |
| Eight files more than the open-file table's first chunk open, the last reads its file, and all close; two pipes more than the pipe table's first chunk carry bytes and close. | A grown table handing out an entry beyond it, or one in use. |
| A pipe is two files and one pipe, released at the last close; bytes cross in order; each end does one thing and cannot seek. | A leaked slot; skipped bytes; a write end read as a file. |
| An empty pipe read by a caller that cannot sleep is refused as busy; a second holder keeps it open; a write with no reader is refused. | A wait for a writer that is the same flow; a child's close ending the parent's pipe; a writer told its bytes went somewhere. |

The descriptor table is checked after the unmount, since a mounted volume is
unclean and allowed to disagree with itself. A volume made by `mke2fs` is mounted
as the root at every boot ([`INITRD.md`](INITRD.md)), and
[`EXT2-VERIFICATION.md`](EXT2-VERIFICATION.md) covers writes judged by `e2fsck`.

## Limitations

1. Nothing is cached between uses; a file opened twice has its inode read twice.
2. An open file cannot be unlinked (Section 8).
3. No atomic rename; a move is a link and an unlink.
4. The EXT2 binding's refusal codes are approximate where the format's code
   distinguishes failures only in words. `VFS_ERROR_EXISTS` and
   `VFS_ERROR_NOT_EMPTY` are exact, established before the operation; "no space"
   is inferred from the free counts after a failure.
5. Permissions are recorded and not enforced; there are no users.
6. Times are not maintained ([`EXT2.md`](EXT2.md)).
7. `VfsUnmount` matches the path as given: a volume mounted at `/sub` cannot be
   withdrawn by naming `/sub/`.
8. Nothing is mounted from the command line; the placement of Section 4.2 is
   fixed.
9. ~~Fixed tables: 4 filesystem types, 4 mounts, 64 nodes, 32 open files, 8
   pipes.~~ **Closed on 2026-09-25**: the node, open-file and pipe tables grow
   from the heap in chunks, the first static so that a diagnostic can still be
   written with the heap exhausted
   ([`../design/MEMORY-LAYOUT.md`](../design/MEMORY-LAYOUT.md), Section 16). The
   4 filesystem types and 4 mounts stay fixed: nothing adds either at run time.
10. No lock. The mount, node and file tables need one, and node references must
    be atomic, before user threads leave the bootstrap processor
    ([`../design/CONCURRENCY.md`](../design/CONCURRENCY.md)).
