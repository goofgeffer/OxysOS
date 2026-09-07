/*
 * File: kernel/test/ext2/probe.c
 * Purpose: Examines whatever EXT2 volume the machine actually carries, and
 *          reports upon it. Nothing here asserts anything.
 * Key functions: KernelReportVolumes.
 * References:
 *   - kernel/test/README.md, "The distinction between a test and a probe": the
 *     reason this file exists and the reason it is named as it is.
 *   - docs/storage/EXT2.md: the format the report decodes.
 *
 * This is a probe and not a self-test, and the division is the point of the
 * file rather than an accident of where it was cut.
 *
 * There is nothing to assert about a disk this kernel did not write. What a
 * probe supplies is the one thing a composed fixture cannot: the fixture shares
 * this kernel's understanding of the format, so a misreading of the
 * specification would be composed into it and then asserted against itself. A
 * probe produces a volume a tool outside this kernel — `e2fsck`, `debugfs`,
 * `dumpe2fs` — can judge, and that is how the defect in the recorded deletion
 * time described in docs/storage/VFS.md, Section 11.1, was found: every
 * assertion in this directory passed, and the volume was nevertheless wrong.
 *
 * The probe that writes is selected by the boot loader's command line and never
 * runs by default. A kernel that wrote to a stranger's disk merely by having
 * been booted would impose a real cost for nothing.
 */

#include "internal.h"

#include <oxys/kernel.h>
#include <oxys/verify.h>
#include <oxys/testvolume.h>
#include <oxys/ext2.h>
#include <oxys/block.h>
#include <oxys/buffer.h>


#define KERNEL_REPORTED_BLOCKS 13U

/* The path resolved upon every volume the machine carries, as a demonstration
 * that resolution works upon a volume this kernel did not compose. */
#define KERNEL_PROBE_PATH "/lost+found"

/* How many bytes of a regular file the probe reports. */
#define KERNEL_REPORTED_BYTES 16U

static void KernelReportBlockAt(BlockDevice *device, const Ext2Superblock *superblock,
                                const Ext2Inode *inode, uint64_t index)
{
    uint32_t block;

    KernelWriteString(" [");
    KernelWriteDecimal(index);
    KernelWriteString("]=");

    if (!Ext2InodeBlock(device, superblock, inode, index, &block))
    {
        KernelWriteString("refused");
        return;
    }

    KernelWriteDecimal((uint64_t)block);
}

static void KernelReportRootInode(BlockDevice *device, const Ext2Superblock *superblock)
{
    const uint64_t pointers = superblock->block_size / EXT2_BLOCK_POINTER_SIZE;
    const uint64_t indirect = EXT2_DIRECT_BLOCK_COUNT;
    const uint64_t doubly = indirect + pointers;
    const uint64_t triply = doubly + (pointers * pointers);
    Ext2Inode root;
    Ext2Inode probe;
    uint64_t count;

    if (!Ext2ReadInode(device, superblock, EXT2_ROOT_INODE, &root))
    {
        KernelWriteString("EXT2: the root inode could not be read: ");
        KernelWriteString(Ext2LastError());
        KernelWriteString("\n");
        return;
    }

    Ext2ReportInode(&root);

    count = Ext2InodeBlockCount(superblock, &root);
    KernelWriteString("EXT2 root blocks:");

    for (uint64_t index = 0U; (index < count) && (index < KERNEL_REPORTED_BLOCKS); ++index)
    {
        uint32_t block;

        if (!Ext2InodeBlock(device, superblock, &root, index, &block))
        {
            KernelWriteString(" (refused: ");
            KernelWriteString(Ext2LastError());
            KernelWriteString(")");
            break;
        }

        KernelWriteString(" ");
        KernelWriteDecimal((uint64_t)block);
    }

    if (count > KERNEL_REPORTED_BLOCKS)
    {
        KernelWriteString(" ...");
    }

    /* The first block of each indirect range the directory reaches. */
    if (count > doubly)
    {
        KernelReportBlockAt(device, superblock, &root, doubly);
    }

    if (count > triply)
    {
        KernelReportBlockAt(device, superblock, &root, triply);
    }

    KernelWriteString("\n");

    /*
     * The root directory of the volume, listed. This is the first report the
     * kernel makes that names anything a person would recognise, and it is the
     * only assertion available upon a real volume: the self-test's composed
     * directory is by construction the directory the traversal expects, whereas
     * the names below were written by mke2fs and by whoever used the disk.
     */
    Ext2ReportDirectory(device, superblock, &root);

    /*
     * One path of the volume, resolved. Every volume `mke2fs` creates holds a
     * lost+found directory in its root, so the probe is a name this kernel may
     * look for upon a volume it knows nothing else about; a volume that does not
     * hold it reports the refusal, which is itself the correct answer.
     */
    if (Ext2ResolvePathNoFollow(device, superblock, KERNEL_PROBE_PATH, &probe))
    {
        KernelWriteString("EXT2 path " KERNEL_PROBE_PATH " resolves to inode ");
        KernelWriteDecimal((uint64_t)probe.number);
        KernelWriteString(", ");
        KernelWriteString(Ext2FileTypeName(Ext2FileTypeOfMode(probe.mode)));
        KernelWriteString(" of ");
        KernelWriteDecimal(probe.size);
        KernelWriteString(" bytes.\n");

        /*
         * What the probe holds, which is the one exercise of the reading of
         * Section 5.5 upon a volume this kernel did not compose. The path is
         * resolved without following a last link, so that a link reports itself
         * and its target rather than silently reporting what it names.
         */
        if (Ext2InodeIsSymbolicLink(&probe))
        {
            char target[EXT2_SYMLINK_MAXIMUM + 1U];

            if (Ext2ReadSymbolicLink(device, superblock, &probe, target, sizeof target))
            {
                KernelWriteString("EXT2 path " KERNEL_PROBE_PATH " is a ");
                KernelWriteString(Ext2InodeIsFastSymbolicLink(superblock, &probe)
                                      ? "target held within its inode: "
                                      : "target held in a block: ");
                KernelWriteString(target);
                KernelWriteString("\n");
            }
            else
            {
                KernelWriteString("EXT2 path " KERNEL_PROBE_PATH " has no readable target: ");
                KernelWriteString(Ext2LastError());
                KernelWriteString("\n");
            }
        }
        else if (Ext2InodeIsRegular(&probe))
        {
            uint8_t head[KERNEL_REPORTED_BYTES];
            uint64_t read = 0U;

            if (Ext2ReadFile(device, superblock, &probe, 0U, head, sizeof head, &read))
            {
                KernelWriteString("EXT2 path " KERNEL_PROBE_PATH " begins:");

                for (uint64_t index = 0U; index < read; ++index)
                {
                    KernelWriteString(" ");
                    KernelWriteHexadecimal((uint64_t)head[index]);
                }

                KernelWriteString(" (");
                KernelWriteDecimal(read);
                KernelWriteString(" bytes read)\n");
            }
            else
            {
                KernelWriteString("EXT2 path " KERNEL_PROBE_PATH " could not be read: ");
                KernelWriteString(Ext2LastError());
                KernelWriteString("\n");
            }
        }
    }
    else
    {
        KernelWriteString("EXT2 path " KERNEL_PROBE_PATH " does not resolve: ");
        KernelWriteString(Ext2LastError());
        KernelWriteString("\n");
    }
}

/*
 * Writes to a volume the machine actually carries, and only when the operator
 * has asked for it at the boot menu.
 *
 * Every other exercise of the writing of sub-task 5.6 is performed upon the
 * device of memory, whose contents this kernel composed and owns. A volume upon
 * a disk belongs to whoever booted this kernel, and a self-test that altered one
 * unbidden would destroy their data to prove a point about its own correctness.
 *
 * Two further precautions bound what this can damage even when it is asked for.
 * It writes only to a file named KERNEL_WRITE_PROBE_PATH, which nothing but a
 * deliberate preparation for this test would have created, and it refuses to
 * proceed unless that file is a regular file the volume already holds — it
 * creates nothing, and it touches nothing it was not pointed at. The file is
 * left holding what this writes, so the operator may compare it from outside.
 */
#define KERNEL_WRITE_PROBE_PATH "/oxys-write-test"
#define KERNEL_WRITE_PROBE_SIZE 8192U

static void KernelWriteProbeVolume(BlockDevice *device, Ext2Superblock *superblock)
{
    Ext2Inode probe;
    Ext2Inode parent;
    Ext2Inode made;
    Ext2Inode within;
    uint64_t moved = 0U;
    uint64_t index;

    if (!KernelCommandLineHasOption("ext2-write-test"))
    {
        return;
    }

    KernelWriteString("EXT2 write test: the command line permits writing to ");
    KernelWriteString(device->name);
    KernelWriteString(".\n");

    if (superblock->read_only)
    {
        KernelWriteString("EXT2 write test: the volume is read-only; nothing written.\n");
        return;
    }

    if (!Ext2ResolvePath(device, superblock, KERNEL_WRITE_PROBE_PATH, &probe))
    {
        KernelWriteString("EXT2 write test: " KERNEL_WRITE_PROBE_PATH " is not present; "
                          "nothing written.\n");
        return;
    }

    if (!Ext2InodeIsRegular(&probe))
    {
        KernelWriteString("EXT2 write test: " KERNEL_WRITE_PROBE_PATH " is not a regular "
                          "file; nothing written.\n");
        return;
    }

    /*
     * The file is emptied and then written afresh, so that the allocation, the
     * freeing and the extension are all exercised upon a real volume. The
     * contents are derived from the offset, so that a file written from the
     * wrong place is distinguishable from one written correctly when it is
     * examined from outside.
     */
    if (!Ext2TruncateFile(device, superblock, &probe, 0U))
    {
        KernelWriteString("EXT2 write test: the file could not be emptied: ");
        KernelWriteString(Ext2LastError());
        KernelWriteString("\n");
        return;
    }

    for (index = 0U; index < KERNEL_WRITE_PROBE_SIZE; index += sizeof KernelFileBuffer)
    {
        uint64_t run = KERNEL_WRITE_PROBE_SIZE - index;

        if (run > sizeof KernelFileBuffer)
        {
            run = sizeof KernelFileBuffer;
        }

        for (uint64_t offset = 0U; offset < run; ++offset)
        {
            KernelFileBuffer[offset] = KernelFileByteAt(index + offset);
        }

        if (!Ext2WriteFile(device, superblock, &probe, index, KernelFileBuffer, run, &moved) ||
            (moved != run))
        {
            KernelWriteString("EXT2 write test: the file could not be written: ");
            KernelWriteString(Ext2LastError());
            KernelWriteString("\n");
            return;
        }
    }

    /*
     * The cache is written back before anything is reported. Until it is, the
     * volume upon the disk holds none of this, and a report of success would
     * describe memory rather than the medium.
     */
    if (!BufferSync())
    {
        KernelWriteString("EXT2 write test: the cache could not be written back.\n");
        return;
    }

    /*
     * The names of sub-task 5.7, made and unmade within a directory of this
     * kernel's own creation. Everything here is removed again before the report,
     * so a volume that held /oxys-write-test before this ran holds exactly the
     * same set of names afterwards, with that one file rewritten. What is left
     * for e2fsck to judge is therefore the accounting rather than the tree.
     */
    if (!Ext2ResolvePath(device, superblock, "/", &parent))
    {
        KernelWriteString("EXT2 write test: the root could not be read.\n");
        return;
    }

    if (!Ext2CreateDirectory(device, superblock, &parent, "oxys-made", 9U, 0x01EDU, &made))
    {
        KernelWriteString("EXT2 write test: a directory could not be created: ");
        KernelWriteString(Ext2LastError());
        KernelWriteString("\n");
        return;
    }

    if (!Ext2CreateFile(device, superblock, &made, "within", 6U,
                        (uint16_t)(EXT2_S_IFREG | 0x01A4U), &within) ||
        !Ext2WriteFile(device, superblock, &within, 0U, KernelFileBuffer, 64U, &moved) ||
        (moved != 64U))
    {
        KernelWriteString("EXT2 write test: a file could not be created within it: ");
        KernelWriteString(Ext2LastError());
        KernelWriteString("\n");
        return;
    }

    KernelWriteString("EXT2 write test: created /oxys-made (inode ");
    KernelWriteDecimal((uint64_t)made.number);
    KernelWriteString(") holding within (inode ");
    KernelWriteDecimal((uint64_t)within.number);
    KernelWriteString(").\n");

    if (!Ext2Unlink(device, superblock, &made, "within", 6U) ||
        !Ext2RemoveDirectory(device, superblock, &parent, "oxys-made", 9U))
    {
        KernelWriteString("EXT2 write test: what was created could not be removed: ");
        KernelWriteString(Ext2LastError());
        KernelWriteString("\n");
        return;
    }

    KernelWriteString("EXT2 write test: removed both again.\n");

    KernelWriteString("EXT2 write test: wrote ");
    KernelWriteDecimal(probe.size);
    KernelWriteString(" bytes to " KERNEL_WRITE_PROBE_PATH " (inode ");
    KernelWriteDecimal((uint64_t)probe.number);
    KernelWriteString(", ");
    KernelWriteDecimal((uint64_t)probe.sector_count);
    KernelWriteString(" sectors); volume now reports ");
    KernelWriteDecimal((uint64_t)superblock->free_block_count);
    KernelWriteString(" free blocks and ");
    KernelWriteDecimal((uint64_t)superblock->free_inode_count);
    KernelWriteString(" free inodes.\n");
}

/*
 * Reads and reports the superblock of every block device the machine carries.
 *
 * Nothing is mounted and nothing is retained. The purpose is that a volume the
 * machine actually holds is put through the parser at every boot, since the
 * self-test's composed volume is by construction the volume the parser expects.
 */
void KernelReportVolumes(void)
{
    const size_t count = BlockDeviceCount();

    if (count == 0U)
    {
        KernelWriteString("EXT2: no block device to examine.\n");
        return;
    }

    for (size_t index = 0U; index < count; ++index)
    {
        BlockDevice *const device = BlockDeviceAt(index);
        Ext2Superblock superblock;

        if (device == NULL)
        {
            break;
        }

        if (Ext2ReadSuperblock(device, &superblock))
        {
            Ext2GroupDescriptor descriptor;

            Ext2ReportVolume(&superblock, device->name);

            /*
             * The first group's descriptor, and the verification of the whole
             * table. Both are reported because the table is the structure every
             * later part of the filesystem is found through, and a volume whose
             * table this kernel refuses is one it could not mount.
             */
            if (Ext2ReadGroupDescriptor(device, &superblock, 0U, &descriptor))
            {
                Ext2ReportGroup(&descriptor);
            }

            if (!Ext2VerifyGroupDescriptors(device, &superblock))
            {
                KernelWriteString("EXT2: the descriptor table of ");
                KernelWriteString(device->name);
                KernelWriteString(" is not trustworthy: ");
                KernelWriteString(Ext2LastError());
                KernelWriteString("\n");
            }

            KernelReportRootInode(device, &superblock);
            KernelWriteProbeVolume(device, &superblock);
        }
        else
        {
            KernelWriteString("EXT2: ");
            KernelWriteString(device->name);
            KernelWriteString(" holds no volume this kernel can read: ");
            KernelWriteString(Ext2LastError());
            KernelWriteString("\n");
        }
    }
}

