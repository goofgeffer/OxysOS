/*
 * File: graphics/faultscreen.c
 * Purpose: Draws the full-screen page that a fault the kernel cannot survive
 *          produces, composed for the fault in hand: its own title, colour,
 *          account and evidence. Faults belonging to a program draw nothing
 *          here; ExceptionDispositionOf decides which is which.
 * Key functions: FaultScreenShowException, FaultScreenShowPanic,
 *          FaultScreenWasDrawn, FaultScreenEntryCount, FaultScreenEntryAt.
 * References:
 *   - Intel 64 and IA-32 Architectures Software Developer's Manual, Volume 3A,
 *     Table 6-1: the exceptions, their mnemonics, and which deliver an error
 *     code. Section 6.13 and Figure 6-6: the selector-form error code.
 *     Section 6.15 and Figure 6-9: the page-fault error code and CR2.
 *   - docs/design/GRAPHICS.md, Sections 24 and 25.
 *
 * What this must survive.
 *
 * Every routine here runs inside a fault handler, upon a machine that has
 * already gone wrong, and the one thing it must not do is go wrong itself: a
 * fault taken while drawing a fault screen produces a double fault, and a fault
 * taken while handling that resets the machine with nothing written anywhere.
 * Four rules follow, and each is observed at the point it applies.
 *
 *   It allocates nothing. Every buffer here is file scope or automatic, and the
 *   heap is not touched — the heap being a thing a fault may have corrupted.
 *
 *   It reads no memory the fault may have poisoned without asking the paging
 *   hierarchy first. The instruction bytes and the stack words are both read
 *   through PagingTranslate, and a word that does not translate is reported as
 *   absent rather than fetched.
 *
 *   It takes no lock. From sub-task 6.13 two processors faulting at once would
 *   interleave two screens, which is the same limitation the diagnostic path
 *   already carries and is recorded with it.
 *
 *   It draws nothing at all where there is no framebuffer. A machine in a text
 *   mode has the display driver showing the report already, and drawing into
 *   memory nothing displays would be worse than useless: it would be a second
 *   thing to go wrong.
 */

#include <oxys/faultscreen.h>
#include <oxys/framebuffer.h>
#include <oxys/graphics.h>
#include <oxys/font.h>
#include <oxys/console.h>
#include <oxys/paging.h>
#include <oxys/exceptions.h>
#include <oxys/cpu.h>
#include <oxys/kernel.h>

/* The margin, in character cells, between the page's content and its edge. */
#define FAULT_MARGIN 2

/* How many bytes of the faulting instruction are reproduced. Fifteen is the
 * greatest length an x86-64 instruction may have (Intel SDM, Volume 2A, Section
 * 2.3.11), so this cannot stop short of the instruction that faulted. */
#define FAULT_OPCODE_BYTES 15U

/* How many quadwords of stack are reproduced. */
#define FAULT_STACK_WORDS 6U

static GraphicsSurface FaultSurface;
static bool FaultScreenDrawn;

/* The colours common to every screen. Only the accent changes. */
static uint32_t FaultPaper;
static uint32_t FaultInk;
static uint32_t FaultDim;
static uint32_t FaultAccent;

/*
 * The screens.
 *
 * One row for each fault the kernel cannot survive and that has something of its
 * own to say. **What reaches this file at all is decided elsewhere**, by
 * ExceptionDispositionOf: a divide by zero, an invalid opcode or an unresolved
 * page fault raised by a program belongs to that program and costs it alone, and
 * a full-screen page announcing the end of the machine for one would be a lie
 * about what happened. What arrives here is an abort, a non-maskable interrupt,
 * a malformed descriptor table, or a fault the kernel raised within itself.
 *
 * A vector absent from this table still receives a screen — the general one
 * below names it from the dispatcher's own mnemonic table — but it receives no
 * account and no evidence chosen for it, and that is the honest presentation of
 * an exception this kernel has not thought about.
 *
 * The colours are held as channel intensities and not as encoded pixels because
 * the encoding depends upon the framebuffer's channel positions, which are read
 * from the boot loader at run time and are not known when this table is
 * compiled. They are chosen to be distinguishable from one another at a glance,
 * that being the point of having more than one: a person who has seen these
 * before should know which fault it is before reading a word.
 */
static const FaultScreenEntry FaultScreens[] = {
    { 2U, "NON-MASKABLE INTERRUPT", "NMI, vector 2",
      "The platform raised a condition it could not defer. This is hardware announcing "
      "something, not a program making a mistake.",
      "The machine, not this kernel. A non-maskable interrupt is memory parity, a "
      "watchdog, or a bus error; nothing a program did can cause one, and nothing this "
      "kernel could have done differently would have prevented it.",
      230U, 60U, 190U, FAULT_EVIDENCE_CONTROL },

    { 6U, "BAD INSTRUCTION IN KERNEL", "#UD, vector 6",
      "The processor could not decode the instruction at the address below. Control "
      "within the kernel reached something that is not code.",
      "The instruction bytes. Bytes that are not an instruction mean control arrived "
      "somewhere that holds no code: a function pointer that was overwritten, or a "
      "return through a damaged stack. The stack below is the second place to look.",
      170U, 120U, 230U, FAULT_EVIDENCE_OPCODE | FAULT_EVIDENCE_STACK },

    { 8U, "DOUBLE FAULT", "#DF, vector 8",
      "The processor could not deliver an earlier exception, and raised this instead. "
      "This screen is drawn upon the stack held in the interrupt stack table.",
      "Not this fault: the one before it. A double fault is nearly always a stack that "
      "was exhausted, unmapped, or pointing at nothing when the first exception tried to "
      "push its frame.",
      220U, 40U, 40U, FAULT_EVIDENCE_STACK | FAULT_EVIDENCE_CONTROL },

    { 10U, "MALFORMED TASK STATE SEGMENT", "#TS, vector 10",
      "A task state segment named by the selector below is malformed, or its descriptor "
      "is not of the type the processor requires. The descriptor tables are the kernel's "
      "own, so this is fatal whatever raised it.",
      "The selector above, against the descriptors the global descriptor table actually "
      "holds. The report upon the serial port lists them.",
      200U, 150U, 60U, FAULT_EVIDENCE_SELECTOR },

    { 11U, "DESCRIPTOR NOT PRESENT", "#NP, vector 11",
      "A descriptor was loaded whose present flag is clear. As with the segment above, "
      "the table it came from is the kernel's own, so terminating whatever reached it "
      "would leave the same descriptor for the next thing to meet.",
      "The selector above. Either the descriptor was never written, or a selector was "
      "computed that names a slot beyond the ones that were.",
      60U, 180U, 180U, FAULT_EVIDENCE_SELECTOR },

    { 12U, "KERNEL STACK FAULT", "#SS, vector 12",
      "A stack operation within the kernel went outside its segment, or the stack "
      "segment named by the selector below is not present.",
      "The stack pointer, and whether the memory beneath it is mapped. A kernel stack "
      "that has run past what was reserved for it produces this before it produces "
      "anything else, and a double fault shortly afterwards.",
      210U, 100U, 30U, FAULT_EVIDENCE_SELECTOR | FAULT_EVIDENCE_STACK },

    { 13U, "KERNEL PROTECTION FAULT", "#GP, vector 13",
      "The kernel attempted an operation the processor's protection rules forbid: a "
      "privileged instruction, a non-canonical address, or a segment used wrongly.",
      "The error code. Where it is not zero it names the selector at fault; where it is "
      "zero the instruction bytes are the evidence, the fault being about what was done "
      "rather than to what.",
      240U, 120U, 30U, FAULT_EVIDENCE_SELECTOR | FAULT_EVIDENCE_OPCODE },

    { 14U, "KERNEL PAGE FAULT", "#PF, vector 14",
      "The kernel touched memory that has no translation, or violated the permissions of "
      "the translation it found. The address it named is below.",
      "The faulting address, and whether a translation existed for it. A page that is "
      "not present is a mapping this kernel never made; a protection violation is one it "
      "made with the wrong permissions.",
      70U, 130U, 240U, FAULT_EVIDENCE_FAULT_ADDRESS | FAULT_EVIDENCE_CONTROL },

    { 18U, "MACHINE CHECK", "#MC, vector 18",
      "The processor reported an internal or bus error. This is hardware announcing a "
      "fault in itself, not software doing something wrong.",
      "Nothing in this kernel. A machine check is an abort: the state below may not "
      "describe where the error occurred, and the machine's own error registers are the "
      "record.",
      250U, 90U, 90U, FAULT_EVIDENCE_CONTROL },

    { FAULT_SCREEN_SOFTWARE, "KERNEL PANIC", "raised by the kernel",
      "The kernel checked a condition it requires, found it false, and stopped. No "
      "processor exception was involved.",
      "The message above. It names the check that failed, and the code that made it is "
      "the code that knew what should have been true.",
      200U, 200U, 210U, 0U }
};

#define FAULT_SCREEN_COUNT (sizeof(FaultScreens) / sizeof(FaultScreens[0]))

/* The screen used for an exception this table says nothing about. It names the
 * vector from the dispatcher's mnemonics rather than pretending to an account it
 * does not have. */
static const FaultScreenEntry FaultScreenGeneral = {
    0U, "UNEXPECTED KERNEL FAULT", "",
    "The kernel raised an exception this table carries no account of, and cannot "
    "continue. The exception is named above.",
    "The vector above, in Intel SDM Volume 3A, Table 6-1. Most exceptions that reach this "
    "screen are ordinary mistakes of a program, such as a divide by zero or an "
    "instruction that is not one, which cost only the program that makes them. Reaching this screen means "
    "one was made by the kernel, where there is nothing smaller to abandon.",
    180U, 180U, 180U, FAULT_EVIDENCE_OPCODE | FAULT_EVIDENCE_STACK | FAULT_EVIDENCE_CONTROL
};

size_t FaultScreenEntryCount(void)
{
    return FAULT_SCREEN_COUNT;
}

const FaultScreenEntry *FaultScreenEntryAt(size_t index)
{
    if (index >= FAULT_SCREEN_COUNT)
    {
        return NULL;
    }

    return &FaultScreens[index];
}

bool FaultScreenWasDrawn(void)
{
    return FaultScreenDrawn;
}

/* The entry for a vector, or the general one where the table has nothing. */
static const FaultScreenEntry *FaultScreenFor(uint64_t vector)
{
    for (size_t index = 0U; index < FAULT_SCREEN_COUNT; ++index)
    {
        if (FaultScreens[index].vector == vector)
        {
            return &FaultScreens[index];
        }
    }

    return &FaultScreenGeneral;
}

/* Draws a string at a character cell, returning the column after it. Nothing
 * wraps: a line too long for the screen is cut off by the clip, which is the
 * correct outcome for a page whose text is composed to fit. */
static int32_t FaultText(int32_t column, int32_t row, const char *text, uint32_t ink)
{
    if (text == NULL)
    {
        return column;
    }

    for (size_t index = 0U; text[index] != '\0'; ++index)
    {
        FontDrawGlyphOpaque(&FaultSurface, column * (int32_t)FONT_WIDTH,
                            row * (int32_t)FONT_HEIGHT, (uint8_t)text[index], ink,
                            FaultPaper);
        ++column;
    }

    return column;
}

/*
 * Draws a sentence broken across as many lines as it needs, breaking at spaces,
 * and returns the row after the last one drawn.
 *
 * The wrapping is here rather than in the table because the width of a line is
 * not known until the framebuffer is: the same sentence occupies two lines at
 * 1280 pixels and four at 640, and a table holding pre-broken lines would be a
 * table that is wrong upon one of the two machines this kernel is tested on.
 */
static int32_t FaultParagraph(int32_t column, int32_t row, const char *text, uint32_t ink,
                              int32_t width)
{
    size_t start = 0U;

    if ((text == NULL) || (width <= 0))
    {
        return row;
    }

    while (text[start] != '\0')
    {
        size_t length = 0U;
        size_t breakpoint = 0U;

        /* How much of what remains fits, and where the last space before that
         * point is. A word longer than the line is emitted whole and overruns,
         * which no sentence here does and which is better than an endless loop. */
        while ((text[start + length] != '\0') && (length < (size_t)width))
        {
            if (text[start + length] == ' ')
            {
                breakpoint = length;
            }

            ++length;
        }

        if ((text[start + length] != '\0') && (breakpoint != 0U))
        {
            length = breakpoint;
        }

        for (size_t index = 0U; index < length; ++index)
        {
            FontDrawGlyphOpaque(&FaultSurface,
                                (column + (int32_t)index) * (int32_t)FONT_WIDTH,
                                row * (int32_t)FONT_HEIGHT, (uint8_t)text[start + index],
                                ink, FaultPaper);
        }

        ++row;
        start += length;

        while (text[start] == ' ')
        {
            ++start;
        }
    }

    return row;
}

/* Renders a 64-bit value as hexadecimal into a caller's buffer, which must hold
 * at least nineteen characters. KernelWriteHexadecimal emits to the diagnostic
 * path and this must produce a string, so the digits are generated here. */
static void FaultHexadecimal(uint64_t value, char *buffer, size_t digits)
{
    static const char Digits[] = "0123456789ABCDEF";

    buffer[0] = '0';
    buffer[1] = 'x';

    for (size_t index = 0U; index < digits; ++index)
    {
        const size_t shift = (digits - 1U - index) * 4U;

        buffer[2U + index] = Digits[(value >> shift) & UINT64_C(0x0F)];
    }

    buffer[2U + digits] = '\0';
}

/* Draws "name value" at a cell, and returns the column after it, so that several
 * may be placed along one line. */
static int32_t FaultValue(int32_t column, int32_t row, const char *name, uint64_t value,
                          size_t digits)
{
    char buffer[19];

    column = FaultText(column, row, name, FaultDim);
    column = FaultText(column, row, " ", FaultDim);
    FaultHexadecimal(value, buffer, digits);
    column = FaultText(column, row, buffer, FaultInk);

    return column + 2;
}

/* Whether an address may be read without raising a second fault. */
static bool FaultAddressIsReadable(uint64_t address)
{
    return (address >= KERNEL_VIRTUAL_BASE) && (PagingTranslate(address) != 0U);
}

/* The banner: a bar in the fault's colour, the title several times life size,
 * and the mnemonic beside it. Returns the row beneath it. */
static int32_t FaultBanner(const FaultScreenEntry *entry, const char *mnemonic,
                           int32_t scale)
{
    const GraphicsRectangle bar = { 0, 0, (int32_t)FaultSurface.width,
                                    (int32_t)FONT_HEIGHT * (scale + 2) };
    int32_t x = FAULT_MARGIN * (int32_t)FONT_WIDTH;

    GraphicsFillRectangle(&FaultSurface, bar, FaultAccent);

    /*
     * The title is drawn in the paper colour upon the accent, and not the accent
     * upon the paper. A solid bar of the fault's colour is what carries across a
     * room, and it is what a person recognises before they are close enough to
     * read a word of it.
     */
    for (size_t index = 0U; entry->title[index] != '\0'; ++index)
    {
        FontDrawGlyphScaled(&FaultSurface, x, (int32_t)FONT_HEIGHT,
                            (uint8_t)entry->title[index], FaultPaper, FaultAccent, scale);
        x += (int32_t)FONT_WIDTH * scale;
    }

    /* The mnemonic sits at the foot of the bar, at ordinary size, where it does
     * not compete with the title but is there for whoever wants the vector. */
    {
        const int32_t row = scale + 1;
        int32_t column = FAULT_MARGIN;

        for (size_t index = 0U; mnemonic[index] != '\0'; ++index)
        {
            FontDrawGlyphOpaque(&FaultSurface, column * (int32_t)FONT_WIDTH,
                                row * (int32_t)FONT_HEIGHT, (uint8_t)mnemonic[index],
                                FaultPaper, FaultAccent);
            ++column;
        }
    }

    return scale + 4;
}

/* Prepares the surface and the palette, or reports that there is nothing to draw
 * upon. */
static bool FaultScreenBegin(const FaultScreenEntry *entry)
{
    if (!GraphicsSurfaceFromFramebuffer(&FaultSurface))
    {
        return false;
    }

    /*
     * The screen changes hands here, and it must, because the console is still
     * being written to: the panic that follows a fatal exception writes to the
     * diagnostic path, and the diagnostic path includes the console, and the
     * console is upon this framebuffer.
     *
     * Its cursor stands at the foot of a screen of boot log, so every newline it
     * is given scrolls the whole framebuffer up by eight pixels. Before this
     * call was added, "KERNEL PANIC: ..." carried the banner of the screen just
     * drawn off the top of the display and shifted the rest of it by three
     * character rows — a fault screen that was composed correctly and displayed
     * wrongly, which is exactly the class of fault that only looking at it
     * finds.
     */
    ConsoleSuspend();

    FaultPaper = FramebufferEncode(16U, 16U, 24U);
    FaultInk = FramebufferEncode(230U, 230U, 235U);
    FaultDim = FramebufferEncode(130U, 130U, 145U);
    FaultAccent = FramebufferEncode(entry->red, entry->green, entry->blue);

    GraphicsResetClip(&FaultSurface);
    GraphicsClear(&FaultSurface, FaultPaper);

    return true;
}

/* The two lines every screen ends with. */
static void FaultFooter(int32_t rows, int32_t width)
{
    /*
     * Four rows from the bottom rather than three. The sentence occupies one
     * line upon a 1280-pixel display and two upon a 640-pixel one, and at three
     * the second line was the last row of the screen — correct, and with no
     * margin at all beneath it, so that any lengthening of the sentence would
     * have pushed it off the display.
     */
    const GraphicsRectangle rule = { 0, (rows - 4) * (int32_t)FONT_HEIGHT,
                                     (int32_t)FaultSurface.width, 1 };

    GraphicsFillRectangle(&FaultSurface, rule, FaultDim);
    (void)FaultParagraph(FAULT_MARGIN, rows - 3,
                         "The machine has halted. The complete register dump and the whole "
                         "of the boot log are upon the serial port.",
                         FaultDim, width);
}

/* Every panel begins with a heading in the fault's own colour. Returns the row
 * beneath it. */
static int32_t FaultHeading(int32_t row, const char *heading)
{
    (void)FaultText(FAULT_MARGIN, row, heading, FaultAccent);

    return row + 1;
}

/*
 * The evidence panels.
 *
 * Each draws only if the entry asked for it, and each returns the row after
 * itself, so that a screen carrying two panels lays them out one beneath the
 * other without any of them knowing what else is present.
 */
static int32_t FaultPanelFaultAddress(int32_t row, uint64_t fault_address,
                                      uint64_t error_code, int32_t width)
{
    row = FaultHeading(row, "THE ADDRESS");

    (void)FaultValue(FAULT_MARGIN + 2, row, "faulting address", fault_address, 16U);
    ++row;

    row = FaultParagraph(FAULT_MARGIN + 2, row,
                         ((error_code & PAGE_FAULT_PRESENT) != 0U)
                             ? "A translation existed and the access violated its "
                               "permissions."
                             : "No translation existed for this address.",
                         FaultInk, width - 2);

    row = FaultParagraph(FAULT_MARGIN + 2, row,
                         ((error_code & PAGE_FAULT_WRITE) != 0U)
                             ? "The access was a write."
                             : "The access was a read.",
                         FaultInk, width - 2);

    if ((error_code & PAGE_FAULT_INSTRUCTION) != 0U)
    {
        row = FaultParagraph(FAULT_MARGIN + 2, row,
                             "It was an instruction fetch, so control had reached this "
                             "address.",
                             FaultInk, width - 2);
    }

    if ((error_code & PAGE_FAULT_RESERVED_BIT) != 0U)
    {
        row = FaultParagraph(FAULT_MARGIN + 2, row,
                             "A reserved bit was set in a paging-structure entry, so the "
                             "hierarchy itself is malformed.",
                             FaultInk, width - 2);
    }

    return row + 1;
}

static int32_t FaultPanelSelector(int32_t row, uint64_t error_code, int32_t width)
{
    const char *where;

    row = FaultHeading(row, "THE ERROR CODE");

    /*
     * A zero error code is not a missing one, and saying so is the point of
     * treating it separately. Intel SDM, Volume 3A, Section 6.13: a
     * general-protection fault raised by something other than a segment — a
     * privileged instruction, a non-canonical address — delivers zero, and a
     * reader who was told "selector 0" would go looking for a descriptor that
     * has nothing to do with it.
     */
    if (error_code == 0U)
    {
        return FaultParagraph(FAULT_MARGIN + 2, row,
                              "Zero. No selector is named, so the fault is about what was "
                              "attempted rather than about a descriptor.",
                              FaultInk, width - 2) +
               1;
    }

    (void)FaultValue(FAULT_MARGIN + 2, row, "error code", error_code, 8U);
    ++row;
    (void)FaultValue(FAULT_MARGIN + 2, row, "selector index",
                     (error_code >> 3U) & UINT64_C(0x1FFF), 4U);
    ++row;

    row = FaultParagraph(FAULT_MARGIN + 2, row,
                         ((error_code & UINT64_C(0x01)) != 0U)
                             ? "The fault came from outside the program: an interrupt or "
                               "an external event."
                             : "The selector was one this kernel loaded or referred to.",
                         FaultInk, width - 2);

    /* Bits 2 and 1 of the error code name the table. Values 1 and 3 both mean the
     * interrupt descriptor table; Intel SDM, Volume 3A, Figure 6-6. */
    switch ((error_code >> 1U) & UINT64_C(0x03))
    {
    case 1U:
    case 3U:
        where = "The descriptor lies in the interrupt descriptor table.";
        break;

    case 2U:
        where = "The descriptor lies in the local descriptor table, which this kernel "
                "does not use.";
        break;

    default:
        where = "The descriptor lies in the global descriptor table.";
        break;
    }

    row = FaultParagraph(FAULT_MARGIN + 2, row, where, FaultInk, width - 2);

    return row + 1;
}

static int32_t FaultPanelOpcode(int32_t row, uint64_t rip, int32_t width)
{
    row = FaultHeading(row, "THE INSTRUCTION");

    if (!FaultAddressIsReadable(rip))
    {
        return FaultParagraph(FAULT_MARGIN + 2, row,
                              "The instruction pointer names memory that is not mapped, so "
                              "no bytes can be shown. Control reached an address that holds "
                              "nothing.",
                              FaultInk, width - 2) +
               1;
    }

    {
        const uint8_t *code = (const uint8_t *)(uintptr_t)rip;
        int32_t column = FAULT_MARGIN + 2;
        char buffer[19];

        for (size_t index = 0U; index < FAULT_OPCODE_BYTES; ++index)
        {
            if (!FaultAddressIsReadable(rip + index))
            {
                break;
            }

            FaultHexadecimal(code[index], buffer, 2U);
            column = FaultText(column, row, &buffer[2], FaultInk);
            column = FaultText(column, row, " ", FaultInk);
        }
    }

    return row + 2;
}

static int32_t FaultPanelStack(int32_t row, uint64_t rsp, int32_t width)
{
    row = FaultHeading(row, "THE STACK");

    if (!FaultAddressIsReadable(rsp))
    {
        return FaultParagraph(FAULT_MARGIN + 2, row,
                              "The stack pointer names memory that is not mapped. That is "
                              "itself the fault worth pursuing: nothing can be pushed here.",
                              FaultInk, width - 2) +
               1;
    }

    {
        const uint64_t *stack = (const uint64_t *)(uintptr_t)rsp;

        for (size_t index = 0U; index < FAULT_STACK_WORDS; ++index)
        {
            const uint64_t at = rsp + (index * 8U);

            if (!FaultAddressIsReadable(at))
            {
                break;
            }

            const int32_t column = FaultValue(FAULT_MARGIN + 2, row, "at", at, 16U);

            (void)FaultValue(column, row, "holds", stack[index], 16U);
            ++row;
        }
    }

    return row + 1;
}

static int32_t FaultPanelOperands(int32_t row, const TrapFrame *frame)
{
    int32_t column;

    row = FaultHeading(row, "THE OPERANDS");

    column = FaultValue(FAULT_MARGIN + 2, row, "RAX", frame->rax, 16U);
    column = FaultValue(column, row, "RCX", frame->rcx, 16U);
    (void)FaultValue(column, row, "RDX", frame->rdx, 16U);

    return row + 2;
}

static int32_t FaultPanelControl(int32_t row)
{
    int32_t column;

    row = FaultHeading(row, "THE CONTROL REGISTERS");

    column = FaultValue(FAULT_MARGIN + 2, row, "CR0", ReadCr0(), 16U);
    (void)FaultValue(column, row, "CR3", ReadCr3(), 16U);
    ++row;
    (void)FaultValue(FAULT_MARGIN + 2, row, "CR4", ReadCr4(), 16U);

    return row + 2;
}

void FaultScreenShowException(const TrapFrame *frame, uint64_t fault_address)
{
    const FaultScreenEntry *entry;
    int32_t columns;
    int32_t rows;
    int32_t width;
    int32_t scale;
    int32_t row;
    int32_t column;

    if (frame == NULL)
    {
        return;
    }

    /*
     * A screen is drawn once. A second call means a fault was raised while the
     * first was being drawn, and drawing over the first would destroy the only
     * account of the original failure — which is the one worth keeping.
     */
    if (FaultScreenDrawn)
    {
        return;
    }

    entry = FaultScreenFor(frame->vector);

    if (!FaultScreenBegin(entry))
    {
        return;
    }

    FaultScreenDrawn = true;

    columns = (int32_t)(FaultSurface.width / FONT_WIDTH);
    rows = (int32_t)(FaultSurface.height / FONT_HEIGHT);
    width = columns - (FAULT_MARGIN * 2);

    /*
     * The title's size is computed from the display and not fixed. Three times
     * life size is legible across a room at 1280 pixels and does not fit at 640,
     * where the same title would run off the edge; the two machines this kernel
     * is tested upon are exactly those two widths.
     */
    scale = ((columns / 3) > 30) ? 3 : 2;

    row = FaultBanner(entry,
                      (entry->mnemonic[0] != '\0') ? entry->mnemonic
                                                  : InterruptVectorName(frame->vector),
                      scale);

    row = FaultParagraph(FAULT_MARGIN, row, entry->meaning, FaultInk, width) + 1;

    /* Where. Every screen carries this, the fault's own panels being what differ. */
    row = FaultHeading(row, "WHERE");
    column = FaultValue(FAULT_MARGIN + 2, row, "RIP", frame->rip, 16U);
    column = FaultValue(column, row, "CS", frame->cs, 4U);
    (void)FaultValue(column, row, "RFLAGS", frame->rflags, 8U);
    ++row;
    column = FaultValue(FAULT_MARGIN + 2, row, "RSP", frame->rsp, 16U);
    (void)FaultValue(column, row, "SS", frame->ss, 4U);
    row += 2;

    if ((entry->evidence & FAULT_EVIDENCE_FAULT_ADDRESS) != 0U)
    {
        row = FaultPanelFaultAddress(row, fault_address, frame->error_code, width);
    }

    if ((entry->evidence & FAULT_EVIDENCE_SELECTOR) != 0U)
    {
        row = FaultPanelSelector(row, frame->error_code, width);
    }

    if ((entry->evidence & FAULT_EVIDENCE_OPERANDS) != 0U)
    {
        row = FaultPanelOperands(row, frame);
    }

    if ((entry->evidence & FAULT_EVIDENCE_OPCODE) != 0U)
    {
        row = FaultPanelOpcode(row, frame->rip, width);
    }

    if ((entry->evidence & FAULT_EVIDENCE_STACK) != 0U)
    {
        row = FaultPanelStack(row, frame->rsp, width);
    }

    if ((entry->evidence & FAULT_EVIDENCE_CONTROL) != 0U)
    {
        row = FaultPanelControl(row);
    }

    row = FaultHeading(row, "WHAT TO LOOK AT FIRST");
    (void)FaultParagraph(FAULT_MARGIN + 2, row, entry->examine, FaultInk, width - 2);

    FaultFooter(rows, width);
}

void FaultScreenDemonstrate(uint64_t vector)
{
    TrapFrame frame;

    if (vector == FAULT_SCREEN_SOFTWARE)
    {
        FaultScreenShowPanic("This is a demonstration. No check actually failed.");
        return;
    }

    /*
     * A frame of values that could not have come from a machine.
     *
     * They are chosen to be unmistakable — repeated nibbles, and an obviously
     * artificial address — so that a photograph of this page cannot be filed as
     * evidence of a fault that occurred. Every field is set: a frame left partly
     * uninitialised would put whatever the stack held into a panel, and the one
     * page in this kernel that must not mislead is the page a person reads when
     * they are already confused.
     */
    frame.r15 = UINT64_C(0x0F0F0F0F0F0F0F0F);
    frame.r14 = UINT64_C(0x0E0E0E0E0E0E0E0E);
    frame.r13 = UINT64_C(0x0D0D0D0D0D0D0D0D);
    frame.r12 = UINT64_C(0x0C0C0C0C0C0C0C0C);
    frame.r11 = UINT64_C(0x0B0B0B0B0B0B0B0B);
    frame.r10 = UINT64_C(0x0A0A0A0A0A0A0A0A);
    frame.r9 = UINT64_C(0x0909090909090909);
    frame.r8 = UINT64_C(0x0808080808080808);
    frame.rbp = UINT64_C(0x0BBBBBBBBBBBBBB0);
    frame.rdi = UINT64_C(0x0DDDDDDDDDDDDDD0);
    frame.rsi = UINT64_C(0x0555555555555550);
    frame.rdx = UINT64_C(0x0000000000000000);
    frame.rcx = UINT64_C(0x0000000000000007);
    frame.rbx = UINT64_C(0x0BBBBBBBBBBBBBBB);
    frame.rax = UINT64_C(0x00000000DEADBEEF);
    frame.vector = vector;

    /*
     * An error code that decodes into something a reader can check against the
     * panel: selector index 8, in the global descriptor table, raised from
     * within the program.
     */
    frame.error_code = UINT64_C(0x0043);

    /* The instruction pointer names this routine, so that the instruction panel
     * has real bytes to reproduce and the reader can see that it read them. */
    frame.rip = (uint64_t)(uintptr_t)&FaultScreenDemonstrate;
    frame.cs = UINT64_C(0x08);
    frame.rflags = UINT64_C(0x0000000000000246);
    frame.rsp = (uint64_t)(uintptr_t)&frame;
    frame.ss = UINT64_C(0x10);

    FaultScreenShowException(&frame, UINT64_C(0x0000DEAD0000BEEF));
}

void FaultScreenShowPanic(const char *message)
{
    const FaultScreenEntry *entry = FaultScreenFor(FAULT_SCREEN_SOFTWARE);
    int32_t columns;
    int32_t rows;
    int32_t width;
    int32_t scale;
    int32_t row;

    if (FaultScreenDrawn)
    {
        return;
    }

    if (!FaultScreenBegin(entry))
    {
        return;
    }

    FaultScreenDrawn = true;

    columns = (int32_t)(FaultSurface.width / FONT_WIDTH);
    rows = (int32_t)(FaultSurface.height / FONT_HEIGHT);
    width = columns - (FAULT_MARGIN * 2);
    scale = ((columns / 3) > 30) ? 3 : 2;

    row = FaultBanner(entry, entry->mnemonic, scale);
    row = FaultParagraph(FAULT_MARGIN, row, entry->meaning, FaultInk, width) + 1;

    row = FaultHeading(row, "THE CHECK THAT FAILED");
    row = FaultParagraph(FAULT_MARGIN + 2, row,
                         (message != NULL) ? message : "(no message was given)", FaultInk,
                         width - 2) +
          1;

    row = FaultHeading(row, "WHAT TO LOOK AT FIRST");
    (void)FaultParagraph(FAULT_MARGIN + 2, row, entry->examine, FaultInk, width - 2);

    FaultFooter(rows, width);
}
