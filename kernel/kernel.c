/*
 * File: kernel/kernel.c
 * Purpose: Contains the C entry point of the Oxys-OS kernel. It validates the
 *          state established by the boot loader, initialises in dependency order
 *          every subsystem the kernel presently has, runs the boot-time
 *          self-tests declared in <oxys/verify.h>, mounts a volume the machine
 *          carries at the root, and then either enters the keyboard echo loop,
 *          where a keyboard is present, or halts the processor where none is.
 * Key functions: KernelMain, KernelPanic, KernelHalt, KernelWriteString,
 *          KernelWriteHexadecimal, KernelWriteDecimal,
 *          KernelCommandLineHasOption, KernelMountRootVolume, KernelEchoLoop,
 *          KernelEchoBackspace, KernelSerialCursorToColumn.
 * References:
 *   - Multiboot2 Specification 2.0, Section 3.3 ("I386 machine state"): EAX
 *     contains 0x36D76289 and EBX the physical address of the Multiboot2
 *     information structure. Those values are preserved by boot/boot.asm and
 *     supplied as the arguments of this function.
 *   - Multiboot2 Specification 2.0, Section 3.6 ("Boot information format"): the
 *     information structure commences with a 32-bit total size followed by a
 *     32-bit reserved field, and is aligned on an 8-byte boundary.
 *   - System V Application Binary Interface, AMD64 Architecture Processor
 *     Supplement, Section 3.2.3: the first two integer arguments are passed in
 *     RDI and RSI respectively.
 *   - Intel 64 and IA-32 Architectures Software Developer\'s Manual, Volume 2B,
 *     "HLT": the instruction halts the processor until an interrupt, a debug
 *     exception, a non-maskable interrupt, or a reset occurs.
 *   - Intel SDM, Volume 2B, "STI": the instruction\'s effect upon the interrupt
 *     flag is delayed by one instruction, so that an interrupt cannot be
 *     delivered until after the instruction following it. This is what makes the
 *     sequence STI followed immediately by HLT free of the window in which a
 *     keyboard echo loop would otherwise service an interrupt and then halt with
 *     nothing left to wake it.
 *   - docs/design/ARCHITECTURE.md, Section 4: the dependency ordering that fixes
 *     the sequence of initialisation below, and with it the order of the phases.
 *
 * The order of initialisation is the substance of this file, and it is not
 * arbitrary in a single place. Each subsystem is established only once
 * everything it reads exists, and each is asserted only once it is established;
 * where a test must be deferred past its own phase because it depends upon a
 * later one, that is said where the deferral occurs.
 *
 * The self-tests themselves are not here. Until sub-task 6.1 they were, and this
 * file had grown to some nine thousand lines of which the entry point was the
 * last two hundred and fifty. They now stand in kernel/test/, one file per
 * subsystem, declared by <oxys/verify.h>; kernel/test/README.md records the
 * arrangement and the reason for it.
 */

#include <oxys/kernel.h>
#include <oxys/verify.h>
#include <oxys/bootinfo.h>
#include <oxys/pmm.h>
#include <oxys/paging.h>
#include <oxys/addrspace.h>
#include <oxys/vmm.h>
#include <oxys/heap.h>
#include <oxys/gdt.h>
#include <oxys/tss.h>
#include <oxys/syscall.h>
#include <oxys/idt.h>
#include <oxys/interrupts.h>
#include <oxys/exceptions.h>
#include <oxys/cpu.h>
#include <oxys/pic.h>
#include <oxys/pit.h>
#include <oxys/keyboard.h>
#include <oxys/vga.h>
#include <oxys/framebuffer.h>
#include <oxys/graphics.h>
#include <oxys/serial.h>
#include <oxys/pci.h>
#include <oxys/ata.h>
#include <oxys/block.h>
#include <oxys/buffer.h>
#include <oxys/ext2.h>
#include <oxys/vfs.h>
#include <oxys/ext2_vfs.h>

/*
 * The name and version of the system, presented upon the console and emitted
 * upon the serial port at every start.
 */
#define OXYS_SYSTEM_NAME    "Oxys-OS"
#define OXYS_VERSION_STRING "0.1.0"

/*
 * Halts the processor permanently with interrupts masked. Execution does not
 * proceed beyond this function. The halt is placed within a loop because the
 * HLT instruction resumes execution upon a non-maskable interrupt or a system
 * management interrupt, neither of which is masked by the CLI instruction.
 */
static _Noreturn void KernelHalt(void)
{
    /*
     * The diagnostic channel is buffered once its interrupts are active, and the
     * halt below clears the interrupt flag permanently, so anything still queued
     * would never be carried. A machine that stops has usually just written the
     * one thing worth reading.
     */
    SerialFlush();

    for (;;)
    {
        __asm__ __volatile__("cli; hlt");
    }
}

/*
 * Converts an unsigned 64-bit value into its hexadecimal representation and
 * writes it, prefixed by "0x", to both output devices. A minimal conversion
 * routine is provided here because the formatted output facilities of the C
 * library are not implemented until Phase 7.
 */
void KernelWriteHexadecimal(uint64_t value)
{
    static const char HexadecimalDigits[] = "0123456789ABCDEF";

    /* Sixteen digits suffice for a 64-bit value, plus a null terminator. */
    char buffer[17];
    size_t index = sizeof(buffer) - 1U;

    buffer[index] = '\0';

    do
    {
        --index;
        buffer[index] = HexadecimalDigits[value & UINT64_C(0x0F)];
        value >>= 4;
    } while (value != 0U);

    VgaWriteString("0x");
    VgaWriteString(&buffer[index]);
    SerialWriteString("0x");
    SerialWriteString(&buffer[index]);
}

/*
 * Writes an unsigned value in decimal to both output devices. The digits are
 * generated least significant first and therefore emitted from the end of the
 * buffer backwards.
 */
void KernelWriteDecimal(uint64_t value)
{
    /* Twenty digits suffice for the greatest 64-bit value, plus a terminator. */
    char buffer[21];
    size_t index = sizeof(buffer) - 1U;

    buffer[index] = '\0';

    do
    {
        --index;
        buffer[index] = (char)('0' + (unsigned char)(value % 10U));
        value /= 10U;
    } while (value != 0U);

    VgaWriteString(&buffer[index]);
    SerialWriteString(&buffer[index]);
}

/*
 * Writes a string to both the text console and the serial port, so that the
 * diagnostic record is complete irrespective of which device the operator is
 * observing.
 */
void KernelWriteString(const char *string)
{
    VgaWriteString(string);
    SerialWriteString(string);
}

void KernelPanic(const char *message)
{
    VgaSetColour(VGA_COLOUR_WHITE, VGA_COLOUR_RED);
    KernelWriteString("\nKERNEL PANIC: ");
    KernelWriteString(message);
    KernelWriteString("\nThe system has been halted.\n");

    KernelHalt();
}

/*
 * The parsed, boot-protocol-neutral description of the machine. It is held at
 * file scope rather than upon the stack because it is substantial, and the boot
 * stack is only 64 KiB.
 */
BootInformation KernelBootInformation;







/*
 * True if the boot loader's command line contains the stated option as a
 * complete word.
 *
 * The kernel has no other means of being told anything at the moment it starts,
 * and one thing it must be told is whether it is permitted to write to a disk.
 * Comparing complete words rather than substrings matters: an option is a
 * decision the operator made, and a decision must not be triggered by a longer
 * word that happens to contain it.
 */
bool KernelCommandLineHasOption(const char *option)
{
    const char *const line = KernelBootInformation.command_line;
    size_t position = 0U;

    while (line[position] != '\0')
    {
        size_t length = 0U;

        while ((line[position] == ' ') || (line[position] == '\t'))
        {
            ++position;
        }

        while ((option[length] != '\0') && (line[position + length] == option[length]))
        {
            ++length;
        }

        if ((option[length] == '\0') &&
            ((line[position + length] == '\0') || (line[position + length] == ' ') ||
             (line[position + length] == '\t')))
        {
            return true;
        }

        while ((line[position] != '\0') && (line[position] != ' ') && (line[position] != '\t'))
        {
            ++position;
        }
    }

    return false;
}













/*
 * Mounts a volume the machine actually carries at the root, and reports what it
 * holds.
 *
 * It is mounted read-only unless the operator booted the entry of the GRUB menu
 * that permits this kernel to write to their volumes. A kernel that mounted a
 * stranger's disk for writing would mark it as not cleanly unmounted merely by
 * having been booted, and every such disk would then demand a check before its
 * owner could mount it again — which is a real cost imposed for nothing.
 *
 * A machine carrying no volume is not in error. `make verify` runs upon one.
 */
static void KernelMountRootVolume(void)
{
    const bool writable = KernelCommandLineHasOption("ext2-write-test");

    VfsInitialise();

    if (!Ext2VfsInitialise())
    {
        KernelWriteString("VFS: the EXT2 filesystem could not be registered.\n");
        return;
    }

    if (!VfsMountRoot("ext2", !writable))
    {
        KernelWriteString("VFS: no volume was mounted at the root: ");
        KernelWriteString(VfsLastError());
        KernelWriteString("\n");
        return;
    }

    VfsReport();
    VfsReportDirectory("/");
    KernelVfsProbeVolume();
}

/*
 * Moves the cursor of a serial terminal to a column of the current line, the
 * column being counted from one, by the sequence ECMA-48 calls CHA — Cursor
 * Character Absolute, CSI Pn G. The display driver crosses a row boundary upon a
 * backspace by moving the cursor itself, which a terminal at the far end of a
 * serial line will not do upon receiving a backspace; the movement must
 * therefore be described to it.
 *
 * The two devices agree only so far as the terminal is eighty columns wide, the
 * kernel having no way to ask it. A wider or narrower terminal will have wrapped
 * the line elsewhere and the correction will land upon the wrong column of it.
 * The proper remedy is a line discipline that knows the width of its terminal,
 * which belongs to Phase 8.
 */
static void KernelSerialCursorToColumn(size_t column)
{
    /* Two digits suffice for a column of an eighty-column line. */
    char sequence[8];
    size_t index = 0U;
    const size_t number = column + 1U;

    sequence[index] = '\x1B';
    ++index;
    sequence[index] = '[';
    ++index;

    if (number >= 10U)
    {
        sequence[index] = (char)('0' + (unsigned char)(number / 10U));
        ++index;
    }

    sequence[index] = (char)('0' + (unsigned char)(number % 10U));
    ++index;
    sequence[index] = 'G';
    ++index;
    sequence[index] = '\0';

    SerialWriteString(sequence);
}

/*
 * Echoes a backspace upon both devices as an erasure.
 *
 * A backspace moves the cursor without erasing, upon the display and upon a
 * serial terminal alike, so an echo that wrote it alone would leave the
 * character the user meant to delete upon the screen and then overwrite it with
 * whatever was typed next. The erasure is the echo's business, not the driver's:
 * the sequence steps back, writes a space over the character, and steps back
 * again to stand where the character was.
 *
 * The display is driven first, and what it did then determines what is sent to
 * the serial line: the driver may have refused to move, the cursor standing at
 * the erase limit, or it may have crossed into the row above, which is a
 * movement the serial terminal must be told about explicitly.
 */
static void KernelEchoBackspace(void)
{
    size_t row;
    size_t column;
    size_t resulting_row;
    size_t resulting_column;

    VgaCursorPosition(&row, &column);
    VgaWriteString("\b \b");
    VgaCursorPosition(&resulting_row, &resulting_column);

    if ((resulting_row == row) && (resulting_column == column))
    {
        /* The cursor stood at the erase limit; there was nothing to erase. */
        return;
    }

    if (resulting_row == row)
    {
        SerialWriteString("\b \b");
        return;
    }

    /* CUU, CSI A, moves the terminal's cursor up one line without erasing. */
    SerialWriteString("\x1B[A");
    KernelSerialCursorToColumn(resulting_column);
    SerialPutCharacter(' ');
    KernelSerialCursorToColumn(resulting_column);
}

/*
 * Echoes characters from the keyboard and from the serial line upon both output
 * devices, indefinitely.
 *
 * This is the one thing the self-tests cannot establish. They drive the decoder
 * directly, which exercises the whole of scan code set 1 upon a machine at which
 * nobody is typing, but leaves the path from the physical key to the decoder —
 * the controller raising its request line, the interrupt controller routing it,
 * the handler reading the data port — asserted only as configured state. The
 * same is true of the serial receiver, whose self-test can say what becomes of a
 * character that has arrived but not that one arrives. Here both paths are
 * exercised in full, by the only means available: a person, or a virtual machine
 * monitor, actually sending a character.
 *
 * The loop halts the processor between keystrokes rather than spinning. The
 * sequence STI followed immediately by HLT is the correct idiom and not merely a
 * compact one: Intel SDM, Volume 2B, "STI", provides that the effect of the
 * instruction is delayed by one instruction, so the HLT is executed before any
 * interrupt can be taken. Were the order reversed, or a further instruction
 * placed between them, a keystroke arriving in the interval would be serviced
 * and the processor would then halt with nothing left to wake it.
 */
static _Noreturn void KernelEchoLoop(void)
{
    VgaSetColour(VGA_COLOUR_LIGHT_CYAN, VGA_COLOUR_BLACK);
    KernelWriteString("\nEcho loop. Characters typed upon the console or sent "
                      "upon COM1 appear upon both.\n");
    KernelWriteString("A backspace erases, and crosses to the line above.\n");
    VgaSetColour(VGA_COLOUR_LIGHT_GREY, VGA_COLOUR_BLACK);

    /*
     * Everything printed up to this point is the kernel's, and everything after
     * it is the user's. The erase limit records the boundary, which is what
     * permits the backspace to cross from one row to the row above: the driver
     * cannot tell the boot log from a line of input, and would otherwise consume
     * the log a character at a time.
     */
    VgaSetEraseLimit();

    for (;;)
    {
        char character;

        __asm__ __volatile__("sti; hlt");

        /*
         * The serial line is a source of characters equally, now that its
         * receiver is interrupt-driven. Echoing them exercises the receive path
         * end to end, which the self-test cannot: it needs a character actually
         * arriving from outside the machine.
         */
        while (SerialReadCharacter(&character))
        {
            const char text[2] = { character, '\0' };

            if (character == '\b')
            {
                KernelEchoBackspace();
            }
            else
            {
                KernelWriteString(text);
            }
        }

        while (KeyboardReadCharacter(&character))
        {
            /* A one-character string, the output routines taking no other form. */
            const char text[2] = { character, '\0' };

            if (character == '\b')
            {
                KernelEchoBackspace();
            }
            else
            {
                KernelWriteString(text);
            }
        }
    }
}

void KernelMain(uint32_t multiboot_information_address, uint32_t multiboot_magic)
{
    /*
     * The serial port is initialised first, so that any subsequent failure is
     * recorded even should the display be unavailable or illegible. A negative
     * result is not an error; it indicates only that no adapter is present.
     */
    (void)SerialInitialise(SERIAL_COM1_PORT);

    VgaInitialise();

    VgaSetColour(VGA_COLOUR_LIGHT_CYAN, VGA_COLOUR_BLACK);
    KernelWriteString(OXYS_SYSTEM_NAME "\n");

    VgaSetColour(VGA_COLOUR_LIGHT_GREY, VGA_COLOUR_BLACK);
    KernelWriteString("Version " OXYS_VERSION_STRING
                      ", x86_64, long mode active, higher-half kernel.\n");

    /*
     * The magic value is validated a second time here, the first validation
     * having been performed in 32-bit mode by boot/boot.asm. The repetition
     * guards against a transfer of control that bypasses the assembly entry
     * point, and costs nothing measurable.
     */
    if (multiboot_magic != MULTIBOOT2_BOOTLOADER_MAGIC)
    {
        KernelPanic("The boot loader is not Multiboot2 compliant.");
    }

    KernelWriteString("Multiboot2 magic value verified.\n");

    /*
     * Reduce the Multiboot2 structure to the neutral description upon which the
     * remainder of the kernel depends. A failure here is unrecoverable: without a
     * memory map the physical frame allocator cannot be constructed, and without
     * that the kernel can do nothing further.
     */
    if (!BootInformationParseMultiboot2(multiboot_information_address,
                                        &KernelBootInformation))
    {
        KernelPanic("The Multiboot2 boot information structure could not be parsed.");
    }

    /*
     * The display is tested next, because it is the instrument through which
     * every later test reports.
     *
     * It was tested before the parse until sub-task 6.2, needing nothing the
     * handover had not already supplied. It cannot be any longer: from that
     * sub-task the boot loader may leave the adapter in a graphics mode, and
     * then the memory this test reads character cells back out of is not the
     * text buffer and nothing it asserts means anything. Which mode the machine
     * is in is stated by the boot information and nowhere else, so the test must
     * follow the parse in order to know whether to run at all.
     */
    KernelVerifyVga();

    BootInformationReport(&KernelBootInformation);

    PhysicalMemoryInitialise(&KernelBootInformation);
    PhysicalMemoryReport();
    KernelVerifyFrameAllocator();

    PagingInitialise(&KernelBootInformation);
    PagingReport();
    KernelVerifyPaging();

    KernelVirtualInitialise();
    KernelHeapInitialise();
    KernelVerifyAllocators();
    KernelVirtualReport();
    KernelHeapReport();

    /*
     * Phase 6, sub-task 6.2. The framebuffer the boot loader left the machine
     * with.
     *
     * It is acquired here, after the arena exists and before anything else
     * competes for it, because it is mapped out of the arena and its extent is
     * fixed by the hardware rather than chosen: a display of 1024 by 768 at four
     * bytes a pixel is three mebibytes of contiguous virtual address space, and
     * taking it first means taking it from a region nothing has fragmented.
     *
     * A false return is not a failure. It means the boot loader left the adapter
     * in a text mode, or described no display at all, and in either case the
     * VGA driver of sub-task 4.2 continues to own the screen. The report states
     * which it was.
     */
    (void)FramebufferInitialise(&KernelBootInformation);
    FramebufferReport();
    KernelVerifyFramebuffer();

    /*
     * Phase 6, sub-task 6.3. The primitives that draw upon it.
     *
     * They need nothing but the framebuffer above, and the greater part of what
     * they are asserted against is a surface composed in memory, so this runs
     * here rather than later: a fault in the arithmetic that computes a byte
     * offset into a surface is better found before anything else has drawn.
     */
    GraphicsReport();
    KernelVerifyGraphics();

    FrameReferenceInitialise();
    KernelVerifyReferenceCounting();
    PhysicalMemoryReport();

    /*
     * The table established by boot/boot.asm resides at a low address that
     * sub-task 2.3 unmapped. It must be replaced before any interrupt gate is
     * installed, because delivering an interrupt obliges the processor to read
     * the descriptor named by the gate's selector.
     */
    GdtInitialise();
    GdtReport();

    IdtInitialise();
    InterruptInitialise();
    ExceptionInitialise();

    /*
     * Phase 6, sub-task 6.1. The apparatus of a privilege transition: the
     * user-mode descriptors, the task state segment that names the stacks the
     * processor loads, and the three registers that configure SYSCALL.
     *
     * It is established here, after the gates exist, and not beside the global
     * descriptor table it extends. LTR reads the descriptor this builds and
     * raises a general-protection exception where it is malformed; done before
     * the interrupt descriptor table existed, that exception would have found no
     * gate and escalated to a reset, and the diagnosis would have been a machine
     * that reboots. Done here it is reported.
     */
    TssInitialise();

    if (!ExceptionInstallInterruptStacks())
    {
        KernelPanic("The double fault could not be given a stack of its own.");
    }

    /*
     * A processor that cannot report SYSCALL cannot run a user program at all,
     * every one capable of long mode supporting it. The kernel proceeds so that
     * the machine may still be examined, and the report and the self-test both
     * state the absence.
     */
    (void)SyscallInitialise();

    /*
     * The global descriptor table reports itself above, at its initialisation;
     * every field of that report is a constant of the table's layout and none of
     * it changes when the task state segment descriptor is filled in, so it is
     * not repeated here.
     */
    TssReport();
    SyscallReport();
    IdtReport();
    InterruptReport();
    KernelVerifyIdt();
    KernelVerifyInterruptStubs();
    KernelVerifyDispatcher();
    KernelVerifyExceptions();
    KernelVerifyCopyOnWrite();
    KernelVerifyAddressSpaces();

    /*
     * The controllers are remapped only now, after the exception handlers exist.
     * Remapping them earlier would have placed device vectors clear of the
     * exceptions without providing anywhere for them to go.
     */
    PicInitialise();
    KernelVerifyPic();
    PicReport();

    /*
     * The timer is the first device to claim a request line, and therefore the
     * first proof that the whole path from a device to a handler is sound.
     */
    PitInitialise(PIT_DEFAULT_FREQUENCY);
    KernelVerifyPit();
    PitReport();

    /*
     * The keyboard is the last device of Phase 3. A machine without one is not
     * in error, so the return value is recorded rather than acted upon; the
     * report and the self-test both accommodate its absence.
     */
    (void)KeyboardInitialise();
    KernelVerifyKeyboard();
    KeyboardReport();

    /*
     * The self-test of sub-task 6.1 runs here rather than beside the
     * initialisation, because half of it must execute SYSCALL with the interrupt
     * flag set: the assertion that IA32_FMASK clears that flag says nothing at
     * all if the flag was clear to begin with, and this is the first point in
     * the sequence at which it may be set safely.
     */
    KernelVerifyPrivilege();

    /*
     * Phase 4 begins here. The serial adapter was configured in the first
     * instruction of this function, so that a failure anywhere above would be
     * recorded; only now, the interrupt controller existing, can it be promoted
     * from polling to interrupts and become a driver rather than a routine.
     */
    SerialActivateInterrupts();
    KernelVerifySerial();
    SerialReport();
    VgaReport();

    /*
     * The bus is enumerated once every device driven so far is working, so that
     * a failure in the enumeration is reported through channels already proved.
     * Nothing is claimed or configured here; the enumeration only establishes
     * what the machine contains, which the disk driver then searches.
     */
    (void)PciInitialise();
    KernelVerifyPci();
    PciReport();

    /*
     * The disk is the last device of this phase and the first whose failure is
     * silent in the ordinary case: a driver that reads the wrong sector returns
     * data, and data that arrived is indistinguishable from data that is right
     * until something tries to interpret it.
     */
    (void)AtaInitialise();
    KernelVerifyAta();
    AtaReport();

    /*
     * Every disk found presents itself through the generic layer, which is what
     * everything above will address it by. The layer is asserted against a
     * device of memory rather than against a disk: the machine this is verified
     * upon has no disk, and one that has holds data a self-test must not write.
     */
    (void)AtaRegisterBlockDevices();
    KernelVerifyBlock();
    BlockReport();

    /*
     * The cache stands between the block layer and everything that will read a
     * medium. Its storage comes from the kernel heap, so it cannot be prepared
     * until that exists, which it has since Phase 2.
     */
    (void)BufferInitialise();
    KernelVerifyBuffer();
    BufferReport();

    /*
     * Phase 5 begins here. Nothing is mounted: the superblock of any volume the
     * machine actually carries is read and reported, and the parser itself is
     * asserted against a volume composed in memory.
     */
    KernelVerifyExt2();
    KernelReportVolumes();

    /*
     * Sub-task 5.8. The layer is asserted against the two volumes of memory it
     * composes, which is where a mount, a descriptor and a mount point crossing
     * can be stated exactly; only then is a volume the machine actually carries
     * mounted at the root, read-only unless the operator permitted otherwise.
     */
    KernelVerifyVfs();
    KernelMountRootVolume();

    PicReport();
    InterruptReport();
    PagingReport();
    AddressSpaceReport();

    VgaSetColour(VGA_COLOUR_LIGHT_GREEN, VGA_COLOUR_BLACK);
    KernelWriteString("Phase 6 initialisation complete: the apparatus of a "
                      "privilege transition stands and has been exercised.\n");

    VgaSetColour(VGA_COLOUR_LIGHT_GREY, VGA_COLOUR_BLACK);

    /*
     * With a keyboard the kernel has something to wait for, and waiting for it
     * demonstrates the interrupt path end to end. Without one there is nothing
     * further to do.
     */
    if (KeyboardIsPresent())
    {
        KernelEchoLoop();
    }

    KernelWriteString("No further subsystems are implemented. Halting.\n");

    KernelHalt();
}
