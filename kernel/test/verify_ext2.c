/*
 * File: kernel/test/verify_ext2.c
 * Purpose: Asserts the EXT2 implementation of Phase 5 against the volume
 *          composed by kernel/test/volume.c: the superblock, the group
 *          descriptors, the inodes and their block pointers, directory
 *          traversal and path resolution, file reading, symbolic links,
 *          allocation, writing, truncation, and the creation and destruction of
 *          names. Reports upon whatever volume the machine actually carries.
 * Key functions: KernelVerifyExt2, KernelReportVolumes.
 * References:
   - docs/storage/EXT2.md, Section 14: every assertion below, paired with the
 *     silent failure it catches.
 *   - The Second Extended File System, Dave Poirier: the format itself. The
 *     field offsets are named in <oxys/ext2.h> and are not restated here.
 *
 * KernelReportVolumes is not a self-test. It examines a real volume and
 * asserts nothing, there being nothing to assert about a disk this kernel did
 * not write; its value is that a claim made about the composed volume may be
 * checked against a real one by a tool outside this kernel, which is how the
 * defect in the deletion time recorded in docs/storage/VFS.md was found.
 */

#include <oxys/kernel.h>
#include <oxys/verify.h>
#include <oxys/testvolume.h>
#include <oxys/ext2.h>
#include <oxys/block.h>
#include <oxys/buffer.h>
#include <oxys/ata.h>

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

/*
 * Alters one field of the composed volume and reports whether the parser refused
 * the result, restoring the volume afterwards.
 *
 * The cache is invalidated around the alteration. The superblock is written into
 * the device's storage directly, beneath both the block layer and the cache, so
 * a cache holding the previous contents would answer the next read with them and
 * the assertion would be made against the volume that no longer exists.
 */
static bool KernelVolumeRefusedWith(BlockDevice *device, size_t offset, uint32_t value,
                                    bool half)
{
    Ext2Superblock superblock;
    bool refused;

    if (half)
    {
        KernelSetVolumeHalf(offset, (uint16_t)value);
    }
    else
    {
        KernelSetVolumeWord(offset, value);
    }

    (void)BufferInvalidateDevice(device);
    refused = !Ext2ReadSuperblock(device, &superblock);

    KernelComposeVolume();
    (void)BufferInvalidateDevice(device);
    return refused;
}

/*
 * Alters one field of the composed group descriptor and reports whether the
 * given judgement refused the result, restoring the descriptor afterwards.
 *
 * The cache is invalidated on both sides of the alteration for the reason
 * KernelVolumeRefusedWith gives: the descriptor is written beneath the cache,
 * and a cache still holding the previous descriptor would answer with it.
 */
static bool KernelDescriptorRefusedWith(BlockDevice *device, const Ext2Superblock *superblock,
                                        size_t offset, uint32_t value, bool half,
                                        bool whole_table)
{
    Ext2GroupDescriptor descriptor;
    bool refused;

    if (half)
    {
        KernelStoreHalf(KernelDescriptorField(offset), (uint16_t)value);
    }
    else
    {
        KernelStoreWord(KernelDescriptorField(offset), value);
    }

    (void)BufferInvalidateDevice(device);

    refused = whole_table ? !Ext2VerifyGroupDescriptors(device, superblock)
                          : !Ext2ReadGroupDescriptor(device, superblock, 0U, &descriptor);

    KernelComposeGroupDescriptor();
    (void)BufferInvalidateDevice(device);
    return refused;
}

/*
 * Asserts that the block group descriptor table is read as it stands, and that a
 * table this kernel must not trust is refused.
 *
 * A descriptor is three block numbers and three counts, and every one of them is
 * a plausible number wherever it is read from. A table read one block early, or
 * a descriptor taken to be 24 or 40 bytes rather than 32, yields block numbers
 * that address real blocks of the volume — the wrong ones — and a kernel that
 * then wrote an inode would write it over a file. Naming the values, and
 * asserting the one statement the table makes as a whole, is what catches that.
 */
static bool KernelVerifyGroups(BlockDevice *device, const Ext2Superblock *superblock)
{
    Ext2GroupDescriptor descriptor;
    bool succeeded = true;

    /* The derived geometry of the table itself, before any of it is read. */
    if ((Ext2GroupDescriptorBlock(superblock) != KERNEL_VOLUME_DESCRIPTOR_BLOCK) ||
        (Ext2GroupDescriptorBlocks(superblock) != 1U) ||
        (Ext2InodeTableBlocks(superblock) != 4U) ||
        (Ext2GroupFirstBlock(superblock, 0U) != 1U) ||
        (Ext2GroupBlockCount(superblock, 0U) != 127U))
    {
        KernelWriteString("  The geometry of the descriptor table is wrong.\n");
        succeeded = false;
    }

    if (!Ext2ReadGroupDescriptor(device, superblock, 0U, &descriptor))
    {
        KernelWriteString("  A well-formed group descriptor was refused: ");
        KernelWriteString(Ext2LastError());
        KernelWriteString("\n");
        return false;
    }

    if ((descriptor.group != 0U) || (descriptor.block_bitmap != KERNEL_VOLUME_BLOCK_BITMAP) ||
        (descriptor.inode_bitmap != KERNEL_VOLUME_INODE_BITMAP) ||
        (descriptor.inode_table != KERNEL_VOLUME_INODE_TABLE) ||
        (descriptor.free_block_count != KERNEL_VOLUME_FREE_BLOCKS) ||
        (descriptor.free_inode_count != KERNEL_VOLUME_FREE_INODES) ||
        (descriptor.used_directory_count != KERNEL_VOLUME_DIRECTORIES))
    {
        KernelWriteString("  A field of the group descriptor was read from the wrong "
                          "place.\n");
        succeeded = false;
    }

    /* The whole table, and the one statement it makes about the volume. */
    if (!Ext2VerifyGroupDescriptors(device, superblock))
    {
        KernelWriteString("  A well-formed descriptor table was refused: ");
        KernelWriteString(Ext2LastError());
        KernelWriteString("\n");
        succeeded = false;
    }

    /* A group the volume does not hold. */
    if (Ext2ReadGroupDescriptor(device, superblock, superblock->group_count, &descriptor))
    {
        KernelWriteString("  A descriptor beyond the end of the table was read.\n");
        succeeded = false;
    }

    /* A structure of the group outside the volume, in both directions. */
    if (!KernelDescriptorRefusedWith(device, superblock, EXT2_OFFSET_BG_INODE_TABLE, 200U,
                                     false, false) ||
        !KernelDescriptorRefusedWith(device, superblock, EXT2_OFFSET_BG_BLOCK_BITMAP, 0U,
                                     false, false))
    {
        KernelWriteString("  A group whose structures lie outside the volume was "
                          "accepted.\n");
        succeeded = false;
    }

    /*
     * An inode table that begins within the volume and ends beyond it. The
     * length is not stored anywhere and follows from the inode size, so a kernel
     * that checked only the first block would read the last inodes of the group
     * from nowhere.
     */
    if (!KernelDescriptorRefusedWith(device, superblock, EXT2_OFFSET_BG_INODE_TABLE, 127U,
                                     false, false))
    {
        KernelWriteString("  An inode table running past the end of the volume was "
                          "accepted.\n");
        succeeded = false;
    }

    /* Two structures beginning upon the same block. */
    if (!KernelDescriptorRefusedWith(device, superblock, EXT2_OFFSET_BG_INODE_BITMAP,
                                     KERNEL_VOLUME_BLOCK_BITMAP, false, false))
    {
        KernelWriteString("  A group with two structures upon one block was accepted.\n");
        succeeded = false;
    }

    /* Counts beyond what the group holds. The count of directories is derived
     * from the volume rather than stated, the bound it must exceed being the
     * inodes in use, which changes whenever the composition does. */
    if (!KernelDescriptorRefusedWith(device, superblock, EXT2_OFFSET_BG_FREE_BLOCKS, 200U,
                                     true, false) ||
        !KernelDescriptorRefusedWith(device, superblock, EXT2_OFFSET_BG_FREE_INODES,
                                     superblock->inodes_per_group + 1U, true, false) ||
        !KernelDescriptorRefusedWith(
            device, superblock, EXT2_OFFSET_BG_USED_DIRECTORIES,
            (superblock->inodes_per_group - superblock->free_inode_count) + 1U, true, false))
    {
        KernelWriteString("  A group reporting more than it holds was accepted.\n");
        succeeded = false;
    }

    /*
     * A descriptor every rule above accepts, whose free count nevertheless
     * disagrees with the superblock's. This is the assertion the table makes as
     * a whole and it is the one a misread table fails.
     */
    if (!KernelDescriptorRefusedWith(device, superblock, EXT2_OFFSET_BG_FREE_BLOCKS, 50U, true,
                                     true))
    {
        KernelWriteString("  A table not accounting for the volume's free space was "
                          "accepted.\n");
        succeeded = false;
    }

    /* Requests with nothing to work upon. */
    if (Ext2ReadGroupDescriptor(NULL, superblock, 0U, &descriptor) ||
        Ext2ReadGroupDescriptor(device, superblock, 0U, NULL) ||
        Ext2VerifyGroupDescriptors(NULL, superblock))
    {
        KernelWriteString("  A degenerate descriptor request was accepted.\n");
        succeeded = false;
    }

    return succeeded;
}

/*
 * Alters one word of the composed filesystem, beneath the superblock, and
 * reports whether the inode reader refused the result. The volume is recomposed
 * and the cache invalidated afterwards, for the reason KernelVolumeRefusedWith
 * gives.
 */
static bool KernelInodeRefusedWith(BlockDevice *device, const Ext2Superblock *superblock,
                                   size_t offset, uint32_t value)
{
    Ext2Inode inode;
    bool refused;

    KernelStoreWord(offset, value);
    (void)BufferInvalidateDevice(device);

    refused = !Ext2ReadInode(device, superblock, KERNEL_VOLUME_FILE_INODE, &inode);

    KernelComposeVolume();
    (void)BufferInvalidateDevice(device);
    return refused;
}

/*
 * Asserts that an inode is found where the format says it is, that its fields
 * are read from the right offsets, and that a file block index is resolved
 * through however many levels of indirection it requires.
 *
 * Locating an inode is three pieces of arithmetic upon numbers that begin at one
 * and indices that begin at zero, and every plausible mistake in it — omitting
 * the subtraction, using the block size where the inode size belongs, taking the
 * group's first block for its inode table — yields an offset that lands upon
 * some other inode of the same volume. That inode is a valid inode. It simply
 * belongs to a different file, and nothing in the machine can tell.
 *
 * The resolution of the block pointers fails the same way. An index that lands
 * one entry adrift within an indirect block, or a level of the walk that divides
 * by the wrong span, produces a block number that is a real block of the volume
 * holding somebody else's data.
 */
static bool KernelVerifyInodes(BlockDevice *device, const Ext2Superblock *superblock)
{
    const uint64_t indirect_base = EXT2_DIRECT_BLOCK_COUNT;
    const uint64_t double_base = indirect_base + KERNEL_VOLUME_POINTERS;
    const uint64_t triple_base =
        double_base + ((uint64_t)KERNEL_VOLUME_POINTERS * KERNEL_VOLUME_POINTERS);
    const uint64_t beyond =
        triple_base + ((uint64_t)KERNEL_VOLUME_POINTERS * KERNEL_VOLUME_POINTERS *
                       KERNEL_VOLUME_POINTERS);
    Ext2Inode root;
    Ext2Inode file;
    uint32_t block;
    bool succeeded = true;

    /* The root directory, which the format reserves as inode 2. */
    if (!Ext2ReadInode(device, superblock, EXT2_ROOT_INODE, &root))
    {
        KernelWriteString("  The root inode was refused: ");
        KernelWriteString(Ext2LastError());
        KernelWriteString("\n");
        return false;
    }

    if ((root.number != EXT2_ROOT_INODE) || !Ext2InodeIsDirectory(&root) ||
        Ext2InodeIsRegular(&root) || (root.link_count != 3U) ||
        (root.size != KERNEL_VOLUME_BLOCK_SIZE) ||
        (root.block[0] != KERNEL_VOLUME_ROOT_DATA) ||
        ((root.mode & EXT2_PERMISSION_MASK) != 0x01EDU))
    {
        KernelWriteString("  The root inode was not read correctly.\n");
        succeeded = false;
    }

    /*
     * The file, which lies in the second block of the inode table: inode 11 is
     * index 10, and eight inodes of 128 bytes occupy a block of 1024. An inode
     * reader that never crossed out of the first block of the table would pass
     * every assertion above and fail here.
     */
    if (!Ext2ReadInode(device, superblock, KERNEL_VOLUME_FILE_INODE, &file))
    {
        KernelWriteString("  The composed file inode was refused: ");
        KernelWriteString(Ext2LastError());
        KernelWriteString("\n");
        return false;
    }

    if ((file.number != KERNEL_VOLUME_FILE_INODE) || !Ext2InodeIsRegular(&file) ||
        Ext2InodeIsDirectory(&file) || (file.size != KERNEL_VOLUME_FILE_SIZE) ||
        (file.link_count != 1U) || (file.sector_count != 32U) || (file.uid != 1000U) ||
        (file.gid != 1001U) || ((file.mode & EXT2_PERMISSION_MASK) != 0x01A4U))
    {
        KernelWriteString("  A field of the file inode was read from the wrong place.\n");
        succeeded = false;
    }

    if (Ext2InodeBlockCount(superblock, &file) != (KERNEL_VOLUME_FILE_SIZE / 1024U))
    {
        KernelWriteString("  The blocks the file's size spans were counted wrongly.\n");
        succeeded = false;
    }

    /* The twelve direct blocks, at both ends of the range. */
    if (!Ext2InodeBlock(device, superblock, &file, 0U, &block) ||
        (block != KERNEL_VOLUME_DIRECT_FIRST))
    {
        KernelWriteString("  The first direct block was resolved wrongly.\n");
        succeeded = false;
    }

    if (!Ext2InodeBlock(device, superblock, &file, EXT2_DIRECT_BLOCK_COUNT - 1U, &block) ||
        (block != (KERNEL_VOLUME_DIRECT_FIRST + EXT2_DIRECT_BLOCK_COUNT - 1U)))
    {
        KernelWriteString("  The last direct block was resolved wrongly.\n");
        succeeded = false;
    }

    /*
     * The indirect block, at its first and last entries. The last is the
     * boundary the whole decomposition turns upon: an index one beyond it must
     * enter the doubly indirect block instead.
     */
    if (!Ext2InodeBlock(device, superblock, &file, indirect_base, &block) ||
        (block != KERNEL_VOLUME_INDIRECT_DATA))
    {
        KernelWriteString("  The first indirect block was resolved wrongly.\n");
        succeeded = false;
    }

    if (!Ext2InodeBlock(device, superblock, &file, double_base - 1U, &block) ||
        (block != KERNEL_VOLUME_INDIRECT_LAST))
    {
        KernelWriteString("  The last indirect block was resolved wrongly.\n");
        succeeded = false;
    }

    /* A hole within an indirect block, which is a block of zeroes and not an
     * error and not the end of the file. */
    if (!Ext2InodeBlock(device, superblock, &file, indirect_base + 1U, &block) ||
        (block != 0U))
    {
        KernelWriteString("  A hole within an indirect block was not reported as one.\n");
        succeeded = false;
    }

    /* The doubly indirect block: a hole at its first entry, data at its sixth. */
    if (!Ext2InodeBlock(device, superblock, &file, double_base, &block) || (block != 0U))
    {
        KernelWriteString("  A hole beneath the doubly indirect block was not reported "
                          "as one.\n");
        succeeded = false;
    }

    if (!Ext2InodeBlock(device, superblock, &file, double_base + 5U, &block) ||
        (block != KERNEL_VOLUME_DOUBLE_DATA))
    {
        KernelWriteString("  A doubly indirect block was resolved wrongly.\n");
        succeeded = false;
    }

    /* The triply indirect block, three levels down. */
    if (!Ext2InodeBlock(device, superblock, &file, triple_base + 3U, &block) ||
        (block != KERNEL_VOLUME_TRIPLE_DATA))
    {
        KernelWriteString("  A triply indirect block was resolved wrongly.\n");
        succeeded = false;
    }

    /*
     * A hole at the top of a subtree. The triply indirect entry of this inode is
     * present, but the doubly indirect block beneath it holds one entry only, so
     * everything past that entry's range is a hole reached without any block
     * being read at all.
     */
    if (!Ext2InodeBlock(device, superblock, &file,
                        triple_base + ((uint64_t)KERNEL_VOLUME_POINTERS *
                                       KERNEL_VOLUME_POINTERS),
                        &block) ||
        (block != 0U))
    {
        KernelWriteString("  A hole occupying a whole subtree was not reported as one.\n");
        succeeded = false;
    }

    /* An index beyond what fifteen pointers can address is refused, not held. */
    if (Ext2InodeBlock(device, superblock, &file, beyond, &block))
    {
        KernelWriteString("  An index beyond the triply indirect range was resolved.\n");
        succeeded = false;
    }

    /* Inode numbers the volume does not hold, at both ends. */
    if (Ext2ReadInode(device, superblock, 0U, &file) ||
        Ext2ReadInode(device, superblock, superblock->inode_count + 1U, &file))
    {
        KernelWriteString("  An inode outside the volume was read.\n");
        succeeded = false;
    }

    /*
     * An inode of the table that was never filled. The bytes are zeroes, which
     * are a valid encoding of nothing, so accepting them would let arithmetic
     * that had strayed beyond the table report a file rather than a mistake.
     */
    if (Ext2ReadInode(device, superblock, KERNEL_VOLUME_UNUSED_INODE, &file))
    {
        KernelWriteString("  An inode not in use was read as a file.\n");
        succeeded = false;
    }

    /* A direct pointer outside the volume, refused when the inode is read. */
    if (!KernelInodeRefusedWith(device, superblock,
                                KernelInodeBlockField(KERNEL_VOLUME_FILE_INODE, 0U), 9999U))
    {
        KernelWriteString("  An inode naming a block outside the volume was accepted.\n");
        succeeded = false;
    }

    /*
     * A pointer within an indirect block that lies outside the volume. It cannot
     * be caught when the inode is read, the block holding it not having been
     * read then, so it is checked where it is fetched.
     */
    KernelStoreWord(KernelPointerField(KERNEL_VOLUME_INDIRECT, 0U), 9999U);
    (void)BufferInvalidateDevice(device);

    if (Ext2ReadInode(device, superblock, KERNEL_VOLUME_FILE_INODE, &file) &&
        Ext2InodeBlock(device, superblock, &file, indirect_base, &block))
    {
        KernelWriteString("  An indirect pointer outside the volume was resolved.\n");
        succeeded = false;
    }

    KernelComposeVolume();
    (void)BufferInvalidateDevice(device);

    /* Requests with nothing to work upon. */
    if (Ext2ReadInode(NULL, superblock, EXT2_ROOT_INODE, &file) ||
        Ext2ReadInode(device, superblock, EXT2_ROOT_INODE, NULL) ||
        Ext2InodeBlock(device, superblock, NULL, 0U, &block) ||
        Ext2InodeBlock(device, superblock, &root, 0U, NULL))
    {
        KernelWriteString("  A degenerate inode request was accepted.\n");
        succeeded = false;
    }

    return succeeded;
}

/*
 * Alters one field of the composed volume and reports whether a traversal of the
 * root directory refused the result, restoring the volume afterwards.
 *
 * The offset is a byte of the device's storage rather than of a block, because
 * the rules a directory is held to are stated partly by its entries and partly
 * by the inode that owns them, and both must be reachable from one helper.
 *
 * The cache is invalidated on both sides of the alteration for the reason
 * KernelVolumeRefusedWith gives.
 */
static bool KernelDirectoryRefusedWith(BlockDevice *device, const Ext2Superblock *superblock,
                                       size_t offset, uint32_t value, uint32_t width)
{
    Ext2DirectoryCursor cursor;
    Ext2DirectoryEntry entry;
    Ext2Inode root;
    bool refused = true;

    if (width == 1U)
    {
        KernelMemoryDeviceStore[offset] = (uint8_t)value;
    }
    else if (width == 2U)
    {
        KernelStoreHalf(offset, (uint16_t)value);
    }
    else
    {
        KernelStoreWord(offset, value);
    }

    (void)BufferInvalidateDevice(device);

    if (Ext2ReadInode(device, superblock, EXT2_ROOT_INODE, &root))
    {
        Ext2DirectoryOpen(&cursor, &root);
        refused = false;

        for (;;)
        {
            const Ext2DirectoryStep step =
                Ext2DirectoryNext(device, superblock, &cursor, &entry);

            if (step == EXT2_DIRECTORY_FAILED)
            {
                refused = true;
                break;
            }

            if (step == EXT2_DIRECTORY_END)
            {
                break;
            }
        }
    }

    KernelComposeVolume();
    (void)BufferInvalidateDevice(device);
    return refused;
}

/*
 * The same for one byte and a path, for the rules that are not visible until an
 * entry and the inode it names are compared with one another.
 */
static bool KernelPathRefusedWith(BlockDevice *device, const Ext2Superblock *superblock,
                                  size_t offset, uint8_t value, const char *path)
{
    Ext2Inode inode;
    bool refused;

    KernelMemoryDeviceStore[offset] = value;
    (void)BufferInvalidateDevice(device);

    refused = !Ext2ResolvePath(device, superblock, path, &inode);

    KernelComposeVolume();
    (void)BufferInvalidateDevice(device);
    return refused;
}

/* Whether a path resolves to the inode expected of it. */
static bool KernelPathIs(BlockDevice *device, const Ext2Superblock *superblock,
                         const char *path, uint32_t expected)
{
    Ext2Inode inode;

    return Ext2ResolvePath(device, superblock, path, &inode) && (inode.number == expected);
}

/* Whether a path is refused, which a path naming nothing must be. */
static bool KernelPathRefused(BlockDevice *device, const Ext2Superblock *superblock,
                              const char *path)
{
    Ext2Inode inode;

    return !Ext2ResolvePath(device, superblock, path, &inode);
}

/*
 * Asserts that the two readings of the two bytes at offset 6 of an entry are
 * distinguished by the incompatible feature flag and not by anything else.
 *
 * This is the one property of the format where the same bytes have two lawful
 * meanings, and where reading the wrong one produces no diagnostic of its own.
 * The entry "." bears a name length of 1 and a file type of EXT2_FT_DIR; read as
 * one sixteen-bit quantity those two bytes are 1 + 256 * 2 = 513, a name that
 * cannot fit within a record of twelve bytes. So the volume that states no file
 * type must refuse the entry the volume that states one accepts, and with the
 * file type byte cleared the same entry must read correctly with no type stated.
 */
static bool KernelVerifyEntryReadings(BlockDevice *device)
{
    Ext2DirectoryCursor cursor;
    Ext2DirectoryEntry entry;
    Ext2Superblock plain;
    Ext2Inode root;
    bool succeeded = true;

    KernelSetVolumeWord(EXT2_OFFSET_FEATURE_INCOMPAT, 0U);
    (void)BufferInvalidateDevice(device);

    if (Ext2ReadSuperblock(device, &plain) &&
        Ext2ReadInode(device, &plain, EXT2_ROOT_INODE, &root))
    {
        Ext2DirectoryOpen(&cursor, &root);

        if (Ext2DirectoryNext(device, &plain, &cursor, &entry) != EXT2_DIRECTORY_FAILED)
        {
            KernelWriteString("  A name length was read as eight bits upon a volume that "
                              "states no file type.\n");
            succeeded = false;
        }

        KernelMemoryDeviceStore[KernelVolumeBlock(KERNEL_VOLUME_ROOT_DATA) +
                                EXT2_OFFSET_DE_FILE_TYPE] = 0U;
        (void)BufferInvalidateDevice(device);

        if (!Ext2DirectoryFind(device, &plain, &root, ".", 1U, &entry) ||
            (entry.inode != EXT2_ROOT_INODE) ||
            (entry.file_type != (uint8_t)EXT2_FT_UNKNOWN))
        {
            KernelWriteString("  An entry of a volume that states no file type was not "
                              "read.\n");
            succeeded = false;
        }
    }
    else
    {
        KernelWriteString("  A volume stating no file type was refused: ");
        KernelWriteString(Ext2LastError());
        KernelWriteString("\n");
        succeeded = false;
    }

    KernelComposeVolume();
    (void)BufferInvalidateDevice(device);
    return succeeded;
}

/*
 * Asserts that the contents of a file are read, that a hole reads as zeroes,
 * that the end of the file is reported by the count rather than as a failure,
 * and that both forms of symbolic link are read and followed.
 *
 * The composed file holds a byte derived from its own offset rather than a
 * constant or a pattern repeating every block. That is deliberate: a reader that
 * returned the right number of bytes from the wrong block would be
 * indistinguishable from a correct one under either of those, and resolving the
 * wrong block is the failure this whole chapter is arranged to catch.
 */
static bool KernelVerifyFiles(BlockDevice *device, const Ext2Superblock *superblock)
{
    const uint64_t data_offset = (uint64_t)EXT2_DIRECT_BLOCK_COUNT * KERNEL_VOLUME_BLOCK_SIZE;
    const uint64_t hole_offset = data_offset + KERNEL_VOLUME_BLOCK_SIZE;
    char target[EXT2_SYMLINK_MAXIMUM + 1U];
    Ext2Inode inode;
    uint64_t read = 0U;
    bool succeeded = true;

    KernelWriteString("EXT2 files: asserting reading, holes and symbolic links.\n");

    if (!Ext2ResolvePath(device, superblock, "/sub/inner", &inode))
    {
        KernelWriteString("  The composed file was not found: ");
        KernelWriteString(Ext2LastError());
        KernelWriteString("\n");
        return false;
    }

    /* The whole file, every byte of it, across the boundary between its two
     * blocks and ending part-way through the second. */
    if (!Ext2ReadFile(device, superblock, &inode, 0U, KernelFileBuffer,
                      sizeof KernelFileBuffer, &read) ||
        (read != KERNEL_VOLUME_INNER_SIZE) || !KernelFileBufferMatches(0U, read))
    {
        KernelWriteString("  A file was not read as it was composed.\n");
        succeeded = false;
    }

    /*
     * A run crossing the boundary between the two blocks. The first block ends
     * at 1024 and this run begins at 1000, so a reader that took the whole run
     * from one block would return 24 correct bytes and 76 wrong ones.
     */
    if (!Ext2ReadFile(device, superblock, &inode, 1000U, KernelFileBuffer, 100U, &read) ||
        (read != 100U) || !KernelFileBufferMatches(1000U, read))
    {
        KernelWriteString("  A read across a block boundary returned the wrong bytes.\n");
        succeeded = false;
    }

    /* A run wholly within the second block, which begins at an offset the first
     * block does not contain. */
    if (!Ext2ReadFile(device, superblock, &inode, 1100U, KernelFileBuffer, 64U, &read) ||
        (read != 64U) || !KernelFileBufferMatches(1100U, read))
    {
        KernelWriteString("  A read from the second block returned the wrong bytes.\n");
        succeeded = false;
    }

    /*
     * The end of the file. A read that would cross it is shortened to it, and a
     * read beginning at or beyond it yields nothing and succeeds — the end of a
     * file is where every reader arrives, and reporting it as a failure would
     * oblige each of them to treat the conclusion of its work as a fault.
     */
    if (!Ext2ReadFile(device, superblock, &inode, KERNEL_VOLUME_INNER_SIZE - 100U,
                      KernelFileBuffer, 1000U, &read) ||
        (read != 100U) || !KernelFileBufferMatches(KERNEL_VOLUME_INNER_SIZE - 100U, read))
    {
        KernelWriteString("  A read crossing the end of the file was not shortened to it.\n");
        succeeded = false;
    }

    if (!Ext2ReadFile(device, superblock, &inode, KERNEL_VOLUME_INNER_SIZE, KernelFileBuffer,
                      64U, &read) ||
        (read != 0U))
    {
        KernelWriteString("  A read at the end of the file did not report the end.\n");
        succeeded = false;
    }

    if (!Ext2ReadFile(device, superblock, &inode, KERNEL_VOLUME_INNER_SIZE + 4096U,
                      KernelFileBuffer, 64U, &read) ||
        (read != 0U))
    {
        KernelWriteString("  A read beyond the end of the file did not report the end.\n");
        succeeded = false;
    }

    if (!Ext2ReadFile(device, superblock, &inode, 0U, KernelFileBuffer, 0U, &read) ||
        (read != 0U))
    {
        KernelWriteString("  A read of no length was not answered with no bytes.\n");
        succeeded = false;
    }

    /*
     * A hole reads as zeroes, and the block beside it reads as data. The sparse
     * file holds block 12 and not block 13, and the two are asserted together
     * because a reader that returned zeroes for both, or data for both, would
     * pass either assertion alone.
     */
    if (!Ext2ResolvePath(device, superblock, "/file", &inode))
    {
        KernelWriteString("  The composed sparse file was not found.\n");
        succeeded = false;
    }
    else
    {
        if (!Ext2ReadFile(device, superblock, &inode, data_offset, KernelFileBuffer, 64U,
                          &read) ||
            (read != 64U) || !KernelFileBufferMatches(data_offset, read))
        {
            KernelWriteString("  A block reached through the indirect block read wrongly.\n");
            succeeded = false;
        }

        if (!Ext2ReadFile(device, superblock, &inode, hole_offset, KernelFileBuffer, 64U,
                          &read) ||
            (read != 64U) || !KernelFileBufferIsZero(read))
        {
            KernelWriteString("  A hole did not read as zeroes.\n");
            succeeded = false;
        }
    }

    /* A directory is traversed and not read. */
    if (Ext2ResolvePath(device, superblock, "/sub", &inode) &&
        Ext2ReadFile(device, superblock, &inode, 0U, KernelFileBuffer, 64U, &read))
    {
        KernelWriteString("  A directory was read as a stream of bytes.\n");
        succeeded = false;
    }

    /* --- The symbolic links. --- */

    if (!Ext2ResolvePathNoFollow(device, superblock, "/link-fast", &inode) ||
        (inode.number != KERNEL_VOLUME_FAST_LINK_INODE) ||
        !Ext2InodeIsFastSymbolicLink(superblock, &inode) ||
        !Ext2ReadSymbolicLink(device, superblock, &inode, target, sizeof target) ||
        !KernelSameString(target, KERNEL_VOLUME_FAST_LINK_TARGET))
    {
        KernelWriteString("  A target held within its inode was not read.\n");
        succeeded = false;
    }

    if (!Ext2ResolvePathNoFollow(device, superblock, "/link-slow", &inode) ||
        (inode.number != KERNEL_VOLUME_SLOW_LINK_INODE) ||
        Ext2InodeIsFastSymbolicLink(superblock, &inode) ||
        !Ext2ReadSymbolicLink(device, superblock, &inode, target, sizeof target) ||
        !KernelSameString(target, KERNEL_VOLUME_SLOW_LINK_TARGET))
    {
        KernelWriteString("  A target held in a block was not read.\n");
        succeeded = false;
    }

    /* A target the caller has no room for is refused rather than truncated. */
    if (Ext2ResolvePathNoFollow(device, superblock, "/link-slow", &inode) &&
        Ext2ReadSymbolicLink(device, superblock, &inode, target,
                             sizeof(KERNEL_VOLUME_SLOW_LINK_TARGET) - 1U))
    {
        KernelWriteString("  A target longer than the buffer was accepted.\n");
        succeeded = false;
    }

    /*
     * Resolution through the links. The fast link names a directory by a
     * relative target, so it is resolved against the root, which holds the link;
     * the slow link names a file by an absolute target that walks through the
     * subdirectory eight times before descending into it.
     */
    if (!KernelPathIs(device, superblock, "/link-fast", KERNEL_VOLUME_SUB_INODE) ||
        !KernelPathIs(device, superblock, "/link-fast/", KERNEL_VOLUME_SUB_INODE) ||
        !KernelPathIs(device, superblock, "/link-fast/inner", KERNEL_VOLUME_INNER_INODE) ||
        !KernelPathIs(device, superblock, "/link-fast/../file", KERNEL_VOLUME_FILE_INODE) ||
        !KernelPathIs(device, superblock, "/link-slow", KERNEL_VOLUME_INNER_INODE))
    {
        KernelWriteString("  A path through a symbolic link did not resolve.\n");
        succeeded = false;
    }

    /*
     * The link itself, rather than what it names. A trailing separator overrides
     * the distinction: a path asserting a directory is asking for what the link
     * names, a link not being one.
     */
    if (!Ext2ResolvePathNoFollow(device, superblock, "/link-fast", &inode) ||
        (inode.number != KERNEL_VOLUME_FAST_LINK_INODE))
    {
        KernelWriteString("  A last symbolic link was followed when it should not be.\n");
        succeeded = false;
    }

    if (!Ext2ResolvePathNoFollow(device, superblock, "/link-fast/", &inode) ||
        (inode.number != KERNEL_VOLUME_SUB_INODE))
    {
        KernelWriteString("  A trailing separator did not override the link.\n");
        succeeded = false;
    }

    /* A link within the path is followed whether or not the last one is. */
    if (!Ext2ResolvePathNoFollow(device, superblock, "/link-fast/inner", &inode) ||
        (inode.number != KERNEL_VOLUME_INNER_INODE))
    {
        KernelWriteString("  A symbolic link within the path was not followed.\n");
        succeeded = false;
    }

    /*
     * A link that names itself. The format permits it — it is a valid file whose
     * contents happen to be its own name — and nothing but a depth bound stops
     * the resolver from following it until the stack is gone.
     */
    KernelStoreWord(KernelInodeField(KERNEL_VOLUME_FAST_LINK_INODE, EXT2_OFFSET_I_SIZE), 9U);
    KernelStoreText(KernelInodeField(KERNEL_VOLUME_FAST_LINK_INODE, EXT2_OFFSET_I_BLOCK),
                    "link-fast");
    (void)BufferInvalidateDevice(device);

    if (Ext2ResolvePath(device, superblock, "/link-fast", &inode))
    {
        KernelWriteString("  A symbolic link naming itself was followed to an end.\n");
        succeeded = false;
    }

    KernelComposeVolume();
    (void)BufferInvalidateDevice(device);

    /* Nothing may be asked of a null argument. */
    if (Ext2ReadFile(NULL, superblock, &inode, 0U, KernelFileBuffer, 64U, &read) ||
        Ext2ReadFile(device, superblock, NULL, 0U, KernelFileBuffer, 64U, &read) ||
        Ext2ReadFile(device, superblock, &inode, 0U, NULL, 64U, &read) ||
        Ext2ReadFile(device, superblock, &inode, 0U, KernelFileBuffer, 64U, NULL) ||
        Ext2ReadSymbolicLink(device, superblock, &inode, NULL, sizeof target) ||
        Ext2ReadSymbolicLink(device, superblock, &inode, target, 0U) ||
        Ext2ResolvePathNoFollow(device, superblock, NULL, &inode))
    {
        KernelWriteString("  A read accepted a null argument.\n");
        succeeded = false;
    }

    if (succeeded)
    {
        KernelWriteString("EXT2 files: reading, holes and symbolic links are sound.\n");
    }

    return succeeded;
}

/*
 * Restores the composed volume after something has written to it.
 *
 * The order matters and is not the order used everywhere else in this file.
 * BufferInvalidateDevice writes dirty buffers back before it discards them, so
 * composing first and invalidating afterwards would flush the writes of the test
 * just finished onto the volume just composed — restoring nothing and leaving a
 * volume that is neither what was written nor what was composed. The cache is
 * therefore emptied first, and the composition follows it.
 *
 * Every self-test before sub-task 5.6 could use the other order safely, none of
 * them having left a dirty buffer behind.
 */
static void KernelRestoreVolume(BlockDevice *device)
{
    (void)BufferInvalidateDevice(device);
    KernelComposeVolume();
    (void)BufferInvalidateDevice(device);
}

/*
 * Asserts that a volume may be altered, and that it still describes itself
 * afterwards.
 *
 * This is the first self-test in the project that writes to a filesystem, and
 * the standard it is held to differs from every one before it. A read that goes
 * wrong returns the wrong bytes to one caller; a write that goes wrong destroys
 * data and cannot be undone, and the destruction is ordinarily silent — a block
 * allocated to two files reads correctly for both of them until one of them
 * writes.
 *
 * Two things follow. The assertions are made about the volume as a whole and not
 * only about the operation performed: after every sequence below, the free counts
 * of the group and of the superblock must agree with one another and with what
 * was actually taken, which is the statement a corrupted allocator cannot
 * satisfy. And every write here is made to the device of memory, never to a disk
 * the machine carries: the volumes upon those belong to whoever booted this
 * kernel.
 */
static bool KernelVerifyWrites(BlockDevice *device, Ext2Superblock *superblock)
{
    const uint64_t indirect_base = EXT2_DIRECT_BLOCK_COUNT + KERNEL_VOLUME_POINTERS;
    const uint64_t double_base = indirect_base + (KERNEL_VOLUME_POINTERS *
                                                  KERNEL_VOLUME_POINTERS);
    Ext2Superblock reread;
    Ext2GroupDescriptor descriptor;
    Ext2Inode inode;
    uint32_t free_blocks;
    uint32_t block = 0U;
    uint32_t number = 0U;
    uint64_t moved = 0U;
    bool used = false;
    bool succeeded = true;

    KernelWriteString("EXT2 writes: asserting allocation, writing and truncation.\n");

    /* --- The bitmaps, read against the composition. --- */

    if (!Ext2BlockInUse(device, superblock, KERNEL_VOLUME_INODE_TABLE, &used) || !used ||
        !Ext2BlockInUse(device, superblock, KERNEL_VOLUME_LAST_BLOCK, &used) || used ||
        !Ext2InodeInUse(device, superblock, EXT2_ROOT_INODE, &used) || !used ||
        !Ext2InodeInUse(device, superblock, KERNEL_VOLUME_UNUSED_INODE, &used) || used ||
        !Ext2InodeInUse(device, superblock, KERNEL_VOLUME_SLOW_LINK_INODE, &used) || !used)
    {
        KernelWriteString("  A bitmap did not report the volume as it was composed.\n");
        succeeded = false;
    }

    /* --- One block, allocated and returned. --- */

    free_blocks = superblock->free_block_count;

    if (!Ext2AllocateBlock(device, superblock, 0U, &block) ||
        !Ext2BlockInUse(device, superblock, block, &used) || !used ||
        (superblock->free_block_count != (free_blocks - 1U)))
    {
        KernelWriteString("  A block was not allocated, or was not then in use.\n");
        succeeded = false;
    }
    else
    {
        /*
         * The superblock upon the volume, and not the copy in memory. An
         * allocator that decremented its own structure and did not write it back
         * would satisfy every assertion made against memory and would leave the
         * volume claiming a block it had given away.
         */
        if (!Ext2ReadSuperblock(device, &reread) ||
            (reread.free_block_count != superblock->free_block_count) ||
            !Ext2ReadGroupDescriptor(device, superblock, 0U, &descriptor) ||
            (descriptor.free_block_count != superblock->free_block_count))
        {
            KernelWriteString("  An allocation was not written back to the volume.\n");
            succeeded = false;
        }

        if (!Ext2FreeBlock(device, superblock, block) ||
            !Ext2BlockInUse(device, superblock, block, &used) || used ||
            (superblock->free_block_count != free_blocks))
        {
            KernelWriteString("  A block was not returned to the volume.\n");
            succeeded = false;
        }
    }

    /* Freeing what is already free is refused: the second free is what allows a
     * block to be given to two files at once. */
    if (Ext2FreeBlock(device, superblock, block) ||
        Ext2FreeInode(device, superblock, KERNEL_VOLUME_UNUSED_INODE, false))
    {
        KernelWriteString("  Something already free was freed a second time.\n");
        succeeded = false;
    }

    /* --- The one free inode, and the exhaustion after it. --- */

    {
        /*
         * Every free inode of the volume, taken until there are none, and then
         * returned. Exhausting the volume rather than allocating one is what
         * asserts that the free count and the bitmap describe the same set: an
         * allocator that miscounted would either stop early, leaving inodes the
         * bitmap says are free, or run past the count and issue one twice.
         */
        uint32_t taken[KERNEL_VOLUME_INODES];
        uint32_t count = 0U;
        const uint32_t available = superblock->free_inode_count;

        while ((count < KERNEL_VOLUME_INODES) &&
               Ext2AllocateInode(device, superblock, false, &number))
        {
            taken[count] = number;
            ++count;
        }

        if ((count != available) || (superblock->free_inode_count != 0U))
        {
            KernelWriteString("  The free inodes of the volume were not all issued.\n");
            succeeded = false;
        }

        /* The lowest free inode is issued first, which is inode 14: the one the
         * self-test of sub-task 5.3 requires to be empty. */
        if ((count == 0U) || (taken[0] != KERNEL_VOLUME_UNUSED_INODE))
        {
            KernelWriteString("  The lowest free inode was not the first issued.\n");
            succeeded = false;
        }

        if (Ext2AllocateInode(device, superblock, false, &number))
        {
            KernelWriteString("  An inode was allocated from a volume holding none.\n");
            succeeded = false;
        }

        for (uint32_t index = 0U; index < count; ++index)
        {
            if (!Ext2InodeInUse(device, superblock, taken[index], &used) || !used ||
                !Ext2FreeInode(device, superblock, taken[index], false))
            {
                KernelWriteString("  An inode was not returned to the volume.\n");
                succeeded = false;
                break;
            }
        }

        if (superblock->free_inode_count != available)
        {
            KernelWriteString("  The inodes returned did not restore the free count.\n");
            succeeded = false;
        }
    }

    /* An inode belonging to the filesystem is never issued and never freed. */
    if (Ext2FreeInode(device, superblock, EXT2_ROOT_INODE, true))
    {
        KernelWriteString("  A reserved inode was freed.\n");
        succeeded = false;
    }

    /* --- Writing within a file that already has the blocks. --- */

    if (!Ext2ResolvePath(device, superblock, "/sub/inner", &inode))
    {
        KernelWriteString("  The composed file was not found.\n");
        return false;
    }

    for (uint32_t index = 0U; index < 128U; ++index)
    {
        KernelFileBuffer[index] = (uint8_t)(0xA0U + (index & 0x0FU));
    }

    if (!Ext2WriteFile(device, superblock, &inode, 100U, KernelFileBuffer, 128U, &moved) ||
        (moved != 128U) || (inode.size != KERNEL_VOLUME_INNER_SIZE))
    {
        KernelWriteString("  A write within a file did not write what it was given.\n");
        succeeded = false;
    }

    if (!Ext2ReadFile(device, superblock, &inode, 100U, KernelFileBuffer, 128U, &moved) ||
        (moved != 128U))
    {
        KernelWriteString("  A file could not be read after being written.\n");
        succeeded = false;
    }
    else
    {
        for (uint32_t index = 0U; index < 128U; ++index)
        {
            if (KernelFileBuffer[index] != (uint8_t)(0xA0U + (index & 0x0FU)))
            {
                KernelWriteString("  A write did not reach the volume.\n");
                succeeded = false;
                break;
            }
        }
    }

    /* The bytes on either side of the write are untouched. */
    if (!Ext2ReadFile(device, superblock, &inode, 0U, KernelFileBuffer, 100U, &moved) ||
        !KernelFileBufferMatches(0U, moved) ||
        !Ext2ReadFile(device, superblock, &inode, 228U, KernelFileBuffer, 100U, &moved) ||
        !KernelFileBufferMatches(228U, moved))
    {
        KernelWriteString("  A write altered bytes beyond the range it was given.\n");
        succeeded = false;
    }

    /* --- Conservation: what a file gives up, it takes back. --- */

    free_blocks = superblock->free_block_count;

    if (!Ext2TruncateFile(device, superblock, &inode, 0U) || (inode.size != 0U) ||
        (inode.sector_count != 0U) || (superblock->free_block_count != (free_blocks + 2U)))
    {
        KernelWriteString("  Truncation to nothing did not return the file's blocks.\n");
        succeeded = false;
    }

    for (uint64_t index = 0U; index < KERNEL_VOLUME_INNER_SIZE; ++index)
    {
        KernelFileBuffer[index] = KernelFileByteAt(index);
    }

    if (!Ext2WriteFile(device, superblock, &inode, 0U, KernelFileBuffer,
                       KERNEL_VOLUME_INNER_SIZE, &moved) ||
        (moved != KERNEL_VOLUME_INNER_SIZE) || (inode.size != KERNEL_VOLUME_INNER_SIZE) ||
        (superblock->free_block_count != free_blocks))
    {
        KernelWriteString("  Rewriting a truncated file did not restore the volume.\n");
        succeeded = false;
    }

    if (!Ext2ReadFile(device, superblock, &inode, 0U, KernelFileBuffer,
                      KERNEL_VOLUME_INNER_SIZE, &moved) ||
        (moved != KERNEL_VOLUME_INNER_SIZE) || !KernelFileBufferMatches(0U, moved))
    {
        KernelWriteString("  A file rewritten after truncation did not read back.\n");
        succeeded = false;
    }

    /* --- Extension, and the hole a write beyond the end leaves. --- */

    if (!Ext2WriteFile(device, superblock, &inode, 4096U, KernelFileBuffer, 16U, &moved) ||
        (moved != 16U) || (inode.size != (4096U + 16U)))
    {
        KernelWriteString("  A write beyond the end did not extend the file.\n");
        succeeded = false;
    }

    if (!Ext2ReadFile(device, superblock, &inode, 2048U, KernelFileBuffer, 512U, &moved) ||
        (moved != 512U) || !KernelFileBufferIsZero(moved))
    {
        KernelWriteString("  The hole left by an extending write did not read as zeroes.\n");
        succeeded = false;
    }

    /* Truncation upward allocates nothing: the file grows by a hole. */
    free_blocks = superblock->free_block_count;

    if (!Ext2TruncateFile(device, superblock, &inode, 1U << 20) ||
        (inode.size != (1U << 20)) || (superblock->free_block_count != free_blocks))
    {
        KernelWriteString("  Truncation upward allocated blocks it need not have.\n");
        succeeded = false;
    }

    /* --- Allocation through the indirection. --- */

    if (!Ext2ResolvePath(device, superblock, "/file", &inode))
    {
        KernelWriteString("  The composed sparse file was not found.\n");
        succeeded = false;
    }
    else
    {
        /*
         * An entry of the doubly indirect block that holds nothing, so that both
         * an indirect block and a data block must be allocated to reach it. Two
         * blocks, and the difference between one and two is the whole of whether
         * a level of the walk was allocated or silently skipped.
         */
        const uint64_t offset = (double_base + KERNEL_VOLUME_POINTERS) *
                                (uint64_t)KERNEL_VOLUME_BLOCK_SIZE;

        free_blocks = superblock->free_block_count;
        KernelFileBuffer[0] = 0x5AU;

        if (!Ext2WriteFile(device, superblock, &inode, offset, KernelFileBuffer, 1U, &moved) ||
            (moved != 1U) || (superblock->free_block_count != (free_blocks - 2U)))
        {
            KernelWriteString("  A write through the indirection did not allocate a chain.\n");
            succeeded = false;
        }

        KernelFileBuffer[0] = 0U;

        if (!Ext2ReadFile(device, superblock, &inode, offset, KernelFileBuffer, 1U, &moved) ||
            (moved != 1U) || (KernelFileBuffer[0] != 0x5AU))
        {
            KernelWriteString("  A byte written through the indirection did not read back.\n");
            succeeded = false;
        }
    }

    /* --- The volume still describes itself. --- */

    if (!Ext2VerifyGroupDescriptors(device, superblock))
    {
        KernelWriteString("  The volume no longer accounts for itself: ");
        KernelWriteString(Ext2LastError());
        KernelWriteString("\n");
        succeeded = false;
    }

    /* --- A volume that may not be written is not written. --- */

    KernelRestoreVolume(device);
    KernelSetVolumeHalf(EXT2_OFFSET_STATE, (uint16_t)EXT2_ERROR_FS);
    (void)BufferInvalidateDevice(device);

    if (Ext2ReadSuperblock(device, &reread) && reread.read_only)
    {
        Ext2Inode victim;

        if (Ext2AllocateBlock(device, &reread, 0U, &block) ||
            Ext2AllocateInode(device, &reread, false, &number) ||
            Ext2FreeBlock(device, &reread, KERNEL_VOLUME_LAST_BLOCK) ||
            Ext2WriteSuperblock(device, &reread) ||
            (Ext2ReadInode(device, &reread, KERNEL_VOLUME_INNER_INODE, &victim) &&
             (Ext2WriteInode(device, &reread, &victim) ||
              Ext2WriteFile(device, &reread, &victim, 0U, KernelFileBuffer, 16U, &moved) ||
              Ext2TruncateFile(device, &reread, &victim, 0U))))
        {
            KernelWriteString("  A read-only volume was altered.\n");
            succeeded = false;
        }
    }
    else
    {
        KernelWriteString("  A volume not cleanly unmounted was not made read-only.\n");
        succeeded = false;
    }

    KernelRestoreVolume(device);

    /* Nothing may be asked of a null argument. */
    if (Ext2AllocateBlock(NULL, superblock, 0U, &block) ||
        Ext2AllocateBlock(device, superblock, 0U, NULL) ||
        Ext2AllocateInode(device, NULL, false, &number) ||
        Ext2WriteInode(device, superblock, NULL) ||
        Ext2WriteFile(device, superblock, NULL, 0U, KernelFileBuffer, 16U, &moved) ||
        Ext2TruncateFile(device, superblock, NULL, 0U) ||
        Ext2BlockInUse(device, superblock, KERNEL_VOLUME_LAST_BLOCK, NULL))
    {
        KernelWriteString("  A write accepted a null argument.\n");
        succeeded = false;
    }

    if (succeeded)
    {
        KernelWriteString("EXT2 writes: allocation, writing and truncation are sound.\n");
    }

    return succeeded;
}

/* How many entries a directory holds, for an assertion about a whole directory
 * rather than about one name within it. */
static bool KernelCountEntries(BlockDevice *device, const Ext2Superblock *superblock,
                               const Ext2Inode *directory, uint64_t *count)
{
    Ext2DirectoryCursor cursor;
    Ext2DirectoryEntry entry;

    *count = 0U;
    Ext2DirectoryOpen(&cursor, directory);

    for (;;)
    {
        const Ext2DirectoryStep step = Ext2DirectoryNext(device, superblock, &cursor, &entry);

        if (step == EXT2_DIRECTORY_FAILED)
        {
            return false;
        }

        if (step == EXT2_DIRECTORY_END)
        {
            return true;
        }

        ++*count;
    }
}

/*
 * Asserts that names may be inserted into a directory and removed from it, and
 * that files and directories may be created and destroyed.
 *
 * A directory is a linked list of records within each of its blocks, and every
 * operation here is an alteration of that list. The failures are accordingly the
 * failures of a list: a record whose length no longer reaches the next one, two
 * records overlapping, a record left in use that nothing points past. None of
 * them is visible in the operation that caused it — the directory reads
 * correctly until the traversal reaches the record that was damaged — so the
 * assertions are made by traversing the whole directory afterwards and counting
 * what comes out, and by requiring the volume to account for itself at the end.
 */
static bool KernelVerifyDirectoryWrites(BlockDevice *device, Ext2Superblock *superblock)
{
    Ext2DirectoryEntry entry;
    Ext2Inode root;
    Ext2Inode made;
    Ext2Inode found;
    uint64_t count = 0U;
    uint64_t before = 0U;
    uint32_t free_blocks;
    uint32_t free_inodes;
    uint16_t root_links;
    bool empty = false;
    bool succeeded = true;

    KernelWriteString("EXT2 names: asserting insertion, removal and creation.\n");

    if (!Ext2ReadInode(device, superblock, EXT2_ROOT_INODE, &root) ||
        !KernelCountEntries(device, superblock, &root, &before))
    {
        KernelWriteString("  The root directory could not be read.\n");
        return false;
    }

    free_blocks = superblock->free_block_count;
    free_inodes = superblock->free_inode_count;
    root_links = root.link_count;

    /* --- A name inserted, found, and removed. --- */

    if (!Ext2DirectoryInsert(device, superblock, &root, "inserted", 8U,
                             KERNEL_VOLUME_FILE_INODE, (uint8_t)EXT2_FT_REG_FILE))
    {
        KernelWriteString("  A name could not be inserted: ");
        KernelWriteString(Ext2LastError());
        KernelWriteString("\n");
        succeeded = false;
    }

    if (!Ext2DirectoryFind(device, superblock, &root, "inserted", 8U, &entry) ||
        (entry.inode != KERNEL_VOLUME_FILE_INODE) ||
        !KernelPathIs(device, superblock, "/inserted", KERNEL_VOLUME_FILE_INODE))
    {
        KernelWriteString("  An inserted name was not found by looking for it.\n");
        succeeded = false;
    }

    /*
     * The whole directory, traversed. An insertion that split a record wrongly
     * leaves the records after it unreachable or overlapping, and neither shows
     * in the name just inserted — only in the count of everything.
     */
    if (!KernelCountEntries(device, superblock, &root, &count) || (count != (before + 1U)))
    {
        KernelWriteString("  The directory no longer yields the entries it holds.\n");
        succeeded = false;
    }

    /* A name already present is refused, a directory holding one name twice
     * making the path to it ambiguous. */
    if (Ext2DirectoryInsert(device, superblock, &root, "inserted", 8U, KERNEL_VOLUME_SUB_INODE,
                            (uint8_t)EXT2_FT_DIR) ||
        Ext2DirectoryInsert(device, superblock, &root, "file", 4U, KERNEL_VOLUME_FILE_INODE,
                            (uint8_t)EXT2_FT_REG_FILE))
    {
        KernelWriteString("  A name already present was inserted a second time.\n");
        succeeded = false;
    }

    if (!Ext2DirectoryRemove(device, superblock, &root, "inserted", 8U) ||
        Ext2DirectoryFind(device, superblock, &root, "inserted", 8U, &entry) ||
        !KernelCountEntries(device, superblock, &root, &count) || (count != before))
    {
        KernelWriteString("  A name was not removed, or the directory did not recover.\n");
        succeeded = false;
    }

    /* Removing what is not there, and removing what may not be removed. */
    if (Ext2DirectoryRemove(device, superblock, &root, "inserted", 8U) ||
        Ext2DirectoryRemove(device, superblock, &root, ".", 1U) ||
        Ext2DirectoryRemove(device, superblock, &root, "..", 2U))
    {
        KernelWriteString("  A name that may not be removed was removed.\n");
        succeeded = false;
    }

    /*
     * The space a removal leaves is reused rather than the directory growing.
     * Inserting and removing the same name many times over must not consume a
     * block: the record before the removed one absorbs its space, and the next
     * insertion splits it again.
     */
    for (uint32_t attempt = 0U; attempt < 64U; ++attempt)
    {
        if (!Ext2DirectoryInsert(device, superblock, &root, "recycled", 8U,
                                 KERNEL_VOLUME_FILE_INODE, (uint8_t)EXT2_FT_REG_FILE) ||
            !Ext2DirectoryRemove(device, superblock, &root, "recycled", 8U))
        {
            KernelWriteString("  A name could not be inserted and removed repeatedly.\n");
            succeeded = false;
            break;
        }
    }

    if (superblock->free_block_count != free_blocks)
    {
        KernelWriteString("  Repeated insertion and removal consumed blocks.\n");
        succeeded = false;
    }

    /* --- A file created, written, linked and destroyed. --- */

    if (!Ext2CreateFile(device, superblock, &root, "created", 7U,
                        (uint16_t)(EXT2_S_IFREG | 0x01A4U), &made) ||
        !Ext2InodeIsRegular(&made) || (made.link_count != 1U) || (made.size != 0U) ||
        (superblock->free_inode_count != (free_inodes - 1U)))
    {
        KernelWriteString("  A file was not created.\n");
        succeeded = false;
    }
    else
    {
        uint64_t moved = 0U;

        KernelFileBuffer[0] = 0x11U;
        KernelFileBuffer[1] = 0x22U;

        if (!Ext2WriteFile(device, superblock, &made, 0U, KernelFileBuffer, 2U, &moved) ||
            (moved != 2U) ||
            !KernelPathIs(device, superblock, "/created", made.number))
        {
            KernelWriteString("  A created file could not be written or reached.\n");
            succeeded = false;
        }

        /* A second name for the same file, and the count that records it. */
        if (!Ext2Link(device, superblock, &root, "linked", 6U, &made) ||
            (made.link_count != 2U) ||
            !KernelPathIs(device, superblock, "/linked", made.number))
        {
            KernelWriteString("  A file was not given a second name.\n");
            succeeded = false;
        }

        /*
         * Removing one of two names removes the name and not the file. An unlink
         * that destroyed the file here would leave the other name leading to an
         * inode that had been freed, and quite possibly reissued.
         */
        if (!Ext2Unlink(device, superblock, &root, "created", 7U) ||
            !Ext2ReadInode(device, superblock, made.number, &found) ||
            (found.link_count != 1U) ||
            !KernelPathIs(device, superblock, "/linked", made.number))
        {
            KernelWriteString("  Removing one of two names destroyed the file.\n");
            succeeded = false;
        }

        if (!Ext2Unlink(device, superblock, &root, "linked", 6U) ||
            (superblock->free_inode_count != free_inodes) ||
            (superblock->free_block_count != free_blocks))
        {
            KernelWriteString("  Removing the last name did not destroy the file.\n");
            succeeded = false;
        }

        /*
         * The inode is free in the bitmap and is refused as a deleted file. Both
         * are asserted: the bitmap is what lets it be issued again, and the
         * refusal is what stops anything reaching it in the meantime.
         */
        if (!Ext2InodeInUse(device, superblock, made.number, &empty) || empty ||
            Ext2ReadInode(device, superblock, made.number, &found))
        {
            KernelWriteString("  An inode freed with its last name was still in use.\n");
            succeeded = false;
        }
    }

    /* --- A directory created and removed. --- */

    if (!Ext2CreateDirectory(device, superblock, &root, "made", 4U, 0x01EDU, &made) ||
        !Ext2InodeIsDirectory(&made) || (made.link_count != 2U) ||
        (made.size != KERNEL_VOLUME_BLOCK_SIZE) || (root.link_count != (root_links + 1U)))
    {
        KernelWriteString("  A directory was not created with its two links.\n");
        succeeded = false;
    }
    else
    {
        /*
         * The two entries every directory holds, resolved by the ordinary lookup
         * rather than assumed. A directory whose ".." named the wrong inode would
         * be reachable and would lead out of itself to somewhere else.
         */
        if (!KernelPathIs(device, superblock, "/made", made.number) ||
            !KernelPathIs(device, superblock, "/made/.", made.number) ||
            !KernelPathIs(device, superblock, "/made/..", EXT2_ROOT_INODE) ||
            !KernelPathIs(device, superblock, "/made/../made", made.number))
        {
            KernelWriteString("  A created directory did not hold \".\" and \"..\".\n");
            succeeded = false;
        }

        if (!Ext2DirectoryIsEmpty(device, superblock, &made, &empty) || !empty)
        {
            KernelWriteString("  A newly created directory was not empty.\n");
            succeeded = false;
        }

        /* A directory holding something is not removed. */
        if (!Ext2CreateFile(device, superblock, &made, "within", 6U,
                            (uint16_t)(EXT2_S_IFREG | 0x01A4U), &found) ||
            !Ext2DirectoryIsEmpty(device, superblock, &made, &empty) || empty)
        {
            KernelWriteString("  A directory holding a file reported itself empty.\n");
            succeeded = false;
        }

        if (Ext2RemoveDirectory(device, superblock, &root, "made", 4U))
        {
            KernelWriteString("  A directory holding a file was removed.\n");
            succeeded = false;
        }

        /* A directory may not be unlinked as a file, nor given a second name. */
        if (Ext2Unlink(device, superblock, &root, "made", 4U) ||
            Ext2Link(device, superblock, &root, "second", 6U, &made))
        {
            KernelWriteString("  A directory was treated as a file.\n");
            succeeded = false;
        }

        if (!Ext2Unlink(device, superblock, &made, "within", 6U) ||
            !Ext2RemoveDirectory(device, superblock, &root, "made", 4U) ||
            (root.link_count != root_links))
        {
            KernelWriteString("  An emptied directory was not removed.\n");
            succeeded = false;
        }
    }

    /* The root may never be removed, whatever it is named by. */
    if (Ext2RemoveDirectory(device, superblock, &root, ".", 1U))
    {
        KernelWriteString("  The root directory was removed.\n");
        succeeded = false;
    }

    /* --- Everything taken has been given back. --- */

    if ((superblock->free_block_count != free_blocks) ||
        (superblock->free_inode_count != free_inodes) ||
        !KernelCountEntries(device, superblock, &root, &count) || (count != before))
    {
        KernelWriteString("  The volume did not return to what it was.\n");
        succeeded = false;
    }

    if (!Ext2VerifyGroupDescriptors(device, superblock))
    {
        KernelWriteString("  The volume no longer accounts for itself: ");
        KernelWriteString(Ext2LastError());
        KernelWriteString("\n");
        succeeded = false;
    }

    /* --- A read-only volume holds its names. --- */

    KernelRestoreVolume(device);
    KernelSetVolumeHalf(EXT2_OFFSET_STATE, (uint16_t)EXT2_ERROR_FS);
    (void)BufferInvalidateDevice(device);

    {
        Ext2Superblock frozen;

        if (Ext2ReadSuperblock(device, &frozen) && frozen.read_only &&
            Ext2ReadInode(device, &frozen, EXT2_ROOT_INODE, &found))
        {
            if (Ext2DirectoryInsert(device, &frozen, &found, "no", 2U,
                                    KERNEL_VOLUME_FILE_INODE, (uint8_t)EXT2_FT_REG_FILE) ||
                Ext2DirectoryRemove(device, &frozen, &found, "file", 4U) ||
                Ext2CreateFile(device, &frozen, &found, "no", 2U,
                               (uint16_t)(EXT2_S_IFREG | 0x01A4U), &made) ||
                Ext2CreateDirectory(device, &frozen, &found, "no", 2U, 0x01EDU, &made) ||
                Ext2Unlink(device, &frozen, &found, "file", 4U) ||
                Ext2RemoveDirectory(device, &frozen, &found, "sub", 3U))
            {
                KernelWriteString("  A read-only volume had its names altered.\n");
                succeeded = false;
            }
        }
        else
        {
            KernelWriteString("  A volume not cleanly unmounted was not made read-only.\n");
            succeeded = false;
        }
    }

    KernelRestoreVolume(device);

    /* Nothing may be asked of a null argument, or of a name that is not one. */
    if (Ext2DirectoryInsert(device, superblock, NULL, "x", 1U, KERNEL_VOLUME_FILE_INODE, 0U) ||
        Ext2DirectoryInsert(device, superblock, &root, NULL, 1U, KERNEL_VOLUME_FILE_INODE, 0U) ||
        Ext2DirectoryInsert(device, superblock, &root, "x", 0U, KERNEL_VOLUME_FILE_INODE, 0U) ||
        Ext2DirectoryInsert(device, superblock, &root, "a/b", 3U, KERNEL_VOLUME_FILE_INODE, 0U) ||
        Ext2DirectoryInsert(device, superblock, &root, "x", 1U, 0U, 0U) ||
        Ext2DirectoryInsert(device, superblock, &root, "x", 1U, superblock->inode_count + 1U,
                            0U) ||
        Ext2DirectoryRemove(device, superblock, NULL, "x", 1U) ||
        Ext2CreateFile(device, superblock, &root, "d", 1U, (uint16_t)EXT2_S_IFDIR, &made) ||
        Ext2CreateDirectory(device, superblock, NULL, "x", 1U, 0x01EDU, &made) ||
        Ext2DirectoryIsEmpty(device, superblock, &root, NULL))
    {
        KernelWriteString("  A directory operation accepted what it should refuse.\n");
        succeeded = false;
    }

    if (succeeded)
    {
        KernelWriteString("EXT2 names: insertion, removal and creation are sound.\n");
    }

    return succeeded;
}

/*
 * Asserts that a directory is traversed as the format lays it out, and that a
 * path is resolved to the inode it names.
 *
 * A directory is the first structure of the volume whose contents are variable
 * rather than fixed: a superblock lies at a known offset, a descriptor is 32
 * bytes and an inode is 128, but an entry is as long as its record length says
 * and the next one begins wherever that lands. Every mistake in reading it is
 * therefore self-propagating — one record length taken from the wrong offset, or
 * one entry advanced by the length of its name rather than by its record length,
 * and every entry after it in the block is read from the middle of something
 * else. The names that come out of that are not obviously wrong; they are
 * fragments of real names, and a lookup that fails to find a file that is there
 * is indistinguishable from a file that is not.
 *
 * The traversal is therefore asserted entry by entry against the layout the
 * volume was composed with, and not merely counted.
 */
static bool KernelVerifyDirectories(BlockDevice *device, const Ext2Superblock *superblock)
{
    static const char *const expected_names[] = {".",   "..",        "file",
                                                "sub", "link-fast", "link-slow"};
    static const uint32_t expected_inodes[] = {
        EXT2_ROOT_INODE,          EXT2_ROOT_INODE,               KERNEL_VOLUME_FILE_INODE,
        KERNEL_VOLUME_SUB_INODE,  KERNEL_VOLUME_FAST_LINK_INODE, KERNEL_VOLUME_SLOW_LINK_INODE};
    static const uint8_t expected_types[] = {
        (uint8_t)EXT2_FT_DIR,     (uint8_t)EXT2_FT_DIR,     (uint8_t)EXT2_FT_REG_FILE,
        (uint8_t)EXT2_FT_DIR,     (uint8_t)EXT2_FT_SYMLINK, (uint8_t)EXT2_FT_SYMLINK};
    const size_t expected_count = sizeof(expected_names) / sizeof(expected_names[0]);
    const size_t root_block = KernelVolumeBlock(KERNEL_VOLUME_ROOT_DATA);
    Ext2DirectoryCursor cursor;
    Ext2DirectoryEntry entry;
    Ext2Inode root;
    Ext2Inode subdirectory;
    size_t counted = 0U;
    bool succeeded = true;

    KernelWriteString("EXT2 directories: asserting traversal and path resolution.\n");

    if (!Ext2ReadInode(device, superblock, EXT2_ROOT_INODE, &root))
    {
        KernelWriteString("  The root inode was refused: ");
        KernelWriteString(Ext2LastError());
        KernelWriteString("\n");
        return false;
    }

    /*
     * The root, entry by entry. The unused record standing between "file" and
     * "sub" must be passed over, and the final record, whose length runs to the
     * end of the block, must end the traversal rather than yield an entry.
     */
    Ext2DirectoryOpen(&cursor, &root);

    for (;;)
    {
        const Ext2DirectoryStep step = Ext2DirectoryNext(device, superblock, &cursor, &entry);

        if (step == EXT2_DIRECTORY_FAILED)
        {
            KernelWriteString("  The root directory could not be traversed: ");
            KernelWriteString(Ext2LastError());
            KernelWriteString("\n");
            return false;
        }

        if (step == EXT2_DIRECTORY_END)
        {
            break;
        }

        if (counted >= expected_count)
        {
            KernelWriteString("  The root directory yielded more entries than it holds.\n");
            succeeded = false;
            break;
        }

        if (!KernelSameString(entry.name, expected_names[counted]) ||
            (entry.inode != expected_inodes[counted]) ||
            (entry.file_type != expected_types[counted]))
        {
            KernelWriteString("  An entry of the root directory was read wrongly: ");
            KernelWriteString(entry.name);
            KernelWriteString("\n");
            succeeded = false;
        }

        ++counted;
    }

    if (counted != expected_count)
    {
        KernelWriteString("  The root directory did not yield the entries it holds.\n");
        succeeded = false;
    }

    /* A name the directory holds is found by looking for it. */
    if (!Ext2DirectoryFind(device, superblock, &root, "file", 4U, &entry) ||
        (entry.inode != KERNEL_VOLUME_FILE_INODE) || (entry.record_length != 16U) ||
        (entry.block != KERNEL_VOLUME_ROOT_DATA) || (entry.offset != 24U))
    {
        KernelWriteString("  A name the root directory holds was not found where it "
                          "stands.\n");
        succeeded = false;
    }

    /*
     * A name is matched by its whole length and not by a prefix of it. The
     * comparison is given a length rather than a terminator, and one that
     * stopped at the shorter of the two would match "fil" against "file".
     */
    if (Ext2DirectoryFind(device, superblock, &root, "fil", 3U, &entry) ||
        Ext2DirectoryFind(device, superblock, &root, "files", 5U, &entry) ||
        Ext2DirectoryFind(device, superblock, &root, "file", 3U, &entry))
    {
        KernelWriteString("  A name was matched against a prefix of another.\n");
        succeeded = false;
    }

    /* The name standing upon the unused record is not a name. */
    if (Ext2DirectoryFind(device, superblock, &root, "removed", 7U, &entry))
    {
        KernelWriteString("  The name upon an unused record was found.\n");
        succeeded = false;
    }

    /*
     * Path resolution. The root is named by the separator alone; repeated
     * separators are one; "." and ".." are resolved as the ordinary entries they
     * are, the ".." of the root naming the root itself; and a path of two
     * components reaches the file within the subdirectory.
     */
    if (!KernelPathIs(device, superblock, "/", EXT2_ROOT_INODE) ||
        !KernelPathIs(device, superblock, "///", EXT2_ROOT_INODE) ||
        !KernelPathIs(device, superblock, "/.", EXT2_ROOT_INODE) ||
        !KernelPathIs(device, superblock, "/..", EXT2_ROOT_INODE) ||
        !KernelPathIs(device, superblock, "/file", KERNEL_VOLUME_FILE_INODE) ||
        !KernelPathIs(device, superblock, "/sub", KERNEL_VOLUME_SUB_INODE) ||
        !KernelPathIs(device, superblock, "/sub/", KERNEL_VOLUME_SUB_INODE) ||
        !KernelPathIs(device, superblock, "/sub/.", KERNEL_VOLUME_SUB_INODE) ||
        !KernelPathIs(device, superblock, "/sub/..", EXT2_ROOT_INODE) ||
        !KernelPathIs(device, superblock, "/sub/../file", KERNEL_VOLUME_FILE_INODE) ||
        !KernelPathIs(device, superblock, "/sub/inner", KERNEL_VOLUME_INNER_INODE) ||
        !KernelPathIs(device, superblock, "//sub///inner", KERNEL_VOLUME_INNER_INODE))
    {
        KernelWriteString("  A path did not resolve to the inode it names.\n");
        succeeded = false;
    }

    /*
     * The refusals. A relative path has nothing to be resolved against; a
     * component that does not exist cannot be traversed; and a component that is
     * not a directory holds no names, whether it stands within the path or is
     * asserted to be a directory by a separator at the end of it.
     */
    if (!KernelPathRefused(device, superblock, "file") ||
        !KernelPathRefused(device, superblock, "") ||
        !KernelPathRefused(device, superblock, "/missing") ||
        !KernelPathRefused(device, superblock, "/sub/missing") ||
        !KernelPathRefused(device, superblock, "/removed") ||
        !KernelPathRefused(device, superblock, "/file/") ||
        !KernelPathRefused(device, superblock, "/file/inner") ||
        !KernelPathRefused(device, superblock, "/sub/inner/"))
    {
        KernelWriteString("  A path that names nothing was resolved.\n");
        succeeded = false;
    }

    /* The subdirectory is a directory, and holds what was composed within it. */
    if (!Ext2ResolvePath(device, superblock, "/sub", &subdirectory) ||
        !Ext2InodeIsDirectory(&subdirectory) ||
        !Ext2DirectoryFind(device, superblock, &subdirectory, "inner", 5U, &entry) ||
        (entry.inode != KERNEL_VOLUME_INNER_INODE) ||
        (entry.file_type != (uint8_t)EXT2_FT_REG_FILE))
    {
        KernelWriteString("  The subdirectory did not hold what was composed within it.\n");
        succeeded = false;
    }

    /* A file is not a directory, and holds no entries whatever its data is. */
    if (Ext2ReadInode(device, superblock, KERNEL_VOLUME_FILE_INODE, &subdirectory))
    {
        Ext2DirectoryOpen(&cursor, &subdirectory);

        if (Ext2DirectoryNext(device, superblock, &cursor, &entry) != EXT2_DIRECTORY_FAILED)
        {
            KernelWriteString("  A regular file was traversed as a directory.\n");
            succeeded = false;
        }
    }

    /*
     * A record contradicting the format is refused. Each of these is a rule the
     * traversal depends upon to terminate or to stay within its block: a record
     * length below the header cannot be advanced past; one that is not a multiple
     * of four leaves the next entry unaligned; one reaching beyond the block
     * contradicts the rule that no entry spans two; a name longer than its record
     * would be read out of the entry that follows; an inode number beyond the
     * volume names nothing; and a name holding the separator is reachable by no
     * path.
     */
    if (!KernelDirectoryRefusedWith(device, superblock,
                                    root_block + EXT2_OFFSET_DE_RECORD_LENGTH, 0U, 2U) ||
        !KernelDirectoryRefusedWith(device, superblock,
                                    root_block + EXT2_OFFSET_DE_RECORD_LENGTH, 14U, 2U) ||
        !KernelDirectoryRefusedWith(device, superblock,
                                    root_block + EXT2_OFFSET_DE_RECORD_LENGTH,
                                    KERNEL_VOLUME_BLOCK_SIZE + 4U, 2U) ||
        !KernelDirectoryRefusedWith(device, superblock,
                                    root_block + EXT2_OFFSET_DE_NAME_LENGTH, 200U, 1U) ||
        !KernelDirectoryRefusedWith(device, superblock, root_block + EXT2_OFFSET_DE_INODE,
                                    superblock->inode_count + 1U, 4U) ||
        !KernelDirectoryRefusedWith(device, superblock, root_block + EXT2_OFFSET_DE_NAME,
                                    (uint32_t)EXT2_PATH_SEPARATOR, 1U))
    {
        KernelWriteString("  A directory entry contradicting the format was accepted.\n");
        succeeded = false;
    }

    /*
     * A directory occupies whole blocks, and holds at least its own entry. A
     * size that is not a multiple of the block size describes a final block
     * ending in the middle of a record.
     */
    if (!KernelDirectoryRefusedWith(device, superblock,
                                    KernelInodeField(EXT2_ROOT_INODE, EXT2_OFFSET_I_SIZE),
                                    KERNEL_VOLUME_BLOCK_SIZE - 24U, 4U) ||
        !KernelDirectoryRefusedWith(device, superblock,
                                    KernelInodeField(EXT2_ROOT_INODE, EXT2_OFFSET_I_SIZE), 0U,
                                    4U))
    {
        KernelWriteString("  A directory whose size cannot be traversed was accepted.\n");
        succeeded = false;
    }

    /*
     * The file type an entry declares must agree with the format of the inode it
     * names. The entry for "file" is made to declare a directory; the inode it
     * names is a regular file, and the path must be refused rather than resolved
     * to a file the caller will then treat as a directory.
     */
    if (!KernelPathRefusedWith(device, superblock, root_block + 24U + EXT2_OFFSET_DE_FILE_TYPE,
                               (uint8_t)EXT2_FT_DIR, "/file"))
    {
        KernelWriteString("  An entry contradicting the inode it names was accepted.\n");
        succeeded = false;
    }

    if (!KernelVerifyEntryReadings(device))
    {
        succeeded = false;
    }

    /* Nothing may be asked of a null argument. */
    if (Ext2ResolvePath(NULL, superblock, "/", &root) ||
        Ext2ResolvePath(device, superblock, NULL, &root) ||
        Ext2ResolvePath(device, superblock, "/", NULL) ||
        Ext2DirectoryFind(device, superblock, NULL, "file", 4U, &entry) ||
        Ext2DirectoryFind(device, superblock, &root, NULL, 4U, &entry) ||
        Ext2DirectoryFind(device, superblock, &root, "file", 0U, &entry) ||
        (Ext2DirectoryNext(device, superblock, NULL, &entry) != EXT2_DIRECTORY_FAILED))
    {
        KernelWriteString("  A directory operation accepted a null argument.\n");
        succeeded = false;
    }

    if (succeeded)
    {
        KernelWriteString("EXT2 directories: traversal and path resolution are sound.\n");
    }

    return succeeded;
}

/*
 * Asserts that the superblock of a volume is read as it stands, and that a
 * volume this kernel must not address is refused rather than read hopefully.
 *
 * A filesystem parser fails silently by construction: every field it reads is a
 * number, and a number read from the wrong offset is still a number. A block
 * size taken from the fragment size, a count read as a half where the format
 * stores a word, an offset four bytes adrift — each yields a volume that looks
 * plausible and addresses the wrong blocks for the rest of the machine's life.
 * The assertions below name the value that each field must have, which is the
 * only form of assertion that catches that.
 */
void KernelVerifyExt2(void)
{
    BlockDevice *device;
    Ext2Superblock superblock;
    bool succeeded = true;

    device = BlockRegister("mem0", &KernelMemoryDeviceOperations, NULL, BLOCK_SIZE_DEFAULT,
                           KERNEL_MEMORY_DEVICE_BLOCKS, false);

    if (device == NULL)
    {
        KernelWriteString("  A device of memory could not be registered.\n");
        KernelWriteString("Volume self-test FAILED.\n");
        return;
    }

    KernelComposeVolume();
    (void)BufferInvalidateDevice(device);

    if (!Ext2ReadSuperblock(device, &superblock))
    {
        KernelWriteString("  A well-formed volume was refused: ");
        KernelWriteString(Ext2LastError());
        KernelWriteString("\n");
        KernelWriteString("Volume self-test FAILED.\n");
        (void)BufferInvalidateDevice(device);
        (void)BlockUnregister(device);
        return;
    }

    /*
     * Every field is compared against the value composed above. A parser reading
     * the right number from the wrong offset is the failure this catches, and
     * only naming the values catches it.
     */
    if ((superblock.magic != EXT2_SUPER_MAGIC) || (superblock.revision != EXT2_DYNAMIC_REV) ||
        (superblock.inode_count != KERNEL_VOLUME_INODES) || (superblock.block_count != 128U) ||
        (superblock.reserved_block_count != 6U) ||
        (superblock.free_block_count != KERNEL_VOLUME_FREE_BLOCKS) ||
        (superblock.free_inode_count != KERNEL_VOLUME_FREE_INODES) ||
        (superblock.first_data_block != 1U) ||
        (superblock.blocks_per_group != 8192U) ||
        (superblock.inodes_per_group != KERNEL_VOLUME_INODES) ||
        (superblock.state != EXT2_VALID_FS))
    {
        KernelWriteString("  A field of the superblock was read from the wrong place.\n");
        succeeded = false;
    }

    /* The block size is derived, not stored, and the geometry follows from it. */
    if ((superblock.block_size != 1024U) || (superblock.sectors_per_block != 2U) ||
        (superblock.group_count != 1U) || (Ext2GroupCount(&superblock) != 1U))
    {
        KernelWriteString("  The geometry derived from the superblock is wrong.\n");
        succeeded = false;
    }

    /* The revision 1 fields, including the label, which is padded and not
     * terminated upon the volume. */
    if ((superblock.first_inode != EXT2_GOOD_OLD_FIRST_INODE) ||
        (superblock.inode_size != EXT2_GOOD_OLD_INODE_SIZE) ||
        (superblock.feature_incompatible != EXT2_FEATURE_INCOMPAT_FILETYPE) ||
        (superblock.feature_read_only != EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER) ||
        (superblock.volume_name[0] != 'o') || (superblock.volume_name[8] != 't') ||
        (superblock.volume_name[9] != '\0'))
    {
        KernelWriteString("  The revision 1 fields were not read correctly.\n");
        succeeded = false;
    }

    /* A volume declaring only features this kernel implements may be written. */
    if (superblock.read_only)
    {
        KernelWriteString("  A volume this kernel fully implements was marked read-only.\n");
        succeeded = false;
    }

    /* The refusals. Each is a volume this kernel must not address as it stands. */
    if (!KernelVolumeRefusedWith(device, EXT2_OFFSET_MAGIC, 0x1234U, true))
    {
        KernelWriteString("  A volume bearing no magic number was accepted.\n");
        succeeded = false;
    }

    if (!KernelVolumeRefusedWith(device, EXT2_OFFSET_REVISION, 2U, false))
    {
        KernelWriteString("  A volume of an unknown revision was accepted.\n");
        succeeded = false;
    }

    if (!KernelVolumeRefusedWith(device, EXT2_OFFSET_LOG_BLOCK_SIZE, 4U, false))
    {
        KernelWriteString("  A block size beyond this kernel was accepted.\n");
        succeeded = false;
    }

    if (!KernelVolumeRefusedWith(device, EXT2_OFFSET_FIRST_DATA_BLOCK, 0U, false))
    {
        KernelWriteString("  A first data block contradicting the block size was "
                          "accepted.\n");
        succeeded = false;
    }

    if (!KernelVolumeRefusedWith(device, EXT2_OFFSET_BLOCKS_PER_GROUP, 0U, false) ||
        !KernelVolumeRefusedWith(device, EXT2_OFFSET_BLOCK_COUNT, 0U, false))
    {
        KernelWriteString("  A degenerate geometry was accepted.\n");
        succeeded = false;
    }

    /*
     * The group count is derivable from the blocks and from the inodes, and the
     * two must agree. Halving the inodes per group leaves a volume every other
     * rule accepts.
     */
    if (!KernelVolumeRefusedWith(device, EXT2_OFFSET_INODES_PER_GROUP, 8U, false))
    {
        KernelWriteString("  A volume whose two group counts disagree was accepted.\n");
        succeeded = false;
    }

    if (!KernelVolumeRefusedWith(device, EXT2_OFFSET_FREE_BLOCKS, 1000U, false))
    {
        KernelWriteString("  A volume reporting more free blocks than it holds was "
                          "accepted.\n");
        succeeded = false;
    }

    if (!KernelVolumeRefusedWith(device, EXT2_OFFSET_INODE_SIZE, 100U, true) ||
        !KernelVolumeRefusedWith(device, EXT2_OFFSET_INODE_SIZE, 2048U, true))
    {
        KernelWriteString("  An inode size that is not a power of two within a block was "
                          "accepted.\n");
        succeeded = false;
    }

    if (!KernelVolumeRefusedWith(device, EXT2_OFFSET_FIRST_INODE, 2U, false))
    {
        KernelWriteString("  A first usable inode among the reserved ones was accepted.\n");
        succeeded = false;
    }

    /*
     * An incompatible feature this kernel lacks makes the volume unreadable; a
     * read-only compatible one makes it unwritable. The distinction is the whole
     * purpose of the two fields, so both directions are asserted.
     */
    if (!KernelVolumeRefusedWith(device, EXT2_OFFSET_FEATURE_INCOMPAT,
                                 EXT2_FEATURE_INCOMPAT_RECOVER, false))
    {
        KernelWriteString("  A volume requiring an unimplemented feature was accepted.\n");
        succeeded = false;
    }

    KernelSetVolumeWord(EXT2_OFFSET_FEATURE_RO_COMPAT, EXT2_FEATURE_RO_COMPAT_BTREE_DIR);
    (void)BufferInvalidateDevice(device);

    if (!Ext2ReadSuperblock(device, &superblock) || !superblock.read_only)
    {
        KernelWriteString("  A volume with an unimplemented read-only feature was not "
                          "made read-only.\n");
        succeeded = false;
    }

    KernelComposeVolume();
    KernelSetVolumeHalf(EXT2_OFFSET_STATE, (uint16_t)EXT2_ERROR_FS);
    (void)BufferInvalidateDevice(device);

    if (!Ext2ReadSuperblock(device, &superblock) || !superblock.read_only)
    {
        KernelWriteString("  A volume that was not cleanly unmounted was not made "
                          "read-only.\n");
        succeeded = false;
    }

    /* A volume of revision 0 states neither inode size nor first inode. */
    KernelComposeVolume();
    KernelSetVolumeWord(EXT2_OFFSET_REVISION, EXT2_GOOD_OLD_REV);
    KernelSetVolumeWord(EXT2_OFFSET_FEATURE_INCOMPAT, EXT2_FEATURE_INCOMPAT_RECOVER);
    (void)BufferInvalidateDevice(device);

    if (!Ext2ReadSuperblock(device, &superblock) ||
        (superblock.inode_size != EXT2_GOOD_OLD_INODE_SIZE) ||
        (superblock.first_inode != EXT2_GOOD_OLD_FIRST_INODE) ||
        (superblock.feature_incompatible != 0U) || (superblock.volume_name[0] != '\0'))
    {
        KernelWriteString("  A volume of revision 0 was not given its fixed values.\n");
        succeeded = false;
    }

    KernelComposeVolume();
    (void)BufferInvalidateDevice(device);

    /*
     * The block group descriptor table, read from the volume just restored. It
     * is asserted here rather than in a self-test of its own because it can only
     * be read through a superblock, and this is where a valid one exists.
     */
    if (!Ext2ReadSuperblock(device, &superblock) || !KernelVerifyGroups(device, &superblock))
    {
        succeeded = false;
    }

    /* The inodes, read through the descriptor table just asserted. */
    KernelComposeVolume();
    (void)BufferInvalidateDevice(device);

    if (!Ext2ReadSuperblock(device, &superblock) || !KernelVerifyInodes(device, &superblock))
    {
        succeeded = false;
    }

    /* The directories, traversed through the inodes just asserted. */
    KernelComposeVolume();
    (void)BufferInvalidateDevice(device);

    if (!Ext2ReadSuperblock(device, &superblock) ||
        !KernelVerifyDirectories(device, &superblock))
    {
        succeeded = false;
    }

    /* The contents of the files, reached through the paths just asserted. */
    KernelComposeVolume();
    (void)BufferInvalidateDevice(device);

    if (!Ext2ReadSuperblock(device, &superblock) || !KernelVerifyFiles(device, &superblock))
    {
        succeeded = false;
    }

    /* The alteration of a volume, upon the device of memory alone. */
    KernelComposeVolume();
    (void)BufferInvalidateDevice(device);

    if (!Ext2ReadSuperblock(device, &superblock) || !KernelVerifyWrites(device, &superblock))
    {
        succeeded = false;
    }

    /* The alteration of a directory, which is what turns an inode into a file
     * somebody can name. */
    KernelRestoreVolume(device);

    if (!Ext2ReadSuperblock(device, &superblock) ||
        !KernelVerifyDirectoryWrites(device, &superblock))
    {
        succeeded = false;
    }

    KernelRestoreVolume(device);

    /* A device with nowhere to put a superblock, and requests without one. */
    if (Ext2ReadSuperblock(NULL, &superblock) || Ext2ReadSuperblock(device, NULL))
    {
        KernelWriteString("  A degenerate request was accepted.\n");
        succeeded = false;
    }

    (void)BufferInvalidateDevice(device);
    (void)BlockUnregister(device);

    device = BlockRegister("mem1", &KernelMemoryDeviceOperations, NULL, BLOCK_SIZE_DEFAULT, 2U,
                           false);

    if ((device == NULL) || Ext2ReadSuperblock(device, &superblock))
    {
        KernelWriteString("  A device too short to hold a superblock was accepted.\n");
        succeeded = false;
    }

    if (device != NULL)
    {
        (void)BufferInvalidateDevice(device);
        (void)BlockUnregister(device);
    }

    KernelWriteString(succeeded ? "Volume self-test passed.\n" : "Volume self-test FAILED.\n");
}
