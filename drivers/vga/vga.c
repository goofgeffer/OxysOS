/*
 * File: drivers/vga/vga.c
 * Purpose: Implements the VGA text-mode display driver: the character output
 *          and its control characters, the scrolling, the colour attributes,
 *          the hardware cursor, and the erase limit that lets a backspace cross
 *          from one row to the row above without destroying what it did not
 *          write.
 * Key functions: VgaInitialise, VgaSetColour, VgaSetBlinkEnabled, VgaClear,
 *          VgaPutCharacter, VgaWriteString, VgaCursorPosition,
 *          VgaSetCursorPosition, VgaHardwareCursorPosition, VgaSetCursorVisible,
 *          VgaSetCursorShape, VgaCharacterAt, VgaScroll, VgaSetEraseLimit,
 *          VgaReport.
 * References:
 *   - IBM Video Graphics Array technical reference: mode 3 provides an 80 by 25
 *     character display whose frame buffer begins at physical address 0x000B8000
 *     and whose cells comprise a code-point byte followed by an attribute byte.
 *   - IBM VGA technical reference, Miscellaneous Output Register (written at
 *     0x03C2 and read at 0x03CC): "If set Color Emulation. Base Address=3Dxh
 *     else Mono Emulation. Base Address=3Bxh". The register pair and the input
 *     status register move with it, and so does the frame buffer.
 *   - IBM VGA technical reference, CRT Controller Registers, addressed through
 *     an index port and a data port: the Cursor Start Register (index 0x0A),
 *     whose bits 0 to 4 are the first scan line of the cursor within the
 *     character cell and whose bit 5 turns the cursor off if set; the Cursor End
 *     Register (index 0x0B), whose bits 0 to 4 are the last scan line and whose
 *     bits 5 and 6 are the cursor skew, a delay expressed in character clocks;
 *     and the Cursor Location High and Low Registers (indices 0x0E and 0x0F),
 *     holding the upper and lower eight bits of the cursor address.
 *   - IBM VGA technical reference, Attribute Controller Registers: the address
 *     and data registers share port 0x03C0, which is written for either purpose
 *     and read for the address, the data being read at 0x03C1; an internal
 *     flip-flop selects between the two and is returned to the address by a read
 *     of the Input Status #1 Register at 0x03DA. Bit 7 of the address register
 *     is the Palette Address Source, which must be set for normal operation and
 *     cleared only to load the internal palette. In the Attribute Mode Control
 *     Register (index 0x10), bit 3 set makes attribute bit 7 blink and clear
 *     makes it select a bright background.
 *   - Intel 64 and IA-32 Architectures Software Developer's Manual, Volume 3A,
 *     Section 4.5: the frame buffer is reachable at PhysicalToVirtual(0xB8000)
 *     because the boot-time paging hierarchy maps the first gibibyte of physical
 *     memory into the higher half.
 *   - ANSI X3.4-1986 (ISO/IEC 646), the control characters: BS (0x08) moves the
 *     active position one character position backward, LF (0x0A) moves it one
 *     line down and CR (0x0D) moves it to the first position of the line. None of
 *     the three erases anything.
 */

#include <oxys/vga.h>
#include <oxys/kernel.h>
#include <oxys/io.h>

/*
 * The index and data ports of the CRT controller in each of the two
 * configurations, together with the input status register that returns the
 * attribute controller's flip-flop to its address state.
 */
#define VGA_CRTC_INDEX_COLOUR     UINT16_C(0x03D4)
#define VGA_CRTC_INDEX_MONOCHROME UINT16_C(0x03B4)
#define VGA_INPUT_STATUS_COLOUR     UINT16_C(0x03DA)
#define VGA_INPUT_STATUS_MONOCHROME UINT16_C(0x03BA)

/* The Miscellaneous Output Register is written at one port and read at another. */
#define VGA_MISCELLANEOUS_OUTPUT_READ UINT16_C(0x03CC)

/* Bit 0 of that register: set for colour emulation, clear for monochrome. */
#define VGA_MISCELLANEOUS_IO_ADDRESS_SELECT UINT8_C(0x01)

/* The CRT controller registers this driver uses. */
#define VGA_CRTC_CURSOR_START         UINT8_C(0x0A)
#define VGA_CRTC_CURSOR_END           UINT8_C(0x0B)
#define VGA_CRTC_CURSOR_LOCATION_HIGH UINT8_C(0x0E)
#define VGA_CRTC_CURSOR_LOCATION_LOW  UINT8_C(0x0F)

/* Bit 5 of the Cursor Start Register turns the cursor off if set. */
#define VGA_CURSOR_DISABLE UINT8_C(0x20)

/* Bits 0 to 4 of both cursor registers select a scan line of the character cell. */
#define VGA_CURSOR_SCAN_LINE_MASK UINT8_C(0x1F)

/* The greatest scan line a character cell may extend to. */
#define VGA_MAXIMUM_SCAN_LINE UINT8_C(31)

/* The attribute controller: its shared address and data port, and its read port. */
#define VGA_ATTRIBUTE_PORT      UINT16_C(0x03C0)
#define VGA_ATTRIBUTE_READ_PORT UINT16_C(0x03C1)

/*
 * Bit 7 of the attribute address register, the Palette Address Source. It is set
 * for normal operation; writing an index with it clear disconnects the internal
 * palette and blanks the display.
 */
#define VGA_ATTRIBUTE_PALETTE_ADDRESS_SOURCE UINT8_C(0x20)

/* The Attribute Mode Control Register, and its blink enable bit. */
#define VGA_ATTRIBUTE_MODE_CONTROL UINT8_C(0x10)
#define VGA_ATTRIBUTE_BLINK_ENABLE UINT8_C(0x08)

/* The number of columns a horizontal tabulation advances to a multiple of. */
#define VGA_TABULATION_WIDTH 8U

/*
 * A pointer to the character frame buffer. It is declared volatile because the
 * memory is examined by the display hardware independently of the processor, and
 * the compiler must therefore not elide or reorder stores to it.
 *
 * The translation is expressed as explicit arithmetic upon the two macro
 * constants rather than as a call to PhysicalToVirtual, because ISO/IEC
 * 9899:2011, Section 6.7.9, paragraph 4, requires the initialiser of an object
 * of static storage duration to be a constant expression, which a function call
 * is not, irrespective of the function being declared inline.
 *
 * It is initialised to the colour buffer and corrected by VgaInitialise should
 * the adapter prove to be in its monochrome configuration, so that a write
 * arriving before initialisation lands upon the buffer of the overwhelmingly
 * more likely arrangement rather than upon a null pointer.
 */
static volatile uint16_t *VgaBuffer =
    (volatile uint16_t *)(uintptr_t)(VGA_TEXT_BUFFER_PHYSICAL + KERNEL_VIRTUAL_BASE);

/* The register ports in use, determined at initialisation. */
static uint16_t VgaCrtcIndex = VGA_CRTC_INDEX_COLOUR;
static uint16_t VgaCrtcData = VGA_CRTC_INDEX_COLOUR + 1U;
static uint16_t VgaInputStatus = VGA_INPUT_STATUS_COLOUR;
static bool VgaColourConfiguration = true;

/* The column at which the next character will be written. */
static size_t VgaCursorColumn;

/* The row at which the next character will be written. */
static size_t VgaCursorRow;

/*
 * The position before which a backspace will not retreat. See VgaSetEraseLimit:
 * it is the whole of the driver's knowledge of which characters upon the display
 * belong to the reader of input and which belong to the kernel.
 */
static size_t VgaEraseLimitRow;
static size_t VgaEraseLimitColumn;

/*
 * The attribute byte applied to characters written subsequently, held in the
 * upper half of a cell value so that it may be combined with a code point by a
 * single bitwise disjunction.
 */
static uint16_t VgaCurrentAttribute;

/* True while bit 7 of the attribute byte selects blinking rather than brightness. */
static bool VgaBlinkSelected = true;

/* Accounting. */
static uint64_t VgaWritten;
static uint64_t VgaScrolls;

/* Writes a CRT controller register through the index and data port pair. */
static void VgaWriteCrtc(uint8_t index, uint8_t value)
{
    PortWriteByte(VgaCrtcIndex, index);
    PortWriteByte(VgaCrtcData, value);
}

/* Reads a CRT controller register through the index and data port pair. */
static uint8_t VgaReadCrtc(uint8_t index)
{
    PortWriteByte(VgaCrtcIndex, index);
    return PortReadByte(VgaCrtcData);
}

/*
 * Reads an attribute controller register. The flip-flop that selects between the
 * address and the data register is returned to the address by a read of the
 * input status register, whose value is of no interest; the index is then
 * written with the palette address source set, without which the display would
 * blank.
 */
static uint8_t VgaReadAttribute(uint8_t index)
{
    (void)PortReadByte(VgaInputStatus);
    PortWriteByte(VGA_ATTRIBUTE_PORT, (uint8_t)(index | VGA_ATTRIBUTE_PALETTE_ADDRESS_SOURCE));
    return PortReadByte(VGA_ATTRIBUTE_READ_PORT);
}

/*
 * Writes an attribute controller register, leaving the flip-flop at the address
 * and the palette address source set, which is the state normal operation
 * requires.
 */
static void VgaWriteAttribute(uint8_t index, uint8_t value)
{
    (void)PortReadByte(VgaInputStatus);
    PortWriteByte(VGA_ATTRIBUTE_PORT, (uint8_t)(index | VGA_ATTRIBUTE_PALETTE_ADDRESS_SOURCE));
    PortWriteByte(VGA_ATTRIBUTE_PORT, value);

    (void)PortReadByte(VgaInputStatus);
    PortWriteByte(VGA_ATTRIBUTE_PORT, VGA_ATTRIBUTE_PALETTE_ADDRESS_SOURCE);
}

/* The offset of a cell within the frame buffer. */
static size_t VgaOffset(size_t row, size_t column)
{
    return (row * (size_t)VGA_WIDTH) + column;
}

/*
 * Composes an attribute byte from a foreground and a background colour index.
 * The foreground occupies bits 0 to 3 and the background bits 4 to 7.
 */
static uint8_t VgaComposeAttribute(VgaColour foreground, VgaColour background)
{
    return (uint8_t)((uint8_t)foreground | ((uint8_t)background << 4));
}

/*
 * Composes a complete cell value from a code point and an attribute byte.
 */
static uint16_t VgaComposeCell(char character, uint8_t attribute)
{
    return (uint16_t)((uint16_t)(unsigned char)character | ((uint16_t)attribute << 8));
}

/*
 * Writes the current cursor position to the CRT controller so that the hardware
 * cursor is displayed at the position at which the next character will appear.
 */
static void VgaUpdateHardwareCursor(void)
{
    const uint16_t position = (uint16_t)VgaOffset(VgaCursorRow, VgaCursorColumn);

    VgaWriteCrtc(VGA_CRTC_CURSOR_LOCATION_LOW, (uint8_t)(position & 0x00FFU));
    VgaWriteCrtc(VGA_CRTC_CURSOR_LOCATION_HIGH, (uint8_t)((position >> 8) & 0x00FFU));
}

void VgaScroll(void)
{
    const uint16_t blank_cell = VgaComposeCell(' ', (uint8_t)(VgaCurrentAttribute >> 8));

    for (size_t row = 1U; row < VGA_HEIGHT; ++row)
    {
        for (size_t column = 0U; column < VGA_WIDTH; ++column)
        {
            VgaBuffer[VgaOffset(row - 1U, column)] = VgaBuffer[VgaOffset(row, column)];
        }
    }

    for (size_t column = 0U; column < VGA_WIDTH; ++column)
    {
        VgaBuffer[VgaOffset(VGA_HEIGHT - 1U, column)] = blank_cell;
    }

    /*
     * The erase limit names a position upon the display, and the display has
     * moved beneath it. Where the limit stood upon the first row the text it
     * protected has left the display altogether, and the limit collapses to the
     * origin; nothing that remains was written before it.
     */
    if (VgaEraseLimitRow > 0U)
    {
        --VgaEraseLimitRow;
    }
    else
    {
        VgaEraseLimitColumn = 0U;
    }

    ++VgaScrolls;
}

/*
 * Advances the cursor to the beginning of the following row, scrolling the
 * display if the cursor would otherwise pass the final row.
 */
static void VgaAdvanceRow(void)
{
    VgaCursorColumn = 0U;

    if (VgaCursorRow + 1U < VGA_HEIGHT)
    {
        ++VgaCursorRow;
    }
    else
    {
        VgaScroll();
    }
}

/*
 * The column of the last character standing upon a row, or zero if the row is
 * blank. It is where the cursor belongs after a backspace has crossed into the
 * row from the row below: the position preceding the first column of a row is
 * the end of the text above it, not the eightieth column of blanks that was
 * never written to.
 */
static size_t VgaLastOccupiedColumn(size_t row)
{
    size_t column = VGA_WIDTH;

    while (column > 0U)
    {
        --column;

        if ((char)(VgaBuffer[VgaOffset(row, column)] & 0x00FFU) != ' ')
        {
            return column;
        }
    }

    return 0U;
}

/*
 * Retreats the cursor by one position, crossing into the row above where it
 * stands in the first column.
 *
 * The backspace does not erase: ANSI X3.4-1986 defines it as a movement of the
 * active position one character position backward and no more, and a caller that
 * means to erase writes the sequence "\b \b", which erases upon a serial
 * terminal equally.
 *
 * The movement stops at the erase limit. Without such a limit the driver has no
 * way to tell a character the user typed from a character the kernel printed,
 * and a backspace crossing a row boundary would consume the boot log a character
 * at a time. The limit supplies exactly the knowledge that was missing, and is
 * set by whoever reads the input, who alone knows where the input began.
 */
static void VgaBackspace(void)
{
    const size_t limit = VgaOffset(VgaEraseLimitRow, VgaEraseLimitColumn);

    if (VgaOffset(VgaCursorRow, VgaCursorColumn) <= limit)
    {
        return;
    }

    if (VgaCursorColumn > 0U)
    {
        --VgaCursorColumn;
    }
    else
    {
        --VgaCursorRow;
        VgaCursorColumn = VgaLastOccupiedColumn(VgaCursorRow);
    }

    /*
     * A row above whose text ends before the limit would otherwise have carried
     * the cursor past it, the limit standing in the middle of a row that a
     * prompt shares with the input that follows it.
     */
    if (VgaOffset(VgaCursorRow, VgaCursorColumn) < limit)
    {
        VgaCursorRow = VgaEraseLimitRow;
        VgaCursorColumn = VgaEraseLimitColumn;
    }
}

void VgaSetColour(VgaColour foreground, VgaColour background)
{
    VgaCurrentAttribute = (uint16_t)((uint16_t)VgaComposeAttribute(foreground, background) << 8);
}

uint8_t VgaAttribute(void)
{
    return (uint8_t)(VgaCurrentAttribute >> 8);
}

bool VgaSetBlinkEnabled(bool enabled)
{
    const uint8_t original = VgaReadAttribute(VGA_ATTRIBUTE_MODE_CONTROL);
    const uint8_t desired = enabled ? (uint8_t)(original | VGA_ATTRIBUTE_BLINK_ENABLE)
                                    : (uint8_t)(original & (uint8_t)~VGA_ATTRIBUTE_BLINK_ENABLE);

    VgaWriteAttribute(VGA_ATTRIBUTE_MODE_CONTROL, desired);

    /*
     * The register is read back because the attribute controller is reached
     * through a flip-flop whose state is not otherwise observable: a write that
     * arrived while the flip-flop stood at the data register would have altered
     * some other register entirely, and the only symptom would be a display that
     * looked wrong to a person.
     */
    if (VgaReadAttribute(VGA_ATTRIBUTE_MODE_CONTROL) != desired)
    {
        VgaWriteAttribute(VGA_ATTRIBUTE_MODE_CONTROL, original);
        return false;
    }

    VgaBlinkSelected = enabled;
    return true;
}

bool VgaBlinkEnabled(void)
{
    return VgaBlinkSelected;
}

void VgaClear(void)
{
    const uint16_t blank_cell = VgaComposeCell(' ', (uint8_t)(VgaCurrentAttribute >> 8));

    for (size_t index = 0U; index < (size_t)VGA_WIDTH * (size_t)VGA_HEIGHT; ++index)
    {
        VgaBuffer[index] = blank_cell;
    }

    VgaCursorColumn = 0U;
    VgaCursorRow = 0U;
    VgaEraseLimitRow = 0U;
    VgaEraseLimitColumn = 0U;
    VgaUpdateHardwareCursor();
}

void VgaInitialise(void)
{
    const uint8_t miscellaneous = PortReadByte(VGA_MISCELLANEOUS_OUTPUT_READ);

    /*
     * Which ports the adapter answers upon is read rather than assumed. The
     * monochrome configuration is not expected upon any machine this kernel will
     * run upon, but establishing it costs one input instruction, and a driver
     * that assumed it would write the cursor location into the void.
     */
    VgaColourConfiguration = (miscellaneous & VGA_MISCELLANEOUS_IO_ADDRESS_SELECT) != 0U;

    if (VgaColourConfiguration)
    {
        VgaCrtcIndex = VGA_CRTC_INDEX_COLOUR;
        VgaInputStatus = VGA_INPUT_STATUS_COLOUR;
        VgaBuffer =
            (volatile uint16_t *)(uintptr_t)(VGA_TEXT_BUFFER_PHYSICAL + KERNEL_VIRTUAL_BASE);
    }
    else
    {
        VgaCrtcIndex = VGA_CRTC_INDEX_MONOCHROME;
        VgaInputStatus = VGA_INPUT_STATUS_MONOCHROME;
        VgaBuffer = (volatile uint16_t *)(uintptr_t)(VGA_TEXT_BUFFER_MONOCHROME_PHYSICAL +
                                                     KERNEL_VIRTUAL_BASE);
    }

    VgaCrtcData = (uint16_t)(VgaCrtcIndex + 1U);

    VgaWritten = 0U;
    VgaScrolls = 0U;

    /*
     * The shape of the cursor is left as the firmware established it, that shape
     * being the one the machine's own display is known to present legibly; only
     * its visibility is asserted, since a cursor the firmware disabled would
     * leave the operator without any indication of where output stands.
     */
    VgaSetCursorVisible(true);

    VgaSetColour(VGA_COLOUR_LIGHT_GREY, VGA_COLOUR_BLACK);

    /*
     * Blinking is disabled so that bit 7 of the attribute brightens the
     * background instead. The failure of that write is not treated as an error:
     * the consequence is merely that the eight bright backgrounds blink instead
     * of being bright, and a display driver that refused to start over it would
     * deprive the machine of its console to no purpose.
     */
    (void)VgaSetBlinkEnabled(false);

    VgaClear();
}

bool VgaIsColourAdapter(void)
{
    return VgaColourConfiguration;
}

uint16_t VgaCrtcIndexPort(void)
{
    return VgaCrtcIndex;
}

void VgaPutCharacter(char character)
{
    switch (character)
    {
    case '\n':
        VgaAdvanceRow();
        break;

    case '\r':
        VgaCursorColumn = 0U;
        break;

    case '\b':
        VgaBackspace();
        break;

    case '\t':
        /* A horizontal tabulation advances to the next multiple of eight columns. */
        do
        {
            VgaPutCharacter(' ');
        } while ((VgaCursorColumn % VGA_TABULATION_WIDTH) != 0U);
        break;

    default:
        VgaBuffer[VgaOffset(VgaCursorRow, VgaCursorColumn)] =
            VgaComposeCell(character, (uint8_t)(VgaCurrentAttribute >> 8));

        ++VgaWritten;
        ++VgaCursorColumn;
        if (VgaCursorColumn >= VGA_WIDTH)
        {
            VgaAdvanceRow();
        }
        break;
    }

    VgaUpdateHardwareCursor();
}

void VgaCursorPosition(size_t *row, size_t *column)
{
    if (row != NULL)
    {
        *row = VgaCursorRow;
    }

    if (column != NULL)
    {
        *column = VgaCursorColumn;
    }
}

bool VgaSetCursorPosition(size_t row, size_t column)
{
    if ((row >= VGA_HEIGHT) || (column >= VGA_WIDTH))
    {
        return false;
    }

    VgaCursorRow = row;
    VgaCursorColumn = column;
    VgaUpdateHardwareCursor();
    return true;
}

bool VgaHardwareCursorPosition(size_t *row, size_t *column)
{
    const uint8_t low = VgaReadCrtc(VGA_CRTC_CURSOR_LOCATION_LOW);
    const uint8_t high = VgaReadCrtc(VGA_CRTC_CURSOR_LOCATION_HIGH);
    const size_t position = ((size_t)high << 8) | (size_t)low;

    if (row != NULL)
    {
        *row = position / (size_t)VGA_WIDTH;
    }

    if (column != NULL)
    {
        *column = position % (size_t)VGA_WIDTH;
    }

    return VgaCursorVisible();
}

void VgaSetCursorVisible(bool visible)
{
    const uint8_t start = VgaReadCrtc(VGA_CRTC_CURSOR_START);

    VgaWriteCrtc(VGA_CRTC_CURSOR_START,
                 visible ? (uint8_t)(start & (uint8_t)~VGA_CURSOR_DISABLE)
                         : (uint8_t)(start | VGA_CURSOR_DISABLE));
}

bool VgaCursorVisible(void)
{
    return (VgaReadCrtc(VGA_CRTC_CURSOR_START) & VGA_CURSOR_DISABLE) == 0U;
}

bool VgaSetCursorShape(uint8_t first_scan_line, uint8_t last_scan_line)
{
    uint8_t start;
    uint8_t end;

    if ((first_scan_line > VGA_MAXIMUM_SCAN_LINE) || (last_scan_line > VGA_MAXIMUM_SCAN_LINE) ||
        (first_scan_line > last_scan_line))
    {
        return false;
    }

    /*
     * Only the scan line fields are replaced. Bit 5 of the start register is the
     * cursor disable, which the shape has no business altering, and bits 5 and 6
     * of the end register are the cursor skew, which is a property of the
     * adapter's timing and not of the shape asked for.
     */
    start = (uint8_t)((VgaReadCrtc(VGA_CRTC_CURSOR_START) & (uint8_t)~VGA_CURSOR_SCAN_LINE_MASK) |
                      first_scan_line);
    end = (uint8_t)((VgaReadCrtc(VGA_CRTC_CURSOR_END) & (uint8_t)~VGA_CURSOR_SCAN_LINE_MASK) |
                    last_scan_line);

    VgaWriteCrtc(VGA_CRTC_CURSOR_START, start);
    VgaWriteCrtc(VGA_CRTC_CURSOR_END, end);
    return true;
}

char VgaCharacterAt(size_t row, size_t column)
{
    if ((row >= VGA_HEIGHT) || (column >= VGA_WIDTH))
    {
        return ' ';
    }

    return (char)(VgaBuffer[VgaOffset(row, column)] & 0x00FFU);
}

void VgaSetEraseLimit(void)
{
    VgaEraseLimitRow = VgaCursorRow;
    VgaEraseLimitColumn = VgaCursorColumn;
}

void VgaEraseLimit(size_t *row, size_t *column)
{
    if (row != NULL)
    {
        *row = VgaEraseLimitRow;
    }

    if (column != NULL)
    {
        *column = VgaEraseLimitColumn;
    }
}

uint64_t VgaCharactersWritten(void)
{
    return VgaWritten;
}

uint64_t VgaScrollCount(void)
{
    return VgaScrolls;
}

void VgaWriteString(const char *string)
{
    if (string == NULL)
    {
        return;
    }

    for (size_t index = 0U; string[index] != '\0'; ++index)
    {
        VgaPutCharacter(string[index]);
    }
}

void VgaReport(void)
{
    KernelWriteString("Display adapter: ");
    KernelWriteString(VgaColourConfiguration ? "colour" : "monochrome");
    KernelWriteString(" configuration, registers at ");
    KernelWriteHexadecimal((uint64_t)VgaCrtcIndex);
    KernelWriteString(", 80 by 25 characters.\n");

    KernelWriteString("Display adapter: cursor ");
    KernelWriteString(VgaCursorVisible() ? "displayed" : "hidden");
    KernelWriteString(", attribute bit 7 selects ");
    KernelWriteString(VgaBlinkSelected ? "blinking" : "a bright background");
    KernelWriteString(".\n");

    KernelWriteString("Display adapter: written ");
    KernelWriteDecimal(VgaWritten);
    KernelWriteString(", scrolled ");
    KernelWriteDecimal(VgaScrolls);
    KernelWriteString(", cursor at row ");
    KernelWriteDecimal((uint64_t)VgaCursorRow);
    KernelWriteString(", column ");
    KernelWriteDecimal((uint64_t)VgaCursorColumn);
    KernelWriteString(".\n");
}
