/*
 * File: kernel/include/oxys/vga.h
 * Purpose: Declares the interface of the VGA text-mode display driver: the
 *          colour attributes, the cursor control, the scrolling, the cell
 *          accessors that make the display readable by the machine and not only
 *          by a person, and the erase limit that governs how far a backspace may
 *          retreat.
 * Key definitions: VgaColour, VgaInitialise, VgaSetColour, VgaSetBlinkEnabled,
 *          VgaClear, VgaPutCharacter, VgaWriteString, VgaCursorPosition,
 *          VgaSetCursorPosition, VgaHardwareCursorPosition, VgaSetCursorVisible,
 *          VgaSetCursorShape, VgaCharacterAt, VgaScroll, VgaSetEraseLimit,
 *          VgaEraseLimit, VgaReport.
 * References:
 *   - IBM Video Graphics Array technical reference: mode 3 presents an 80 by 25
 *     character display whose frame buffer begins at physical address 0x000B8000
 *     when the adapter is in its colour configuration, and at 0x000B0000 when it
 *     is in its monochrome one.
 *   - Each character cell occupies two bytes: the code point in the first byte
 *     and the attribute in the second, the attribute comprising a four-bit
 *     foreground index in bits 0 to 3 and a background index in bits 4 to 6,
 *     with bit 7 selecting either blinking or a bright background according to
 *     bit 3 of the Attribute Mode Control Register.
 *   - IBM VGA technical reference, Miscellaneous Output Register: bit 0 selects
 *     colour emulation, whose registers are addressed at 0x03Dx, or monochrome
 *     emulation, whose registers are addressed at 0x03Bx.
 *   - IBM VGA technical reference, CRT Controller Registers: the Cursor Start
 *     Register (index 0x0A), the Cursor End Register (index 0x0B) and the Cursor
 *     Location High and Low Registers (indices 0x0E and 0x0F).
 *   - ANSI X3.4-1986 (ISO/IEC 646), the control characters: the treatment of the
 *     backspace, the line feed, the carriage return and the horizontal
 *     tabulation by VgaPutCharacter.
 */

#ifndef OXYS_VGA_H
#define OXYS_VGA_H

#include <oxys/types.h>

/* The dimensions of the standard VGA colour text mode (mode 3). */
#define VGA_WIDTH  80
#define VGA_HEIGHT 25

/*
 * The physical addresses of the text-mode frame buffer in the two configurations
 * the adapter may be found in. Which of them is in use is determined at
 * initialisation and reported by VgaIsColourAdapter.
 */
#define VGA_TEXT_BUFFER_PHYSICAL            UINT64_C(0x000B8000)
#define VGA_TEXT_BUFFER_MONOCHROME_PHYSICAL UINT64_C(0x000B0000)

/*
 * The sixteen colour indices of the standard VGA text-mode palette. All sixteen
 * are valid as foreground colours. Indices 8 to 15 are valid as background
 * colours only while blinking is disabled, bit 7 of the attribute serving either
 * purpose but not both; see VgaSetBlinkEnabled.
 */
typedef enum VgaColour
{
    VGA_COLOUR_BLACK          = 0,
    VGA_COLOUR_BLUE           = 1,
    VGA_COLOUR_GREEN          = 2,
    VGA_COLOUR_CYAN           = 3,
    VGA_COLOUR_RED            = 4,
    VGA_COLOUR_MAGENTA        = 5,
    VGA_COLOUR_BROWN          = 6,
    VGA_COLOUR_LIGHT_GREY     = 7,
    VGA_COLOUR_DARK_GREY      = 8,
    VGA_COLOUR_LIGHT_BLUE     = 9,
    VGA_COLOUR_LIGHT_GREEN    = 10,
    VGA_COLOUR_LIGHT_CYAN     = 11,
    VGA_COLOUR_LIGHT_RED      = 12,
    VGA_COLOUR_LIGHT_MAGENTA  = 13,
    VGA_COLOUR_YELLOW         = 14,
    VGA_COLOUR_WHITE          = 15
} VgaColour;

/*
 * Prepares the driver for use. The configuration of the adapter is read rather
 * than assumed: the Miscellaneous Output Register determines whether the
 * registers and the frame buffer are addressed for colour or for monochrome
 * operation, and the shape of the cursor is taken as the firmware left it. The
 * default colour pair is selected, blinking is disabled so that the whole of the
 * palette is available as a background, and the display is cleared.
 *
 * This must be called before any other function declared here.
 */
void VgaInitialise(void);

/* True if the adapter answered in its colour configuration, as it will upon any
 * machine of interest; the monochrome configuration is supported because the
 * cost of reading one register is lower than the cost of assuming. */
bool VgaIsColourAdapter(void);

/* The CRT controller index port in use, 0x03D4 or 0x03B4. */
uint16_t VgaCrtcIndexPort(void);

/*
 * Selects the foreground and background colours applied to characters written
 * subsequently.
 */
void VgaSetColour(VgaColour foreground, VgaColour background);

/* The attribute byte presently applied to characters written. */
uint8_t VgaAttribute(void);

/*
 * Determines what bit 7 of the attribute byte means. While blinking is enabled
 * that bit makes the character blink and the background is limited to the first
 * eight colours; while it is disabled the bit brightens the background instead,
 * and all sixteen colours are available. The driver disables it, a kernel having
 * no use for blinking text and every use for the sixteenth background colour.
 *
 * Returns false if the Attribute Mode Control Register did not read back with
 * the value written, in which case nothing has been changed.
 */
bool VgaSetBlinkEnabled(bool enabled);

/* True while bit 7 of the attribute byte selects blinking. */
bool VgaBlinkEnabled(void);

/*
 * Fills the entire display with space characters in the current colour pair and
 * returns the cursor and the erase limit to the origin.
 */
void VgaClear(void);

/*
 * Writes a single character at the cursor position and advances the cursor.
 *
 * The line feed advances to the beginning of the following row; the carriage
 * return returns to the beginning of the current row; the horizontal tabulation
 * advances to the next multiple of eight columns.
 *
 * The backspace moves the cursor one position backward without erasing, as ANSI
 * X3.4-1986 defines it; a caller that means to erase writes "\b \b". In the
 * first column it crosses into the row above, to the position immediately after
 * the text standing there — not to the eightieth column of blanks that was never
 * written to, and not onto the last character, which the backspace has not
 * reached and must not consume. A row that is entirely occupied is the
 * exception: it ended by wrapping rather than by a line feed, so there is no
 * separator to consume and the cursor stops upon its final character. The
 * movement will not pass the erase limit; see VgaSetEraseLimit.
 *
 * The display is scrolled upward by one row when the cursor passes the final row.
 */
void VgaPutCharacter(char character);

/*
 * Writes a null-terminated string by repeated application of VgaPutCharacter.
 */
void VgaWriteString(const char *string);

/*
 * Reports the position at which the next character will be written. Either
 * pointer may be null, in which case that component is not reported. It exists
 * so that the boot-time self-test may assert the cursor movements above, which
 * are otherwise observable only by a person looking at the display.
 */
void VgaCursorPosition(size_t *row, size_t *column);

/*
 * Places the cursor at the stated position. Returns false, having changed
 * nothing, if the position lies outside the display.
 */
bool VgaSetCursorPosition(size_t row, size_t column);

/*
 * Reports the position the CRT controller itself holds, read back from the
 * Cursor Location registers rather than from the driver's own record of it.
 * Either pointer may be null. Returns false if the cursor is disabled, in which
 * case the location registers describe a cursor that is not displayed.
 *
 * The two positions agreeing is the only evidence available that the driver is
 * addressing the controller correctly: a position written to the wrong register
 * index leaves the cursor stationary, which the machine cannot otherwise notice.
 */
bool VgaHardwareCursorPosition(size_t *row, size_t *column);

/* Displays or hides the hardware cursor, preserving its shape either way. */
void VgaSetCursorVisible(bool visible);

/* True while the hardware cursor is displayed. */
bool VgaCursorVisible(void);

/*
 * Selects the first and last scan lines of the character cell that the cursor
 * occupies, a cell being at most 32 scan lines high. Returns false, having
 * changed nothing, if either line lies outside that range or if the first line
 * is below the last, which would present no cursor at all.
 */
bool VgaSetCursorShape(uint8_t first_scan_line, uint8_t last_scan_line);

/*
 * The code point standing in the stated cell, or a space if the position lies
 * outside the display. The display is the only record of what has been printed,
 * so a self-test that means to assert what appears must read it back.
 */
char VgaCharacterAt(size_t row, size_t column);

/*
 * Moves the contents of the display upward by one row, discarding the first row
 * and filling the final row with spaces in the current colour pair. The cursor
 * is not moved; the erase limit is, since the text it protects has moved.
 */
void VgaScroll(void);

/*
 * Records the current cursor position as the erase limit: the position before
 * which a backspace will not retreat. It is what allows the backspace to cross
 * from one row to the row above without destroying output that the user never
 * typed, the driver otherwise having no way to tell the two apart. A reader of
 * input sets it where the input is to begin.
 */
void VgaSetEraseLimit(void);

/* Reports the erase limit. Either pointer may be null. */
void VgaEraseLimit(size_t *row, size_t *column);

/* Accounting, read by the boot-time self-test and by VgaReport. */
uint64_t VgaCharactersWritten(void);
uint64_t VgaScrollCount(void);

/* Writes a description of the adapter and its accounting to the console. */
void VgaReport(void);

#endif /* OXYS_VGA_H */
