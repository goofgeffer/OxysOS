/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/fs/persist.c
 * Purpose: Implements the persistent `/etc`: the volume labelled `oxys-etc`
 *          found upon a disk, mounted over the ramdisk's `/etc`, seeded with
 *          what it lacks of it, and released cleanly when the machine stops.
 * Key functions: PersistMountByLabel, PersistRelease, PersistEtc,
 *          PersistReportWrite.
 * References:
 *   - kernel/include/oxys/fs/persist.h: why a volume of its own, and why by
 *     label.
 *   - docs/storage/PERSIST.md: the design, and every assertion made upon it.
 *   - The Second Extended File System, Dave Poirier: `s_volume_name` and
 *     `s_state`.
 *
 * Concurrency. It runs at start, before any program, and at the power call,
 * after every other processor has been stopped; the layers it calls are the
 * VFS's and the EXT2 driver's, which document their own.
 */

#include <oxys/fs/persist.h>
#include <oxys/fs/vfs.h>
#include <oxys/fs/ext2.h>
#include <oxys/block/block.h>
#include <oxys/block/buffer.h>
#include <oxys/dev/storage/ramdisk.h>
#include <oxys/kernel.h>

/* The files of the covered directory, read before it is covered. Static,
 * because they are a quarter of a megabyte at most and read once. */
typedef struct PersistSeed
{
    char name[VFS_NAME_MAXIMUM + 1U];
    uint16_t permissions;
    uint64_t length;
    uint8_t bytes[PERSIST_SEED_FILE_BYTES];
} PersistSeed;

static PersistSeed PersistSeeds[PERSIST_SEED_FILES_MAXIMUM];
static size_t PersistSeedCount;

static PersistReport PersistEtcReport;

static size_t PersistLength(const char *text)
{
    size_t length = 0U;

    while (text[length] != '\0')
    {
        ++length;
    }

    return length;
}

static void PersistCopy(char *destination, size_t capacity, const char *source)
{
    size_t index = 0U;

    while ((source[index] != '\0') && ((index + 1U) < capacity))
    {
        destination[index] = source[index];
        ++index;
    }

    destination[index] = '\0';
}

/* Whether the superblock names itself `label` — the whole of its sixteen bytes,
 * and not a prefix: `oxys-etc-old` is another volume. */
static bool PersistLabelIs(const Ext2Superblock *volume, const char *label)
{
    size_t index = 0U;

    while ((label[index] != '\0') && (index < EXT2_VOLUME_NAME_LENGTH))
    {
        if (volume->volume_name[index] != label[index])
        {
            return false;
        }

        ++index;
    }

    return (label[index] == '\0') &&
           ((index == EXT2_VOLUME_NAME_LENGTH) || (volume->volume_name[index] == '\0'));
}

/*
 * Reads the regular files of `point` into PersistSeeds. A file larger than a
 * seed holds, or beyond the count, is counted as skipped: seeding half of a
 * configuration file would be worse than seeding none of it, since the half
 * parses.
 */
static void PersistReadSeeds(const char *point, PersistReport *report)
{
    const int directory = VfsOpen(point, VFS_OPEN_READ | VFS_OPEN_DIRECTORY, 0U);
    VfsDirectoryEntry entry;
    bool end = false;

    PersistSeedCount = 0U;

    if (directory < 0)
    {
        return;
    }

    while (VfsReadDirectory(directory, &entry, &end) && !end)
    {
        char path[VFS_PATH_MAXIMUM + 1U];
        VfsAttributes attributes;
        PersistSeed *seed;
        uint64_t taken = 0U;
        int file;

        if ((entry.name[0] == '.') &&
            ((entry.name[1] == '\0') || ((entry.name[1] == '.') && (entry.name[2] == '\0'))))
        {
            continue;
        }

        if ((PersistLength(point) + 1U + PersistLength(entry.name)) > VFS_PATH_MAXIMUM)
        {
            ++report->skipped;
            continue;
        }

        PersistCopy(path, sizeof path, point);
        PersistCopy(&path[PersistLength(path)], sizeof path - PersistLength(path), "/");
        PersistCopy(&path[PersistLength(path)], sizeof path - PersistLength(path), entry.name);

        if (!VfsStat(path, &attributes) || (attributes.type != VFS_NODE_REGULAR))
        {
            continue;
        }

        if ((PersistSeedCount == PERSIST_SEED_FILES_MAXIMUM) ||
            (attributes.size > PERSIST_SEED_FILE_BYTES))
        {
            ++report->skipped;
            continue;
        }

        seed = &PersistSeeds[PersistSeedCount];
        file = VfsOpen(path, VFS_OPEN_READ, 0U);

        if ((file < 0) || !VfsRead(file, seed->bytes, attributes.size, &taken) ||
            (taken != attributes.size))
        {
            if (file >= 0)
            {
                (void)VfsClose(file);
            }

            ++report->skipped;
            continue;
        }

        (void)VfsClose(file);
        PersistCopy(seed->name, sizeof seed->name, entry.name);
        seed->permissions = attributes.permissions;
        seed->length = taken;
        ++PersistSeedCount;
    }

    (void)VfsClose(directory);
}

/* Writes each seed the mounted volume lacks; one it has is kept as it is. */
static void PersistWriteSeeds(const char *point, PersistReport *report)
{
    for (size_t index = 0U; index < PersistSeedCount; ++index)
    {
        const PersistSeed *const seed = &PersistSeeds[index];
        char path[VFS_PATH_MAXIMUM + 1U];
        uint64_t written = 0U;
        int file;

        PersistCopy(path, sizeof path, point);
        PersistCopy(&path[PersistLength(path)], sizeof path - PersistLength(path), "/");
        PersistCopy(&path[PersistLength(path)], sizeof path - PersistLength(path), seed->name);

        /*
         * Created only where absent, by the exclusive open, so that the test
         * and the creation are one act: a file the person edited is never
         * overwritten by the one the build shipped, which is the whole reason
         * the volume exists.
         */
        file = VfsOpen(path, VFS_OPEN_WRITE | VFS_OPEN_CREATE | VFS_OPEN_EXCLUSIVE,
                       seed->permissions);

        if (file < 0)
        {
            ++report->kept;
            continue;
        }

        if (VfsWrite(file, seed->bytes, seed->length, &written) && (written == seed->length))
        {
            ++report->seeded;
        }
        else
        {
            ++report->skipped;
        }

        (void)VfsClose(file);
    }
}

BlockDevice *PersistFindByLabel(const char *label)
{
    const BlockDevice *const ramdisk = RamdiskDevice();
    Ext2Superblock volume;

    if (label == NULL)
    {
        return NULL;
    }

    for (size_t index = 0U; index < BlockDeviceCount(); ++index)
    {
        BlockDevice *const device = BlockDeviceAt(index);

        if ((device == NULL) || (device == ramdisk))
        {
            continue;
        }

        if (Ext2ReadSuperblock(device, &volume) && PersistLabelIs(&volume, label))
        {
            return device;
        }
    }

    return NULL;
}

bool PersistMountByLabel(const char *label, const char *point, PersistReport *report)
{
    BlockDevice *const chosen = PersistFindByLabel(label);

    if (report != NULL)
    {
        *report = (PersistReport){ 0 };
    }

    if ((chosen == NULL) || (point == NULL) || (report == NULL))
    {
        return false;
    }

    return PersistMountDevice(chosen, point, report);
}

bool PersistMountDevice(BlockDevice *chosen, const char *point, PersistReport *report)
{
    Ext2Superblock volume;

    if ((chosen == NULL) || (point == NULL) || (report == NULL))
    {
        return false;
    }

    *report = (PersistReport){ 0 };

    if (!Ext2ReadSuperblock(chosen, &volume))
    {
        return false;
    }

    report->found = true;
    PersistCopy(report->device, sizeof report->device, chosen->name);

    /*
     * A volume left open by a machine that stopped without shutting down is
     * marked clean here rather than mounted read-only, PERSIST.md:
     * the read-only rule waits for a check this system cannot run, and a
     * configuration that can never again be edited is a worse failure than the
     * one the rule guards against, upon a volume of three small files that this
     * system made. A volume with errors *recorded* upon it is another matter —
     * something found a fault — and is left read-only.
     */
    if ((volume.state & EXT2_ERROR_FS) == 0U)
    {
        /*
         * The reader made the volume read-only for being unclean, and the
         * writer refuses a read-only volume, so the flag is lifted for this one
         * write — but only where no feature this kernel cannot write is in use,
         * which is the reader's other reason and one this does not overrule.
         * Found by running it: the first form wrote nothing and said nothing,
         * and the next start mounted the volume read-only.
         */
        if (((volume.state & EXT2_VALID_FS) == 0U) &&
            ((volume.feature_read_only & ~(uint32_t)EXT2_FEATURES_RO_COMPAT_SUPPORTED) == 0U))
        {
            volume.read_only = false;
            volume.state |= (uint16_t)EXT2_VALID_FS;

            if (Ext2WriteSuperblock(chosen, &volume) && BufferSync())
            {
                report->marked_clean = true;
            }
        }
    }

    PersistReadSeeds(point, report);

    if (!VfsMountVolume(chosen->name, point, "ext2", false))
    {
        return false;
    }

    report->mounted = true;
    report->errors = (volume.state & EXT2_ERROR_FS) != 0U;
    report->read_only = report->errors;

    for (size_t index = 0U; index < VfsMountCount(); ++index)
    {
        const VfsMount *const mount = VfsMountAt(index);

        if ((mount != NULL) && mount->mounted && (mount->device == chosen))
        {
            report->read_only = mount->read_only;
        }
    }

    if (!report->read_only)
    {
        PersistWriteSeeds(point, report);
        (void)VfsSync();
    }

    return true;
}

bool PersistRelease(const char *point)
{
    (void)VfsSync();

    return VfsUnmount(point);
}

bool PersistEtc(void)
{
    const bool mounted =
        PersistMountByLabel(PERSIST_ETC_LABEL, PERSIST_ETC_POINT, &PersistEtcReport);

    PersistReportWrite(PERSIST_ETC_POINT, &PersistEtcReport);

    return mounted;
}

void PersistReportWrite(const char *point, const PersistReport *report)
{
    if ((report == NULL) || !report->found)
    {
        KernelWriteString("Persistent ");
        KernelWriteString(point);
        KernelWriteString(": no volume labelled " PERSIST_ETC_LABEL
                          "; the ramdisk's copy is used, and edits last until the "
                          "machine stops.\n");

        return;
    }

    KernelWriteString("Persistent ");
    KernelWriteString(point);
    KernelWriteString(": the volume " PERSIST_ETC_LABEL " upon ");
    KernelWriteString(report->device);

    if (!report->mounted)
    {
        KernelWriteString(" could not be mounted; the ramdisk's copy is used.\n");

        return;
    }

    KernelWriteString(!report->read_only ? " is mounted"
                      : report->errors   ? " is mounted read-only, errors being recorded upon it"
                                         : " is mounted read-only, not having been marked clean");
    KernelWriteString("; ");
    KernelWriteDecimal(report->seeded);
    KernelWriteString(" file(s) seeded, ");
    KernelWriteDecimal(report->kept);
    KernelWriteString(" kept as edited, ");
    KernelWriteDecimal(report->skipped);
    KernelWriteString(" skipped.");

    if (report->marked_clean)
    {
        KernelWriteString(" It had not been cleanly unmounted, and was marked clean.");
    }

    KernelWriteString("\n");
}
