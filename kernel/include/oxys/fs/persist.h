/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/include/oxys/fs/persist.h
 * Purpose: Declares the persistent `/etc`: a volume upon a disk, recognised by
 *          its label, mounted over the ramdisk's `/etc` at start and seeded from
 *          it, so that a configuration a person edits survives a restart.
 * Key definitions: PERSIST_ETC_LABEL, PERSIST_ETC_POINT, PersistReport,
 *          PersistMountByLabel, PersistFindByLabel, PersistMountDevice,
 *          PersistRelease, PersistEtc, PersistReportWrite.
 * References:
 *   - docs/storage/PERSIST.md: the design, and every assertion made upon it.
 *   - docs/storage/VFS.md: how a mount covers a directory,
 *     and the mark a writable mount leaves upon a volume.
 *   - The Second Extended File System, Dave Poirier, the superblock's
 *     `s_volume_name` (offset 120, sixteen bytes) and `s_state`, whose two bits
 *     EXT2_VALID_FS and EXT2_ERROR_FS say that the volume was cleanly unmounted
 *     and that errors were found in it.
 *
 * Why a volume of its own, over `/etc`, and not a directory of a larger one.
 *
 *   One disk holding `/etc` and `/home` would need `/etc` bound to a directory
 *   of that disk. This layer walks by node alone, as VFS.md explains, so the
 *   directory would be one node reached by two paths, and `..` from it could
 *   not tell which it came by — `cd /etc; cd ..` would arrive in the disk's
 *   root rather than at `/`, silently. A volume mounted directly upon `/etc`
 *   has no second path, and `..` leaves it the way every mount is left.
 *
 * Why by label.
 *
 *   The root is chosen by name, as VFS.md explains, so that a stranger's disk
 *   is never mounted where this system's files belong; the same reasoning
 *   applies twice over to a volume mounted for writing over `/etc`. Only a
 *   volume whose superblock names itself PERSIST_ETC_LABEL is taken, and a disk
 *   without that label is left to be mounted at `/mnt`, read-only, as before.
 */

#ifndef OXYS_FS_PERSIST_H
#define OXYS_FS_PERSIST_H

#include <oxys/types.h>

#define PERSIST_ETC_LABEL "oxys-etc"
#define PERSIST_ETC_POINT "/etc"

/* The most files seeded into a fresh volume, and the most bytes of each. The
 * ramdisk's `/etc` is three files of a few kilobytes; a file larger than this
 * is left unseeded and said to be. */
#define PERSIST_SEED_FILES_MAXIMUM 16U
#define PERSIST_SEED_FILE_BYTES    (16U * 1024U)

/* What a persistent mount did, for the boot report and the self-test. */
typedef struct PersistReport
{
    bool found;        /* A volume with the label was upon some device. */
    bool mounted;      /* It was mounted over the point. */
    bool read_only;    /* It was mounted, but read-only. */
    bool errors;       /* Errors were recorded upon it, which is why. */
    bool marked_clean; /* It had not been cleanly unmounted, and was marked so. */
    size_t seeded;     /* Files copied onto it from the directory it covers. */
    size_t kept;       /* Files it already had, left as they were. */
    size_t skipped;    /* Files too large, or too many, to seed. */
    char device[32];
} PersistReport;

/*
 * Finds the volume labelled `label` upon any device but the ramdisk, and mounts
 * it over the directory `point`, having first read the regular files of that
 * directory; each of them the volume lacks is then written onto it, and each it
 * has is left alone. Returns whether a volume was mounted, and fills `report`
 * either way.
 *
 * A volume found not cleanly unmounted, with no errors recorded, is marked
 * clean before it is mounted — PERSIST.md says why this and only
 * this volume is — and one with errors recorded is mounted read-only.
 */
bool PersistMountByLabel(const char *label, const char *point, PersistReport *report);

/* The two halves of the above: the device carrying the volume labelled `label`,
 * never the ramdisk, or null; and the mount of a device already chosen. They
 * are apart so that the self-test mounts the device it composed, and searches
 * only for a label no real disk carries — a self-test that searched for
 * PERSIST_ETC_LABEL upon a machine booted with its `/etc` disk would find that
 * disk and write its test files upon it. */
struct BlockDevice;
struct BlockDevice *PersistFindByLabel(const char *label);
bool PersistMountDevice(struct BlockDevice *device, const char *point, PersistReport *report);

/*
 * Writes back and releases the volume mounted over `point`, so that it is
 * marked cleanly unmounted before the machine stops. Where something still
 * holds a file upon it the unmount is refused, and everything outstanding is
 * written back regardless. Returns whether it was unmounted.
 */
bool PersistRelease(const char *point);

/* The two above for `/etc` and the label this system gives its volume, and the
 * report of what was done. */
bool PersistEtc(void);
void PersistReportWrite(const char *point, const PersistReport *report);

#endif /* OXYS_FS_PERSIST_H */
