/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/test/storage/initrd.c
 * Purpose: Asserts the work of sub-task 7.7 — the initial ramdisk the boot
 *          loader supplies as a module, the block device it is presented as, and
 *          the root filesystem it is mounted as — against the root the machine
 *          actually booted with rather than against one this test composed.
 * Key functions: KernelVerifyInitrd.
 * References:
 *   - docs/storage/INITRD.md, Section 7: what this asserts, in the four groups
 *     below, and the two things nothing here can assert.
 *   - Multiboot2 Specification 2.0, Section 3.6.6: the module tag the ramdisk
 *     arrives in.
 *   - kernel/test/libc/utilities_image.asm: the copy of each utility that is
 *     embedded in this image, which is what the copy upon the ramdisk is
 *     compared against.
 *   - docs/storage/BLOCK.md and docs/storage/VFS.md: the two layers the bytes
 *     travel through between the module and this file.
 *
 * Why this test is unlike every other one in this directory.
 *
 *   Every other self-test here composes its subject. `kernel/test/volume.c`
 *   writes an EXT2 volume into an array, registers a device over it, and asserts
 *   against what it wrote — which is exactly right for asserting a *parser*, and
 *   is no use at all for asserting a *boot*. A composed volume proves that a
 *   ramdisk can be mounted. It cannot distinguish a kernel that mounted one from
 *   a kernel that did not.
 *
 *   So this test composes nothing and mounts nothing. It runs after
 *   KernelMountRootVolume and examines the root that function left behind. If
 *   the module was not found, or the device was not registered, or the mount was
 *   refused, the assertions below fail — which is the property being bought, and
 *   it is the reason this one file sits outside the sequence every other
 *   self-test is run in.
 *
 * The comparison against the embedded copy, and why it is the assertion that
 * matters.
 *
 *   The five utilities and the shell are upon the ramdisk *and* inside this
 *   image: the Makefile copies one file, `build/user/<name>.embed.elf`, into
 *   both. So the bytes are known to be the same at build time, and any
 *   difference observed here was introduced by the path between them — the
 *   module's extent, the direct map, the block device's arithmetic, the buffer
 *   cache, the inode's direct and indirect block pointers, the file's length.
 *
 *   Every one of those fails silently. A read that follows the wrong indirect
 *   block returns a block; a length rounded up to the block returns bytes. A
 *   test that opened each file and found it present would pass upon all of them,
 *   and so would a test that ran the program — a program whose last page is
 *   wrong still starts, because `_start` is at the beginning.
 */

#include <oxys/kernel.h>
#include <oxys/test/verify.h>

#include <oxys/block/block.h>
#include <oxys/boot/bootinfo.h>
#include <oxys/dev/storage/ramdisk.h>
#include <oxys/exec/elf.h>
#include <oxys/fs/vfs.h>
#include <oxys/mm/heap.h>
#include <oxys/proc/process.h>

/* The directory the utilities stand in upon the ramdisk. The Makefile's
 * INITRD_UTILITIES list and this one describe the same six programs, and the
 * count below is what says so out loud. */
#define KERNEL_INITRD_BIN "/bin"

/* Where the test writes, to establish that the root it was given can be written
 * to. It is created and removed within this function, so a ramdisk that survived
 * into a later boot — which none does — would not accumulate it. */
#define KERNEL_INITRD_SCRATCH "/verify-initrd"

/* What the scratch file is filled with. A pattern of more than one byte, because
 * a write that delivered the right count of the wrong byte is a real failure and
 * a single repeated character cannot detect it. */
#define KERNEL_INITRD_CONTENTS "sub-task 7.7: the root is writable.\n"

/*
 * The copies embedded in this image, placed here by
 * kernel/test/libc/utilities_image.asm — and the shell by
 * kernel/test/libc/line_image.asm. They are arrays of unknown size and not
 * pointers, for the reason kernel/test/libc/startup.c records: a linker symbol
 * has an address and no storage.
 */
extern const uint8_t KernelProgramEchoBegin[];
extern const uint8_t KernelProgramEchoEnd[];
extern const uint8_t KernelProgramCatBegin[];
extern const uint8_t KernelProgramCatEnd[];
extern const uint8_t KernelProgramListBegin[];
extern const uint8_t KernelProgramListEnd[];
extern const uint8_t KernelProgramMakeDirBegin[];
extern const uint8_t KernelProgramMakeDirEnd[];
extern const uint8_t KernelProgramRemoveBegin[];
extern const uint8_t KernelProgramRemoveEnd[];
extern const uint8_t KernelProgramTouchBegin[];
extern const uint8_t KernelProgramTouchEnd[];
extern const uint8_t KernelProgramCopyBegin[];
extern const uint8_t KernelProgramCopyEnd[];
extern const uint8_t KernelProgramRemoveDirBegin[];
extern const uint8_t KernelProgramRemoveDirEnd[];
extern const uint8_t KernelProgramWordCountBegin[];
extern const uint8_t KernelProgramWordCountEnd[];
extern const uint8_t KernelProgramMicroBegin[];
extern const uint8_t KernelProgramMicroEnd[];
extern const uint8_t KernelProgramShellBegin[];
extern const uint8_t KernelProgramShellEnd[];

/* One program: where it stands upon the ramdisk, and the extent of the copy
 * embedded here. The table is walked rather than the six being written out, so
 * that a seventh is one row and not six edits — as the shell of 8.1 was. */
typedef struct KernelInitrdUtility
{
    const char *path;
    const uint8_t *begin;
    const uint8_t *end;
} KernelInitrdUtility;

static const KernelInitrdUtility KernelInitrdUtilities[] = {
    { KERNEL_INITRD_BIN "/echo", KernelProgramEchoBegin, KernelProgramEchoEnd },
    { KERNEL_INITRD_BIN "/cat", KernelProgramCatBegin, KernelProgramCatEnd },
    { KERNEL_INITRD_BIN "/ls", KernelProgramListBegin, KernelProgramListEnd },
    { KERNEL_INITRD_BIN "/mkdir", KernelProgramMakeDirBegin, KernelProgramMakeDirEnd },
    { KERNEL_INITRD_BIN "/rm", KernelProgramRemoveBegin, KernelProgramRemoveEnd },
    { KERNEL_INITRD_BIN "/touch", KernelProgramTouchBegin, KernelProgramTouchEnd },
    { KERNEL_INITRD_BIN "/cp", KernelProgramCopyBegin, KernelProgramCopyEnd },
    { KERNEL_INITRD_BIN "/rmdir", KernelProgramRemoveDirBegin, KernelProgramRemoveDirEnd },
    { KERNEL_INITRD_BIN "/wc", KernelProgramWordCountBegin, KernelProgramWordCountEnd },
    { KERNEL_INITRD_BIN "/micro", KernelProgramMicroBegin, KernelProgramMicroEnd },
    { KERNEL_INITRD_BIN "/sh", KernelProgramShellBegin, KernelProgramShellEnd },
};

#define KERNEL_INITRD_UTILITY_COUNT \
    (sizeof(KernelInitrdUtilities) / sizeof(KernelInitrdUtilities[0]))

static bool KernelInitrdSucceeded;

static void KernelInitrdRequire(bool condition, const char *statement)
{
    if (!condition)
    {
        KernelWriteString("  ");
        KernelWriteString(statement);
        KernelWriteString(" FAILED.\n");
        KernelInitrdSucceeded = false;
    }
}

/* Says what went wrong as well as that something did, the filesystem layer
 * holding a reason that a boolean cannot carry. */
static void KernelInitrdRequireVfs(bool condition, const char *statement)
{
    if (!condition)
    {
        KernelWriteString("  ");
        KernelWriteString(statement);
        KernelWriteString(" FAILED: ");
        KernelWriteString(VfsLastError());
        KernelWriteString(".\n");
        KernelInitrdSucceeded = false;
    }
}

/* ---------------------------------------------------------------------------
 * Group 1: the module arrived, and became a device beneath a root.
 * ------------------------------------------------------------------------- */

static void KernelInitrdArrival(const BootInformation *information)
{
    const BootModule *const module =
        BootInformationFindModule(information, RAMDISK_MODULE_NAME);
    const BlockDevice *const device = RamdiskDevice();

    KernelInitrdRequire(module != NULL,
                        "the boot loader supplied no module named " RAMDISK_MODULE_NAME);
    KernelInitrdRequire(device != NULL,
                        "no block device was registered for the initial ramdisk");

    if ((module == NULL) || (device == NULL))
    {
        return;
    }

    /*
     * The device's geometry is the module's extent, and that is asserted rather
     * than assumed. A device registered with the wrong block count reads
     * correctly everywhere except at its end, and the end of a volume is where
     * the last inode's last block is.
     */
    KernelInitrdRequire(device->block_size == BLOCK_SIZE_DEFAULT,
                        "the ramdisk was registered with a block size the buffer cache "
                        "cannot hold");
    KernelInitrdRequire((device->block_count * device->block_size) ==
                            (module->end - module->start),
                        "the ramdisk's geometry does not describe the module's extent");
    KernelInitrdRequire(!device->read_only,
                        "the ramdisk was registered read-only, and nothing owns it but "
                        "this kernel");

    /*
     * And the root is mounted upon that device and not upon some other one. A
     * machine carrying a disk with a volume upon it would mount that volume at
     * the root were the ramdisk preferred by accident rather than by name, and
     * every assertion below would then be about the wrong filesystem.
     */
    KernelInitrdRequire(VfsRootIsMounted(), "no volume is mounted at the root");

    if (VfsRootIsMounted())
    {
        const VfsMount *const root = VfsMountAt(0U);

        KernelInitrdRequire((root != NULL) && (root->device == device),
                            "the root is mounted upon a device that is not the ramdisk");
    }
}

/* ---------------------------------------------------------------------------
 * Group 2: the bytes upon the ramdisk are the bytes that were put there.
 * ------------------------------------------------------------------------- */

/*
 * Reads a whole file from the root into memory obtained from the kernel heap.
 *
 * The length is taken from the file's own attributes and the read is then
 * required to deliver exactly that many bytes and to report an end immediately
 * afterwards. Both halves are necessary: a filesystem that reported a length
 * larger than the file would satisfy the first, and one that delivered the file
 * and then delivered it again would satisfy the second.
 *
 * Returns null having said what went wrong. The caller frees what it is given.
 */
static uint8_t *KernelInitrdReadFile(const char *path, uint64_t *length)
{
    VfsAttributes attributes;
    uint8_t *buffer;
    uint64_t read = 0U;
    uint64_t trailing = 0U;
    uint8_t beyond = 0U;
    int descriptor;

    *length = 0U;

    if (!VfsStat(path, &attributes))
    {
        KernelInitrdRequireVfs(false, "a utility is absent from the ramdisk");

        return NULL;
    }

    if (attributes.type != VFS_NODE_REGULAR)
    {
        KernelInitrdRequire(false, "a utility upon the ramdisk is not a regular file");

        return NULL;
    }

    buffer = (uint8_t *)KernelAllocate((size_t)attributes.size);

    if (buffer == NULL)
    {
        KernelInitrdRequire(false, "the kernel heap could not hold a utility read from "
                                   "the ramdisk");

        return NULL;
    }

    descriptor = VfsOpen(path, VFS_OPEN_READ, 0U);

    if (descriptor < 0)
    {
        KernelFree(buffer);
        KernelInitrdRequireVfs(false, "a utility upon the ramdisk could not be opened");

        return NULL;
    }

    if (!VfsRead(descriptor, buffer, attributes.size, &read))
    {
        (void)VfsClose(descriptor);
        KernelFree(buffer);
        KernelInitrdRequireVfs(false, "a utility upon the ramdisk could not be read");

        return NULL;
    }

    /* One byte beyond the length the file declares. A file that delivers it is
     * a file whose size and whose contents disagree, and the disagreement is
     * invisible to a reader that asks only for the size. */
    (void)VfsRead(descriptor, &beyond, 1U, &trailing);
    (void)VfsClose(descriptor);

    if ((read != attributes.size) || (trailing != 0U))
    {
        KernelFree(buffer);
        KernelInitrdRequire(false, "a utility upon the ramdisk is not the length it "
                                   "declares");

        return NULL;
    }

    *length = read;

    return buffer;
}

static void KernelInitrdContents(void)
{
    for (size_t index = 0U; index < KERNEL_INITRD_UTILITY_COUNT; ++index)
    {
        const KernelInitrdUtility *const utility = &KernelInitrdUtilities[index];
        const uint64_t embedded = (uint64_t)(utility->end - utility->begin);
        uint64_t length = 0U;
        uint8_t *contents;
        uint64_t differing = 0U;

        contents = KernelInitrdReadFile(utility->path, &length);

        if (contents == NULL)
        {
            continue;
        }

        if (length != embedded)
        {
            KernelWriteString("  ");
            KernelWriteString(utility->path);
            KernelWriteString(" FAILED: it is ");
            KernelWriteDecimal(length);
            KernelWriteString(" bytes upon the ramdisk and ");
            KernelWriteDecimal(embedded);
            KernelWriteString(" bytes in this image.\n");
            KernelInitrdSucceeded = false;
            KernelFree(contents);

            continue;
        }

        for (uint64_t offset = 0U; offset < length; ++offset)
        {
            if (contents[offset] != utility->begin[offset])
            {
                ++differing;
            }
        }

        if (differing != 0U)
        {
            KernelWriteString("  ");
            KernelWriteString(utility->path);
            KernelWriteString(" FAILED: ");
            KernelWriteDecimal(differing);
            KernelWriteString(" of its ");
            KernelWriteDecimal(length);
            KernelWriteString(" bytes differ from the copy in this image.\n");
            KernelInitrdSucceeded = false;
        }

        KernelFree(contents);
    }
}

/* ---------------------------------------------------------------------------
 * Group 3: a program read from the root runs.
 * ------------------------------------------------------------------------- */

/*
 * Composes the argument block, out of a null-terminated list of strings standing
 * in this kernel's read-only data.
 *
 * It is the same composition kernel/test/libc/utilities.c performs and it is not
 * shared with it, deliberately: that file's copy is part of what sub-task 7.6 is
 * asserted by, and a helper shared between a test and the thing it asserts is a
 * helper that can be wrong in both places at once.
 */
static bool KernelInitrdArguments(ProcessArguments *arguments,
                                  const char *const *strings)
{
    arguments->argument_count = 0U;
    arguments->environment_count = 0U;
    arguments->storage_used = 0U;

    for (size_t index = 0U; strings[index] != NULL; ++index)
    {
        const char *const string = strings[index];
        size_t length = 0U;

        while (string[length] != '\0')
        {
            ++length;
        }

        if ((arguments->argument_count >= PROCESS_ARGUMENT_COUNT_MAXIMUM) ||
            ((arguments->storage_used + length + 1U) > PROCESS_ARGUMENT_BYTES_MAXIMUM))
        {
            return false;
        }

        arguments->argument[arguments->argument_count] = arguments->storage_used;
        ++arguments->argument_count;

        for (size_t offset = 0U; offset <= length; ++offset)
        {
            arguments->storage[arguments->storage_used + offset] = string[offset];
        }

        arguments->storage_used += (uint32_t)(length + 1U);
    }

    return true;
}

/*
 * Loads a program out of the root filesystem and runs it at privilege level 3,
 * asserting the status it ended with.
 *
 * The image is read through the filesystem rather than taken from this kernel's
 * own copy, which is the whole point of the exercise: what is being asserted is
 * that a program upon the ramdisk can be executed, and a run of the embedded
 * copy would assert that a program can be executed.
 */
static void KernelInitrdExecute(const char *path, const char *const *strings,
                                int64_t expected)
{
    ProcessArguments arguments;
    Process *process;
    Thread *thread;
    Thread *boot;
    ElfImage loaded;
    uint64_t stack;
    uint64_t length = 0U;
    uint8_t *image;
    const size_t open_before = VfsOpenFileCount();

    image = KernelInitrdReadFile(path, &length);

    if (image == NULL)
    {
        return;
    }

    if (!KernelInitrdArguments(&arguments, strings))
    {
        KernelFree(image);
        KernelInitrdRequire(false, "the argument block could not be composed");

        return;
    }

    boot = ThreadAdoptCurrent("boot");

    if (boot == NULL)
    {
        KernelFree(image);
        KernelInitrdRequire(false, "the kernel's own flow of control could not be "
                                   "adopted as a thread");

        return;
    }

    process = ProcessCreate("initrd", NULL);

    if (process == NULL)
    {
        ThreadDestroy(boot);
        KernelFree(image);
        KernelInitrdRequire(false, "a process could not be created for the program");

        return;
    }

    if (ElfLoad(&process->space, image, length, &loaded) != ELF_OK)
    {
        ProcessDestroy(process);
        ThreadDestroy(boot);
        KernelFree(image);
        KernelInitrdRequire(false, "a program read from the ramdisk did not load");

        return;
    }

    ProcessRecordImage(process, &loaded);
    stack = ProcessCreateUserStack(process, &arguments);

    if (stack == 0U)
    {
        ProcessDestroy(process);
        ThreadDestroy(boot);
        KernelFree(image);
        KernelInitrdRequire(false, "a program read from the ramdisk was given no stack");

        return;
    }

    thread = ThreadCreate(process, loaded.entry, stack);

    if ((thread == NULL) || !ThreadStart(thread))
    {
        ProcessDestroy(process);
        ThreadDestroy(boot);
        KernelFree(image);
        KernelInitrdRequire(false, "a program read from the ramdisk could not be "
                                   "started");

        return;
    }

    KernelInitrdRequire(ThreadCurrent() == boot,
                        "the kernel did not resume the thread that started the program");
    KernelInitrdRequire(process->state == PROCESS_EXITED,
                        "the program's process was not marked as ended");

    if (process->exit_status != expected)
    {
        KernelWriteString("  the program ");
        KernelWriteString(path);
        KernelWriteString(" FAILED: the status was ");
        KernelWriteHexadecimal((uint64_t)process->exit_status);
        KernelWriteString(" and not ");
        KernelWriteHexadecimal((uint64_t)expected);
        KernelWriteString(".\n");
        KernelInitrdSucceeded = false;
    }

    ProcessDestroy(process);
    ThreadDestroy(boot);
    KernelFree(image);

    KernelInitrdRequire(VfsOpenFileCount() == open_before,
                        "a program run from the ramdisk left an open file behind it");
}

/* ---------------------------------------------------------------------------
 * Group 4: the root can be written.
 * ------------------------------------------------------------------------- */

static void KernelInitrdWritable(void)
{
    static const char contents[] = KERNEL_INITRD_CONTENTS;
    const uint64_t length = (uint64_t)(sizeof(contents) - 1U);
    uint8_t readback[sizeof(contents)];
    VfsAttributes attributes;
    uint64_t transferred = 0U;
    uint64_t differing = 0U;
    int descriptor;

    descriptor = VfsOpen(KERNEL_INITRD_SCRATCH,
                         VFS_OPEN_WRITE | VFS_OPEN_CREATE | VFS_OPEN_TRUNCATE, 0644U);

    if (descriptor < 0)
    {
        KernelInitrdRequireVfs(false, "a file could not be created upon the root");

        return;
    }

    KernelInitrdRequireVfs(VfsWrite(descriptor, contents, length, &transferred),
                           "a file upon the root could not be written");
    (void)VfsClose(descriptor);
    KernelInitrdRequire(transferred == length,
                        "a write to the root did not deliver every byte");

    /*
     * The file is read back through a second open rather than by seeking upon
     * the first. A position rewound upon an open file may be served from
     * whatever that file already holds; a second open must reach the volume.
     */
    descriptor = VfsOpen(KERNEL_INITRD_SCRATCH, VFS_OPEN_READ, 0U);

    if (descriptor < 0)
    {
        KernelInitrdRequireVfs(false, "a file written upon the root could not be "
                                      "reopened");
        (void)VfsUnlink(KERNEL_INITRD_SCRATCH);

        return;
    }

    transferred = 0U;
    KernelInitrdRequireVfs(VfsRead(descriptor, readback, sizeof(readback), &transferred),
                           "a file written upon the root could not be read back");
    (void)VfsClose(descriptor);

    KernelInitrdRequire(transferred == length,
                        "a file read back from the root is not the length it was "
                        "written with");

    if (transferred == length)
    {
        for (uint64_t offset = 0U; offset < length; ++offset)
        {
            if (readback[offset] != (uint8_t)contents[offset])
            {
                ++differing;
            }
        }
    }

    KernelInitrdRequire(differing == 0U,
                        "a file read back from the root is not what was written to it");

    KernelInitrdRequireVfs(VfsUnlink(KERNEL_INITRD_SCRATCH),
                           "a file created upon the root could not be removed");
    KernelInitrdRequire(!VfsStat(KERNEL_INITRD_SCRATCH, &attributes),
                        "a file removed from the root is still there");
}

/* ---------------------------------------------------------------------------
 * The self-test.
 * ------------------------------------------------------------------------- */

void KernelVerifyInitrd(void)
{
    static const char *const echo_vector[] = { "echo", "the root filesystem is the",
                                               "initial ramdisk.", NULL };

    KernelInitrdSucceeded = true;

    KernelWriteString("Initial ramdisk: the module of sub-task 7.7, the device beneath "
                      "it, and the root it is mounted as.\n");

    KernelInitrdArrival(&KernelBootInformation);

    /*
     * The three groups below all resolve paths, so they are attempted only where
     * something is mounted at the root. Without the guard each of them would
     * report a separate failure describing the same single cause, and a reader
     * of the log would have to work out that they were one fault.
     */
    if (!VfsRootIsMounted())
    {
        KernelWriteString("Initial ramdisk self-test FAILED: nothing is mounted at the "
                          "root, so nothing further could be asserted.\n");

        return;
    }

    KernelInitrdContents();
    KernelInitrdExecute(KERNEL_INITRD_BIN "/echo", echo_vector, 0);
    KernelInitrdWritable();

    if (KernelInitrdSucceeded)
    {
        KernelWriteString("Initial ramdisk self-test passed: ");
        KernelWriteDecimal((uint64_t)KERNEL_INITRD_UTILITY_COUNT);
        KernelWriteString(" programs stand in " KERNEL_INITRD_BIN
                          " byte for byte as they were built, one of them ran from "
                          "there at privilege level 3, and the root took a write.\n");
    }
    else
    {
        KernelWriteString("Initial ramdisk self-test FAILED.\n");
    }
}
