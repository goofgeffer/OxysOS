<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# The Persistent `/etc`

**Phase**: between sub-tasks 9.7 and 9.8 of
[`../project/PLAN.md`](../project/PLAN.md), at the project owner's direction.
**Source**: [`../../kernel/fs/persist.c`](../../kernel/fs/persist.c) and its
header; the write-back in [`../../kernel/fs/vfs/file.c`](../../kernel/fs/vfs/file.c)
and the namespace operations of the VFS; the release in `KernelPower`,
[`../../kernel/kernel.c`](../../kernel/kernel.c); the disk tool
[`../../tools/etc-disk.sh`](../../tools/etc-disk.sh).
**Specifications**: The Second Extended File System (Dave Poirier): the
superblock's `s_volume_name` (offset 120, 16 bytes) and `s_state`
(`EXT2_VALID_FS`, `EXT2_ERROR_FS`).

The root is the initial ramdisk ([`INITRD.md`](INITRD.md)), rebuilt from the
image at every boot. An EXT2 volume labelled `oxys-etc`, on a disk of its own,
is mounted writable over the ramdisk's `/etc`, so that configuration edited on
the running machine survives a restart. `/etc` comes first because it is what a
person changes and then restarts to see; a persistent `/home` beside a volatile
`/etc` would keep the files and reset the settings.

## 1. A volume of its own

The volume is mounted directly over `/etc`, hiding the ramdisk's `/etc` as any
mount hides the directory it covers ([`VFS.md`](VFS.md)).

It is not a directory of a larger volume bound onto `/etc`. The VFS finds a
mount through the node it covers and walks by node alone, so a bound directory
would be one node reachable by two paths, and `..` from it could not know which
path it came by: `cd /etc; cd ..` would land in the disk's root, silently. A
volume mounted on `/etc` has one path, and `..` leaves it as any mount is left.

## 2. Finding the volume

At start, after the ramdisk becomes the root and before any disk is mounted at
`/mnt`, every block device but the ramdisk is read for an EXT2 superblock whose
`s_volume_name` is exactly `oxys-etc`.

- **The whole label, not a prefix.** `oxys-etc-old` is another volume.
- **Chosen by name, never by being first**, the rule [`VFS.md`](VFS.md) applies
  to the root. This mount is written to: a stranger's disk mounted here would
  have their disk marked opened and this system's configuration read from their
  data.
- **Before `/mnt`.** A disk without the label is mounted at `/mnt` read-only;
  the labelled one is claimed first so it is not taken for that.

## 3. Seeding

Before the volume covers `/etc`, the ramdisk's regular files in `/etc` are read
into memory (at most 16 files of 16 KiB). Once mounted, each file the volume
lacks is created on it exclusively, so that the test and the creation are one
act. **A file the volume already has is never overwritten**: that would restore
the shipped configuration over the edited one at every start.

**The shipped copies stay reachable** at `/share/defaults/etc`, read-only on the
ramdisk and not covered by the volume. `cp /share/defaults/etc/session.conf /etc/`
restores one at once, and the session falls back to the shipped `session.conf`
when the person's offers nothing to launch ([`../design/SESSION.md`](../design/SESSION.md)).

## 4. A volume left open

A writable mount marks a volume not cleanly unmounted while it is open, and the
next mount of such a volume is read-only until it is checked
([`VFS.md`](VFS.md)). That is right for a stranger's disk. Here it would end
persistence: an emulator is usually stopped by closing its window, and this
system has no checker, so the configuration could never be edited again.

- A volume labelled `oxys-etc`, left open, **with no error recorded**, is marked
  clean before it is mounted; the boot report says
  `It had not been cleanly unmounted, and was marked clean.`
- The read-only flag the reader set for the unclean state is lifted for that one
  superblock write, and only when no feature the kernel cannot write is in use;
  otherwise the write would be refused and the mount would fall to read-only for
  a false reason.
- A volume with `EXT2_ERROR_FS` set is mounted read-only, and the report says
  so. `tools/etc-disk.sh check` runs the host's `e2fsck` over the image;
  `repair` lets it mend what it safely can.

## 5. Write-back and release

| When | What is written back | Why |
| ---- | -------------------- | --- |
| A descriptor opened for writing is closed (`VfsClose`) | The buffer cache | An edit saved and then lost to a closed window would be a file the person was told was saved. |
| `link`, `unlink`, `mkdir` or `rmdir` returns | The buffer cache | `micro` saves into a file beside the target and moves the name with `unlink` and `link`; a move left in the cache is lost with the window, and the edit with it. |
| The power call | Sync, then unmount | The volume is left marked clean. If a file is still held, the unmount is refused, everything is written back regardless, and the next start marks it clean (Section 4). |

The rule applies to every writable mount but the root: the ramdisk is memory and
lasts no longer than the cache, so syncing it is wasted work.

## 6. Making and using a disk

```sh
tools/etc-disk.sh create                     # ~/oxys-disks/oxys-etc.img, empty
make run-qemu ETC_DISK=~/oxys-disks/oxys-etc.img
tools/etc-disk.sh list                       # the files on it
tools/etc-disk.sh check                      # e2fsck, read-only
```

- The image lives **outside the repository**: under `build/`, `make clean` would
  delete a person's settings; inside the tree, `git add -A` would commit them.
  `create` refuses an existing image and a path inside the repository.
- To return to the shipped configuration, copy from `/share/defaults/etc`,
  delete the file on the volume (the next start seeds it), or create a new image.
- `ETC_DISK` is a variable of `run-qemu`, not a target of its own.
- Under VirtualBox, convert the image to VDI and attach it as the IDE primary
  slave; under Bochs, attach it flat as `ata0-slave`. The machine finds it by
  label on any controller ([`../project/TESTING.md`](../project/TESTING.md)).

## Verification

`KernelVerifyPersist` in [`../../kernel/test/storage/persist.c`](../../kernel/test/storage/persist.c)
runs inside the VFS self-test, on a device made of memory labelled
`oxys-selftest`. It never searches for `oxys-etc`: on a machine booted with its
real disk, the test would find and write to it.

| Property asserted | The failure it would catch |
| ----------------- | -------------------------- |
| The volume is found by its whole label, not by a prefix of it nor a label it prefixes. | `oxys-etc-old` or `oxys` mounted over `/etc` for writing. |
| A volume left open with no error is marked clean and mounted writable. | Configuration read-only for ever after the first closed window. |
| A file the volume lacks is seeded; one it has is kept, with its own contents. | The shipped configuration written over the edited one at every start. |
| A file closed on the volume is on the medium, beyond the buffer cache. | An edit lost when the emulator is closed. |
| A directory made and removed, and a file unlinked, leave no dirty buffer when the call returns. | A save by `micro` lost with a closed window after it was reported. |
| Released, the volume is clean on the medium and the covered directory shows again. | A volume demanding a check at every start; a mount that will not come away. |

`config-check` asserts that the shipped copies at `/share/defaults/etc` are
present, readable, and offer a launcher.

## Limitations

1. Only `/etc` persists. A persistent `/home` needs a second volume, and a second
   volume on the same disk needs partitions, which the block layer does not read
   ([`BLOCK.md`](BLOCK.md)).
2. A shipped file that changes does not reach an existing volume: nothing records
   which files were edited. The new copy is at `/share/defaults/etc`.
3. A volume left open is marked clean without a check. A file being written as
   the machine stopped may be torn; deleting it restores the shipped copy.
4. Only regular files are seeded, not directories; `/etc` has none.
5. `lost+found`, made by `mke2fs`, appears in `/etc`.
6. A file held open and written for a long time reaches the medium only when it
   is closed or the machine shuts down.
