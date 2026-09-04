/*
 * File: kernel/include/oxys/console.h
 * Purpose: Declares the graphical console: the text display drawn upon the
 *          framebuffer, which restores the operator's view of the boot that
 *          sub-task 6.2 took away.
 * Key definitions: ConsoleInitialise, ConsoleWriteCharacter, ConsoleWriteString,
 *          ConsoleIsActive, ConsoleSetColour, ConsoleColumns, ConsoleRows,
 *          ConsoleColumn, ConsoleRow, ConsoleSetEraseLimit, ConsoleSuspend,
 *          ConsoleReport.
 * References:
 *   - ANSI X3.4-1986: the four control characters implemented, and the meaning
 *     each is given.
 *   - docs/devices/DISPLAY.md, Sections 6 and 7: the same four characters as the
 *     text-mode driver implements them. This console matches that driver
 *     deliberately, including the erase limit, so that one diagnostic path does
 *     not behave differently upon two displays.
 *   - docs/design/GRAPHICS.md, Sections 19 and 20: the design and its limits.
 *
 * What this is for.
 *
 * Sub-task 6.2 asked the boot loader for a framebuffer, and a boot loader that
 * supplies one sets a graphics mode to do it. The text-mode driver then goes on
 * writing correctly to memory the adapter no longer displays, and for two
 * sub-tasks the screen has shown a test pattern and nothing else. This is the
 * console that ends that.
 *
 * It is not a replacement for the text-mode driver and does not supersede it.
 * Both are written to by the diagnostic path, and which of them the operator can
 * actually see is decided by the mode the boot loader left the machine in.
 */

#ifndef OXYS_CONSOLE_H
#define OXYS_CONSOLE_H

#include <oxys/types.h>

/*
 * The capacity of the buffer that holds what was written before the console
 * existed.
 *
 * The framebuffer cannot be acquired until the kernel virtual arena exists,
 * because the mapping comes out of it, and by then some sixteen hundred bytes of
 * the boot log have already been written. Without this the screen would begin
 * part way through the boot and the earliest messages — the ones that report the
 * handover and the memory map, which are exactly the ones worth seeing when a
 * machine will not boot — would appear upon the serial port alone.
 *
 * Four kibibytes is comfortably more than that and is fixed: it is .bss, it is
 * never grown, and what does not fit is dropped with the drop recorded. A buffer
 * that reallocated would need the heap, which does not exist that early either.
 */
#define CONSOLE_EARLY_CAPACITY 4096U

/*
 * Prepares the console upon the framebuffer and replays whatever was written
 * before it existed.
 *
 * Returns false where there is no framebuffer to draw upon, in which case
 * nothing is drawn and every later write is discarded. That is not a failure of
 * the machine: it means the boot loader left the adapter in a text mode, and the
 * driver of sub-task 4.2 is displaying the console instead.
 */
bool ConsoleInitialise(void);

/* Whether there is a framebuffer console being drawn upon. */
bool ConsoleIsActive(void);

/*
 * Writes one character, interpreting LF, CR, HT and BS as ANSI X3.4-1986
 * defines them and as docs/devices/DISPLAY.md, Section 6, describes for the
 * text-mode driver:
 *
 *   LF  moves to the first column of the following row, scrolling upon the last.
 *   CR  moves to the first column of the current row.
 *   HT  advances to the next multiple of eight columns — to a multiple, not by
 *       eight.
 *   BS  moves one position backward and does not erase, and will not retreat
 *       past the erase limit.
 *
 * Every other control character is drawn as the replacement glyph rather than
 * being silently discarded, so that a control character nothing meant to emit is
 * visible in the output rather than absent from it.
 *
 * Before the console exists the character is recorded for replay instead; see
 * CONSOLE_EARLY_CAPACITY.
 */
void ConsoleWriteCharacter(char character);

/* Writes a null-terminated string, character by character. */
void ConsoleWriteString(const char *string);

/*
 * The colours subsequent characters are drawn in, as encoded pixels of the
 * framebuffer's layout — FramebufferEncode produces them.
 *
 * The background is used to fill a cell before its glyph is drawn, and to fill
 * the row exposed by a scroll, so a change of background takes effect upon what
 * is written next and not upon what stands already.
 */
void ConsoleSetColour(uint32_t foreground, uint32_t background);

/* The extent of the console, in characters. Zero where it is not active. */
uint32_t ConsoleColumns(void);
uint32_t ConsoleRows(void);

/* The active position, in characters. */
uint32_t ConsoleColumn(void);
uint32_t ConsoleRow(void);

/*
 * Records the active position as the limit a backspace may not retreat past.
 *
 * The reason is the reason the text-mode driver has one, and it is the same
 * reason: an echo loop must not let a person backspace over the prompt, or over
 * output the kernel wrote and they did not type. See
 * docs/devices/DISPLAY.md, Section 7.
 */
void ConsoleSetEraseLimit(void);

/*
 * Stops the console drawing, permanently, and gives the framebuffer to whoever
 * asked. Subsequent writes are discarded here; the display driver and the serial
 * port continue to receive everything.
 *
 * This exists for the fault screens of sub-task 6.4, and the reason is a fault
 * that was found by looking at one. A fault screen is drawn from within a fault
 * handler, and the panic that follows goes on writing to the diagnostic path —
 * which is this console, upon the same framebuffer. Its cursor was at the foot
 * of a screen full of boot log, so each newline of "KERNEL PANIC: ..." scrolled
 * the framebuffer up by a row of pixels: the fault screen's banner was carried
 * off the top of the display and its whole layout was shifted by three character
 * rows. The screen was correct and what was displayed was not.
 *
 * There is no resumption. A machine that has drawn a fault screen is halting,
 * and a console that could be restarted would be a mechanism with no caller.
 */
void ConsoleSuspend(void);

/* Emits a summary upon the diagnostic path. */
void ConsoleReport(void);

#endif /* OXYS_CONSOLE_H */
