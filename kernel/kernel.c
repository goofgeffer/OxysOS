/*
 * File: kernel/kernel.c
 * Purpose: Contains the C entry point of the Oxys-OS kernel. It validates the
 *          state established by the boot loader, initialises the early
 *          diagnostic output devices, presents the system identification banner,
 *          and halts the processor pending the subsystems of subsequent phases.
 * Key functions: KernelMain, KernelPanic, KernelHalt, KernelReportBootState.
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
 *   - Intel 64 and IA-32 Architectures Software Developer's Manual, Volume 2B,
 *     "HLT": the instruction halts the processor until an interrupt, a debug
 *     exception, a non-maskable interrupt, or a reset occurs.
 */

#include <oxys/kernel.h>
#include <oxys/vga.h>
#include <oxys/serial.h>

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
static void KernelWriteHexadecimal(uint64_t value)
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
 * Writes a string to both the text console and the serial port, so that the
 * diagnostic record is complete irrespective of which device the operator is
 * observing.
 */
static void KernelWriteString(const char *string)
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
 * Emits a record of the state received from the boot loader. The total size
 * field of the Multiboot2 information structure is read in order to demonstrate
 * that the structure is reachable through the higher-half mapping; the
 * systematic parsing of its tags is deferred to Phase 2, sub-task 2.1.
 */
static void KernelReportBootState(uint32_t multiboot_information_address)
{
    const uint32_t *information_structure;
    uint32_t total_size;

    KernelWriteString("Multiboot2 information structure at physical address ");
    KernelWriteHexadecimal((uint64_t)multiboot_information_address);
    KernelWriteString(".\n");

    /*
     * The Multiboot2 Specification, Section 3.6, requires the structure to be
     * aligned on an 8-byte boundary. A misaligned address indicates a defective
     * boot loader and is treated as unrecoverable.
     */
    if ((multiboot_information_address & 0x07U) != 0U)
    {
        KernelPanic("The Multiboot2 information structure is misaligned.");
    }

    information_structure =
        (const uint32_t *)(uintptr_t)PhysicalToVirtual((PhysicalAddress)multiboot_information_address);
    total_size = information_structure[0];

    KernelWriteString("Multiboot2 information structure total size: ");
    KernelWriteHexadecimal((uint64_t)total_size);
    KernelWriteString(" bytes.\n");
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
    KernelReportBootState(multiboot_information_address);

    VgaSetColour(VGA_COLOUR_LIGHT_GREEN, VGA_COLOUR_BLACK);
    KernelWriteString("Phase 1 initialisation complete.\n");

    VgaSetColour(VGA_COLOUR_LIGHT_GREY, VGA_COLOUR_BLACK);
    KernelWriteString("No further subsystems are implemented. Halting.\n");

    KernelHalt();
}
