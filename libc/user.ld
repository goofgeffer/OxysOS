/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: libc/user.ld
 * Purpose: The linker script every user program of this system is linked with:
 *          where a program is loaded, which sections become which segments, and
 *          the page alignment between them that keeps their permissions apart.
 * Key definitions: _start (the entry point), USER_LOAD_ADDRESS.
 * References:
 *   - System V Application Binary Interface, AMD64 supplement, and the generic
 *     ELF specification: PT_LOAD segments, their permissions, and the rule that
 *     loadable segments appear in ascending order of virtual address.
 *   - kernel/exec/elf.c: the loader these choices are made for. It refuses a
 *     segment below the first page, refuses one at or above
 *     SYSCALL_USER_LIMIT, and refuses a table whose segments are not ascending
 *     and disjoint.
 *   - kernel/abi/oxys/syscall_abi.h: SYSCALL_USER_LIMIT, the boundary a program
 *     may not name.
 *   - kernel/include/oxys/proc/process.h: PROCESS_USER_STACK_TOP, which is
 *     where the stack is, and PROCESS_BREAK_MAXIMUM, which bounds the heap that
 *     grows upward from just above this image.
 *   - docs/design/LIBC.md, Section 11.4: the link procedure of which this is
 *     one half.
 *
 * Why four mebibytes, and not one or zero.
 *
 *   The first page of an address space must stay unmapped, so that a null
 *   pointer dereference faults rather than reading whatever a segment put there;
 *   kernel/exec/elf.c refuses any segment below it for exactly that reason. Four
 *   mebibytes is what every toolchain upon this architecture has used for a
 *   non-relocatable executable since the ELF ABI was written, so a program built
 *   here and read by any other tool lands where that tool expects — and the
 *   space below it is a whole page-directory entry's worth of address, which is
 *   room for anything a later phase decides belongs below a program.
 *
 * Why every segment is page-aligned, when the loader does not require it.
 *
 *   The loader will reuse a page a segment shares with the one before it, so an
 *   image whose read-only and writable sections meet inside a page would load.
 *   It would load with one permission for both, arbitrarily chosen — the writable
 *   segment's, since it arrives second — and the result is a program whose
 *   constants are writable and whose text may be. The alignment below costs at
 *   most three pages of address space per program and removes the question.
 *
 *   **The ALIGN(4K) directives below are also what keeps the image small**, and
 *   that was found by measurement rather than assumed. `-z max-page-size=0x1000`
 *   was passed alongside this script on the belief that it was doing that work;
 *   with this script present it changes the output by not one byte, every
 *   segment's virtual address having already been fixed here, and it has been
 *   removed from the Makefile. What does the work there is `-n`, which stops the
 *   linker padding the *file* between segments for a demand-paged loader this
 *   kernel is not.
 *
 * What this alignment cannot be checked by.
 *
 *   Nothing in this project asserts that the three segments really were given
 *   three different permissions. The kernel would have to interrogate an address
 *   space about what it maps, and an address space cannot answer that — which is
 *   limitation 2 of ../docs/design/MEMORY-LAYOUT.md, recorded there long before
 *   this file existed. The negative test that deleted these ALIGN directives
 *   reported nothing, and this paragraph is the record of why.
 */

ENTRY(_start)

USER_LOAD_ADDRESS = 0x400000;

PHDRS
{
    /*
     * Three segments, one per permission. They are declared explicitly rather
     * than left to the linker's own grouping, because the grouping is what
     * decides the permissions this kernel maps the pages with, and a program's
     * text being writable is not a thing to arrive at by default.
     *
     * FILEHDR and PHDRS are deliberately not included in any of them. Including
     * them would place the ELF headers inside the first loadable segment, which
     * is what a dynamic loader needs in order to find the program header table
     * at run time; nothing here has a dynamic loader, and the cost is that the
     * first segment's file offset and virtual address stop agreeing modulo the
     * page size, which is a thing to get wrong for no gain.
     */
    text    PT_LOAD FLAGS(5); /* R + X */
    rodata  PT_LOAD FLAGS(4); /* R     */
    data    PT_LOAD FLAGS(6); /* R + W */
}

SECTIONS
{
    . = USER_LOAD_ADDRESS;

    .text ALIGN(4K) : {
        /*
         * The startup object first, and named by a section of its own.
         *
         * The entry point recorded in the file header is `_start` wherever it
         * lands, so this is not what makes the program run correctly — it is
         * what makes the program's first instruction also the lowest address of
         * its image, which is a property the self-test asserts and a reader of a
         * disassembly relies upon.
         *
         * **It was `*(.text.startup)` first, and that was wrong.** GCC places
         * `main` in `.text.startup` at -O2, so that spelling put the program's
         * `main` ahead of the runtime's `_start` — which loaded, ran, and would
         * have kept working until somebody read the entry point and believed it.
         * The startup object therefore declares `.text.entry`, a name no
         * compiler emits, and this line is the only thing that places it.
         */
        *(.text.entry)
        *(.text.startup)
        *(.text .text.*)
    } :text

    .rodata ALIGN(4K) : {
        *(.rodata .rodata.*)
    } :rodata

    .data ALIGN(4K) : {
        *(.data .data.*)
    } :data

    /*
     * The uninitialised data shares the writable segment with the initialised
     * data, which is what makes its memory size exceed its file size — and that
     * is the one thing in this script the kernel's loader is asked to do beyond
     * copying bytes. kernel/exec/elf.c zeroes the difference; an image whose
     * .bss were a segment of its own would work equally well and would cost a
     * page.
     */
    .bss : {
        *(COMMON)
        *(.bss .bss.*)
    } :data

    /*
     * Everything a program does not need at run time is discarded rather than
     * left to land wherever the linker puts it. A note or a comment section with
     * no address is harmless; one that acquires an address becomes a fourth
     * segment, and a fourth segment is a page this kernel maps for a build
     * identifier nothing reads.
     */
    /DISCARD/ : {
        *(.comment)
        *(.note .note.*)
        *(.eh_frame .eh_frame_hdr)
        *(.gnu.hash)
        *(.gnu_debuglink)
    }
}
