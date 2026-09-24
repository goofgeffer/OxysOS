<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# The Persistent `/etc`

**Phase**: 9, of 2026-09-23, between sub-tasks 9.7 and 9.8 of
[`../project/PLAN.md`](../project/PLAN.md) and before `Oxys 1 Beta`, at the
project owner's direction.

Section 1 is what was missing; Section 2 why `/etc` has a volume of its own;
Section 3 how the volume is found; Section 4 the volume left open; Section 5
the write-back; Section 6 the verification; Section 7 how a person makes and
uses one; Section 8 the limitations.

**Authority**: `PROJECT_GUIDELINES.md`, Sections 2, 3 and 6.

**Implementation**: [`../../kernel/fs/persist.c`](../../kernel/fs/persist.c)
and its header; the write-back in `VfsClose`,
[`../../kernel/fs/vfs/file.c`](../../kernel/fs/vfs/file.c); the release in
`KernelPower`, [`../../kernel/kernel.c`](../../kernel/kernel.c); the disk made by
[`../../tools/etc-disk.sh`](../../tools/etc-disk.sh). Asserted by
[`../../kernel/test/storage/persist.c`](../../kernel/test/storage/persist.c).

**Specifications**: The Second Extended File System, Dave Poirier — the
superblock's `s_volume_name` at offset 120, sixteen bytes, and `s_state`, whose
bit `EXT2_VALID_FS` says the volume was cleanly unmounted and whose bit
`EXT2_ERROR_FS` says errors were found. The mount beneath all of it is
[`VFS.md`](VFS.md).

## 1. What was missing

Since sub-task 7.7 the root is the initial ramdisk, and `/etc` is upon it. A
person could edit `/etc/session.conf` with `micro` and see the change the next
time the session started — and lose it at the next boot, the ramdisk being
memory rebuilt from the image each time. [`../design/CONFIG.md`](../design/CONFIG.md),
Section 7, limitation 3, had said so since sub-task 9.4.

**The owner's point was that `/etc` is the directory that must survive.** A
persistent `/home` or `/var` beside a ramdisk `/etc` would keep a person's files
and reset the settings they changed at every start — the settings being the
thing a person changes and then restarts to see.

## 2. A volume of its own, mounted over `/etc`

The volume is an EXT2 volume upon a disk of its own, mounted directly over the
ramdisk's `/etc` for writing. The ramdisk's `/etc` is hidden beneath it, as
every covered directory is, [`VFS.md`](VFS.md), Section 5.2.

**Not a directory of a larger volume bound onto `/etc`.** One disk holding
`/etc` and `/home` would need `/etc` bound to a directory of that disk. This
layer finds a mount through the node it covers and walks by node alone,
[`VFS.md`](VFS.md), Section 5.1 — so a bound directory would be one node reached
by two paths, `/etc` and the disk's own `etc`, and `..` from it could not know
which it had come by. `cd /etc; cd ..` would arrive in the disk's root rather
than at `/`, and nothing would say so. A layer that walked by mount and node
together could bind; this one would have to be rewritten to. A volume upon
`/etc` itself has one path, and `..` leaves it as every mount is left: **observed**
under QEMU, `cd /etc`, `cd ..` and `pwd` printing `/`.

## 3. Found by its label, before anything else

At start, after the ramdisk has taken the root and **before** the machine's
volume is mounted at `/mnt`, every block device but the ramdisk is read for an
EXT2 superblock whose `s_volume_name` is exactly `oxys-etc`. **The whole label,
and not a prefix**: a volume named `oxys-etc-old` is another volume.

This is the rule [`VFS.md`](VFS.md), Section 8.2, made for the root — chosen by
name, never by being found first — applied to a mount that is **written**. A
stranger's disk mounted over `/etc` for writing would be both of the failures
Section 8.1 of that document guards against at once: their disk marked as
opened, and this system's configuration read from whatever their disk held. A
disk without the label is left to be mounted at `/mnt`, read-only, as before;
the persistent volume is mounted first so that it is not taken for that one.

**Seeding.** Before the volume covers `/etc`, the regular files of the
ramdisk's `/etc` are read into memory — sixteen files of sixteen kilobytes at
most, where there are three of a few. Once it is mounted, each file the volume
lacks is written onto it, by an exclusive creation, so that the test and the
creation are one act. **Each file it has is left as it is**: overwriting it
would put the shipped configuration back over the edited one at every start,
which is the one thing the volume exists to prevent. A new file a later build
ships therefore appears upon the volume at the next start; a changed one does
not, Section 8.

**The shipped copies stay reachable**, since 2026-09-24. Each file of the
ramdisk's `/etc` is also staged read-only at `/share/defaults/etc`, which the
volume does not cover: before, the volume hid the only shipped copy, and
deleting a file and restarting was the only way to have it back — which a
person would not know to do. Now `cp` restores one at once, and the session
falls back to the shipped `session.conf` where the person's offers nothing to
launch, [`../design/SESSION.md`](../design/SESSION.md), Section 3.3.
`config-check` asserts that the copies are there, read without fault, and offer
a launcher.

## 4. A volume left open

A writable mount marks the volume as not cleanly unmounted for as long as it is
open, and the next mount of a volume so marked makes it read-only until a check
has been run, [`VFS.md`](VFS.md), Section 8. That is right for a stranger's disk.
Upon this one it would be the end of persistence: an emulator is most often
stopped by closing its window, the mark is then left behind, and **this system
has no checker** to run — so after the first such stop a person's configuration
could never be edited again.

So a volume labelled `oxys-etc`, found not cleanly unmounted **with no error
recorded upon it**, is marked clean before it is mounted, and the boot report
says so: `It had not been cleanly unmounted, and was marked clean.` A volume
with `EXT2_ERROR_FS` set — something found a fault in it — is mounted read-only,
and the report says that instead. `tools/etc-disk.sh check` runs the build
host's `e2fsck` over the image, and `repair` lets it mend what it safely can.

**Found by running it, and recorded.** The first form set the bit and wrote the
superblock — and the write was refused without a word, because the reader had
already made the superblock read-only for being unclean and the writer refuses
a read-only volume. The next start mounted the volume read-only and blamed
errors that were not there. The flag is now lifted for that one write, and only
where no feature the kernel cannot write is in use — the reader's other reason,
which this does not overrule — and the report names the true reason for a
read-only mount.

## 5. Written back, and released

**The buffer cache is written back when a file written upon the volume is
closed.** The cache holds a write until something syncs it, so without this an
edit saved in `micro` and then lost to a closed window would be a file the
person had been told was saved. `VfsClose` syncs when the descriptor was opened
for writing upon any writable mount but the root; the ramdisk at the root is
memory and lasts no longer than the cache, so it is not synced for nothing.
**Observed**: a line appended to `/etc/desktop.conf`, the emulator then killed,
and the line there at the next start, `e2fsck` finding the volume consistent.

**A name made or removed is written back when the call returns**, since
2026-09-24: `link`, `unlink`, `mkdir` and `rmdir` upon any writable mount but
the root sync as a close does. It was needed by `micro`, which since the same
day saves a file whole into a file beside it and moves the name across with
`unlink` and `link` — so that a save which fails part way leaves the edited
file as it was — and a move left in the buffer cache would be lost with the
emulator's window, the edit with it.

**At the power call** the volume is synced and unmounted before the machine
stops, so that it is marked clean. **Observed**: after `shutdown` the build
host's `debugfs` read `Filesystem state: clean`. Where something still holds a
file upon it the unmount is refused, everything outstanding is written back
regardless, and the next start marks it clean, Section 4.

## 6. Verification

The self-test runs inside the virtual filesystem's own, upon its root volume and
its second device of memory. **It never searches for `oxys-etc`**: a machine
booted with its persistent disk would have that disk found and written upon by
the test. It searches instead for a label no disk is given, `oxys-selftest`,
and mounts the device it composed. **Observed** under QEMU with the owner's disk
attached: the self-test passed, the disk mounted over `/etc` after it, and the
three files upon the disk were byte for byte what they had been.

| Property asserted | The silent failure it would catch |
| ----------------- | --------------------------------- |
| The volume is found by its whole label, and not by a prefix of it nor by a label it is a prefix of | A volume named `oxys-etc-old`, or `oxys`, mounted over `/etc` for writing. **Observed** as a damage |
| A volume left open, with no error recorded, is marked clean before it is mounted, and is mounted writable | A configuration read-only for ever after the first time the machine was stopped by closing its window. **Observed** while it was written, Section 4 |
| One file the volume lacked is seeded and one it had is kept; the seeded file reads as the covered directory's, and the kept one does not | The shipped configuration written over the edited one at every start. **Observed** as a damage |
| A file written upon the volume is upon the medium — the store behind the device, which the buffer cache does not reach — once it is closed | An edit lost when the emulator is closed. **Observed** as a damage |
| A directory made and removed, and a file unlinked, upon the volume leave no dirty buffer once the call returns, the cache having been emptied before | A save by `micro`, which moves a name, lost with a closed window after it was reported written. **Observed** as a damage |
| Released, the volume is clean upon the medium, and the covered directory shows again | A volume demanding a check at every start; a mount that would not come away |

### 6.1 The damage applied, and what the test said

In one build, reverted: the label's final comparison made to accept anything
that matched so far, the seeding's exclusive creation made a truncating one,
and the write-back upon a close removed. The run said `a volume was found by a
prefix of its label`, `the seeding did not copy one file and keep one` and `a
file the volume had was overwritten by the covered directory's`, and `a file
closed upon the volume was not written back to the medium`. `Persistent /etc
self-test FAILED.` The label longer than the volume's was still refused, as the
code says it would be: it differs at a character inside the loop, before the
comparison that was damaged.

### 6.2 In each environment

| Environment | How the disk was attached | First start | Second start |
| ----------- | ------------------------- | ----------- | ------------ |
| QEMU q35 | `-drive …,if=ide`, which q35 puts upon its AHCI controller: `ahci0` | 3 seeded; a line appended, the emulator killed | 3 kept, marked clean, the line there; a second line, `shutdown`, the volume clean; a third start with both lines and nothing to mark |
| VirtualBox | The image converted to a VDI and attached as the IDE primary slave: `ata1` | 3 seeded; powered off | 3 kept, marked clean |
| Bochs | `ata0-slave`, flat: `ata1` | 3 seeded; stopped | 3 kept, marked clean |

## 7. Making and using one

```sh
tools/etc-disk.sh create                     # ~/oxys-disks/oxys-etc.img, empty
make run-qemu ETC_DISK=~/oxys-disks/oxys-etc.img
tools/etc-disk.sh list                       # what is upon it
tools/etc-disk.sh check                      # e2fsck, altering nothing
```

The image is kept **outside the repository**: under `build/`, `make clean` would
remove a person's settings; inside the tree, one `git add -A` would commit them.
`create` refuses an image that exists, and refuses a path inside the repository.
To go back to the shipped configuration, delete a file upon the volume — the
next start seeds it again — or remove the image and create another.

**No make target was added.** `PROJECT_GUIDELINES.md`, Section 3, lists every
target and `make lint` holds the Makefile to the list; a target is an amendment
of that document, which is the owner's to make. `ETC_DISK` is a variable of the
existing `run-qemu`.

## 8. Limitations

1. **Only `/etc`.** A persistent `/home` needs a second volume, and a second
   volume upon the same disk needs partitions, which the block layer does not
   read. Section 2 is why it is not a directory of this one.
2. **A changed shipped file does not reach an existing volume**, Section 3. A
   person who has not edited `session.conf` and updates the system keeps the
   old one until they delete it. Telling an unedited file from an edited one
   would need a record of what was shipped, which nothing keeps yet. **Since
   2026-09-24 the shipped copy is reachable without a restart**: every file is
   also at `/share/defaults/etc`, which the volume does not cover, so
   `cp /share/defaults/etc/session.conf /etc/session.conf` takes it back at once,
   Section 3.
3. **A volume left open is marked clean without a check**, Section 4, upon the
   judgement that three configuration files not checked are better than three
   that can never be edited. A file being written as the machine stopped may be
   torn; deleting it restores the shipped one.
4. **Directories under `/etc` are not seeded**, only its regular files. There
   are none.
5. **`lost+found` stands in `/etc`**, `mke2fs` having made it; `e2fsck` puts
   what it recovers there.
6. **Written back upon a close, or a change of name.** A file held open and
   written for minutes is upon the medium when it is closed, or when the
   machine is shut down. A name made or removed is upon it when the call
   returns, since 2026-09-24, Section 5.
