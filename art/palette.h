/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: CC0-1.0 */
/*
 * File: art/palette.h
 * Purpose: The colours of Oxys-OS, named once so that the kernel's boot screen,
 *          the window manager's frames and the session's desktop cannot drift
 *          apart from one another.
 * Key definitions: OXYS_GROUND_*, OXYS_BAR_*, OXYS_BAR_QUIET_*, OXYS_INK_*,
 *          OXYS_DIM_*, OXYS_PAPER_*, OXYS_BORDER_*, OXYS_DISC_*, OXYS_RGB.
 * References:
 *   - docs/project/INSPIRATIONS.md: a small palette, mostly quiet,
 *     with colour reserved for the few things that must be told apart at a
 *     glance — and legibility taken over prettiness wherever the two disagree.
 *   - docs/design/SESSION.md: why the boot screen and the desktop
 *     must draw the same thing in the same colours.
 *
 * Why the colours are triples and not pixels.
 *
 *   A pixel value means nothing without an encoding: the kernel's is
 *   FramebufferEncode, which asks the adapter where the channels sit, and a
 *   program's is the 0x00RRGGBB the window protocol carries. Naming the
 *   channels leaves each side to make its own pixel, and is the reason this
 *   header may be included by both without either knowing about the other.
 *
 * Why it is here, with the mark, rather than in either.
 *
 *   The kernel is LGPL and the session is MIT, and a palette belonging to
 *   either could not be used by the other without a relicensing this project
 *   may not perform. art/README.md records the same argument for the mark; the
 *   colours follow it because they are the same kind of thing — a decision
 *   about appearance, drawn upon by two programs under two licences.
 */

#ifndef OXYS_ART_PALETTE_H
#define OXYS_ART_PALETTE_H

/*
 * The ground: the yellow everything stands upon — the boot screen, the power
 * screen, the desktop, and the screen where no window is.
 */
#define OXYS_GROUND_RED   233U
#define OXYS_GROUND_GREEN 186U
#define OXYS_GROUND_BLUE  60U

/*
 * The bars: the panel, and the title band of the window holding the focus. A
 * lighter and more orange yellow than the ground, which is what separates a bar
 * from the ground behind it without a line between them — the two differ in
 * lightness and in hue at once, so neither a person who sees colour poorly nor
 * a screen that renders it badly is left with two identical yellows.
 */
#define OXYS_BAR_RED   255U
#define OXYS_BAR_GREEN 201U
#define OXYS_BAR_BLUE  120U

/*
 * The quiet bar: the title band of every window that does not hold the focus.
 * Paler and less saturated, so that "which window takes the keys" is answered
 * by how strong a colour is rather than by which colour it is.
 */
#define OXYS_BAR_QUIET_RED   243U
#define OXYS_BAR_QUIET_GREEN 219U
#define OXYS_BAR_QUIET_BLUE  168U

/* The ink: text and outlines. A dark brown rather than black, black upon a
 * saturated yellow being harsher than anything else here. */
#define OXYS_INK_RED   58U
#define OXYS_INK_GREEN 40U
#define OXYS_INK_BLUE  14U

/* The dim ink: text that is present and not being read — a version string, a
 * line beneath a mark. */
#define OXYS_DIM_RED   124U
#define OXYS_DIM_GREEN 96U
#define OXYS_DIM_BLUE  40U

/* The paper: a window's content, as it is created. Warm rather than white, for
 * the reason it was warm before: pure white glares beside dark text. */
#define OXYS_PAPER_RED   250U
#define OXYS_PAPER_GREEN 246U
#define OXYS_PAPER_BLUE  236U

/* The border about a window's frame: darker than the ground and than the bar,
 * so that a window has an edge upon either. */
#define OXYS_BORDER_RED   120U
#define OXYS_BORDER_GREEN 88U
#define OXYS_BORDER_BLUE  30U

/*
 * The mark's disc, of art/logo.h. It is the colour the mark was drawn in and
 * is not a choice made here: it is measured from the artwork, so that the
 * bitmap and whatever fills it agree.
 */
#define OXYS_DISC_RED   201U
#define OXYS_DISC_GREEN 160U
#define OXYS_DISC_BLUE  96U

/* A client's pixel, which the window protocol carries as 0x00RRGGBB. The
 * kernel does not use this: it asks FramebufferEncode, the adapter's channels
 * not being in this order upon every machine. */
#define OXYS_RGB(red, green, blue) \
    (((unsigned int)(red) << 16) | ((unsigned int)(green) << 8) | (unsigned int)(blue))

#endif /* OXYS_ART_PALETTE_H */
