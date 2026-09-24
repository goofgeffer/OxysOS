/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/test/storage/persist.c
 * Purpose: Asserts the persistent `/etc` upon the second volume of the fixture
 *          of kernel/test/volume.c: found by its label and by nothing less,
 *          marked clean when it was left open, mounted over a directory it
 *          seeds without overwriting, written back when a file upon it is
 *          closed, and released clean.
 * Key functions: KernelVerifyPersist.
 * References:
 *   - kernel/include/oxys/fs/persist.h: what is asserted, and why the test
 *     mounts the device it composed rather than searching for the label a
 *     real disk would carry.
 *   - docs/storage/PERSIST.md: every assertion here paired with the
 *     silent failure it would catch.
 *
 * It runs inside the virtual filesystem's self-test, upon its root volume and
 * its second device, and puts back what it altered before that test goes on.
 */

#include <oxys/kernel.h>
#include <oxys/test/verify.h>
#include <oxys/fs/persist.h>
#include <oxys/fs/vfs.h>
#include <oxys/fs/ext2.h>
#include <oxys/block/buffer.h>

#include "../volume.h"

/* A label no disk this system is given will carry, so that a search for it
 * finds the composed volume and nothing a person owns. */
#define VERIFY_PERSIST_LABEL "oxys-selftest"
#define VERIFY_PERSIST_POINT "/persist"

static bool VerifyPersistSucceeded;

static void VerifyPersistRequire(bool condition, const char *statement)
{
    if (!condition)
    {
        KernelWriteString("  ");
        KernelWriteString(statement);
        KernelWriteString("\n");
        VerifyPersistSucceeded = false;
    }
}

/* Writes `text` as the whole of the file at `path`, creating it. */
static bool VerifyPersistWrite(const char *path, const char *text, uint32_t flags)
{
    const int file = VfsOpen(path, VFS_OPEN_WRITE | flags, 0644U);
    uint64_t length = 0U;
    uint64_t written = 0U;
    bool outcome;

    while (text[length] != '\0')
    {
        ++length;
    }

    if (file < 0)
    {
        return false;
    }

    outcome = VfsWrite(file, text, length, &written) && (written == length);

    return VfsClose(file) && outcome;
}

/* Whether the file at `path` begins with `text`. */
static bool VerifyPersistBegins(const char *path, const char *text)
{
    char buffer[64];
    const int file = VfsOpen(path, VFS_OPEN_READ, 0U);
    uint64_t length = 0U;
    uint64_t read = 0U;
    bool same;

    while (text[length] != '\0')
    {
        ++length;
    }

    if ((file < 0) || (length > sizeof buffer))
    {
        return false;
    }

    same = VfsRead(file, buffer, length, &read) && (read == length);

    for (uint64_t index = 0U; same && (index < length); ++index)
    {
        same = buffer[index] == text[index];
    }

    (void)VfsClose(file);

    return same;
}

/* Whether the medium itself — the store behind the device, not the buffer
 * cache — holds `text` anywhere. */
static bool VerifyPersistUponMedium(const uint8_t *store, size_t size, const char *text)
{
    size_t length = 0U;

    while (text[length] != '\0')
    {
        ++length;
    }

    for (size_t at = 0U; (at + length) <= size; ++at)
    {
        size_t matched = 0U;

        while ((matched < length) && (store[at + matched] == (uint8_t)text[matched]))
        {
            ++matched;
        }

        if (matched == length)
        {
            return true;
        }
    }

    return false;
}

static uint16_t VerifyPersistState(const uint8_t *store)
{
    return (uint16_t)(store[EXT2_SUPERBLOCK_OFFSET + EXT2_OFFSET_STATE] |
                      ((uint16_t)store[EXT2_SUPERBLOCK_OFFSET + EXT2_OFFSET_STATE + 1U] << 8));
}

void KernelVerifyPersist(BlockDevice *device, uint8_t *store, size_t size)
{
    uint8_t saved_label[EXT2_VOLUME_NAME_LENGTH];
    const size_t label_at = EXT2_SUPERBLOCK_OFFSET + EXT2_OFFSET_VOLUME_NAME;
    const size_t state_at = EXT2_SUPERBLOCK_OFFSET + EXT2_OFFSET_STATE;
    const uint8_t saved_state = store[state_at];
    PersistReport report;

    VerifyPersistSucceeded = true;

    KernelWriteString("Persistent /etc: asserting the label, the seeding, the write-back and "
                      "the release upon a volume in memory.\n");

    /*
     * The second volume is given the test's label, and left as though a
     * machine had stopped with it open: the clean bit cleared, no error bit.
     */
    for (size_t index = 0U; index < EXT2_VOLUME_NAME_LENGTH; ++index)
    {
        saved_label[index] = store[label_at + index];
        store[label_at + index] =
            (index < (sizeof VERIFY_PERSIST_LABEL - 1U)) ? (uint8_t)VERIFY_PERSIST_LABEL[index] : 0U;
    }

    store[state_at] = (uint8_t)(saved_state & (uint8_t)~(uint8_t)EXT2_VALID_FS);
    (void)BufferInvalidateDevice(device);

    /*
     * **By the whole label and by nothing less.** A search that took a prefix
     * would mount a volume named `oxys-etc-old` over `/etc`; one that ignored
     * the label would mount a stranger's disk there, for writing.
     */
    VerifyPersistRequire(PersistFindByLabel(VERIFY_PERSIST_LABEL) == device,
                         "the volume carrying the label was not found by it");
    VerifyPersistRequire(PersistFindByLabel("oxys-selftes") == NULL,
                         "a volume was found by a prefix of its label");
    VerifyPersistRequire(PersistFindByLabel("oxys-selftest-x") == NULL,
                         "a volume was found by a label it is a prefix of");

    /* The directory to be covered, with one file the volume also has — the
     * composed volume's `file` — and one it does not. */
    VerifyPersistRequire(VfsCreateDirectory(VERIFY_PERSIST_POINT, 0755U) &&
                             VerifyPersistWrite(VERIFY_PERSIST_POINT "/file", "shipped file",
                                                VFS_OPEN_CREATE) &&
                             VerifyPersistWrite(VERIFY_PERSIST_POINT "/fresh", "shipped fresh",
                                                VFS_OPEN_CREATE),
                         "the directory to be covered could not be made");

    VerifyPersistRequire(PersistMountDevice(device, VERIFY_PERSIST_POINT, &report) &&
                             report.mounted && !report.read_only,
                         "the volume was not mounted for writing over the directory");

    /*
     * **Left open, it is marked clean and mounted writable** — and the mark
     * reached the medium. Mounted read-only instead, a person's configuration
     * could never again be edited after the first time the machine was
     * switched off by closing its window. **Observed** while this was written:
     * the first form wrote nothing and said nothing, PERSIST.md.
     */
    VerifyPersistRequire(report.marked_clean,
                         "a volume left open was not marked clean before it was mounted");

    /*
     * **What the volume lacks is seeded; what it has is kept.** Overwriting a
     * file the volume has would put back the shipped configuration over the
     * one a person edited, at every start, which is the one thing the volume
     * exists to prevent.
     */
    VerifyPersistRequire((report.seeded == 1U) && (report.kept == 1U) && (report.skipped == 0U),
                         "the seeding did not copy one file and keep one");
    VerifyPersistRequire(VerifyPersistBegins(VERIFY_PERSIST_POINT "/fresh", "shipped fresh"),
                         "a file the volume lacked was not seeded from the covered directory");
    VerifyPersistRequire(!VerifyPersistBegins(VERIFY_PERSIST_POINT "/file", "shipped file"),
                         "a file the volume had was overwritten by the covered directory's");

    /*
     * **A file written upon it is upon the medium once it is closed**, not in
     * the buffer cache: an emulator stopped by closing its window would lose
     * the edit otherwise. The store behind the device is searched, which the
     * cache does not reach.
     */
    VerifyPersistRequire(VerifyPersistWrite(VERIFY_PERSIST_POINT "/fresh", "edited upon it",
                                            VFS_OPEN_TRUNCATE) &&
                             VerifyPersistUponMedium(store, size, "edited upon it"),
                         "a file closed upon the volume was not written back to the medium");


    /*
     * **A name made or removed upon it is upon the medium when the call
     * returns**, since 2026-09-24, as a file closed upon it is: `micro` saves
     * by writing a file beside and moving the name across with `unlink` and
     * `link`, and a move left in the buffer cache would be lost with the
     * emulator's window, the edit with it. The cache is emptied first, so that
     * a dirty buffer afterwards can only be what the call left.
     */
    (void)BufferSync();
    VerifyPersistRequire(VfsCreateDirectory(VERIFY_PERSIST_POINT "/made", 0755U) &&
                             (BufferDirtyCount() == 0U) &&
                             VfsRemoveDirectory(VERIFY_PERSIST_POINT "/made") &&
                             (BufferDirtyCount() == 0U) &&
                             VfsUnlink(VERIFY_PERSIST_POINT "/fresh") &&
                             (BufferDirtyCount() == 0U),
                         "a name made or removed upon the volume was left in the buffer cache");

    /* **Released, it is clean**, and the directory beneath shows again. */
    VerifyPersistRequire(PersistRelease(VERIFY_PERSIST_POINT) &&
                             ((VerifyPersistState(store) & EXT2_VALID_FS) != 0U),
                         "the volume was not released clean");
    VerifyPersistRequire(VerifyPersistBegins(VERIFY_PERSIST_POINT "/file", "shipped file"),
                         "the covered directory did not show again once the volume was released");

    /* Put back as the virtual filesystem's test left it. */
    (void)VfsUnlink(VERIFY_PERSIST_POINT "/file");
    (void)VfsUnlink(VERIFY_PERSIST_POINT "/fresh");
    (void)VfsRemoveDirectory(VERIFY_PERSIST_POINT);

    for (size_t index = 0U; index < EXT2_VOLUME_NAME_LENGTH; ++index)
    {
        store[label_at + index] = saved_label[index];
    }

    store[state_at] = saved_state;
    (void)BufferInvalidateDevice(device);

    KernelWriteString(VerifyPersistSucceeded
                          ? "Persistent /etc self-test passed: found by its whole label, marked "
                            "clean when left open, seeded without overwriting, written back upon "
                            "a close and upon a name made or removed, and released clean.\n"
                          : "Persistent /etc self-test FAILED.\n");
}
