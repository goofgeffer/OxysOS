/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/test/program.h
 * Purpose: Declares the composer with which a self-test builds an ELF64 program
 *          by hand — the header, the segments, and the few instruction forms
 *          such a program needs — so that a test which must run code at
 *          privilege level 3 writes what the program does and not how an
 *          instruction is encoded.
 * Key definitions: TestProgram, TestProgramInitialise, TestProgramElfHeader,
 *          TestProgramSegment, TestProgramSeek, TestProgramCopy,
 *          TestProgramMoveImmediate32, TestProgramMoveImmediate64,
 *          TestProgramMoveRegister, TestProgramAddRegister,
 *          TestProgramSubtractRegister, TestProgramMultiplyImmediate,
 *          TestProgramCall, TestProgramUndefined, TEST_PROGRAM_RAX and the
 *          remaining register numbers.
 * References:
 *   - Intel 64 and IA-32 Architectures Software Developer's Manual, Volume 2A
 *     and 2B: the encoding of each instruction emitted, cited at the function
 *     that emits it.
 *   - Tool Interface Standard, Executable and Linking Format, and the ELF-64
 *     Object File Format: the file header and the program header table, whose
 *     field offsets are the ones <oxys/exec/elf.h> decodes.
 *   - docs/design/LIBC.md, Section 8.4 and Section 9.5: the two self-tests that
 *     use this, and why each of them has to be a program rather than a call.
 *
 * Why this exists, and why it is here rather than in kernel/.
 *
 *   This kernel cannot execute SYSCALL: the SYSRET that ends its handling of one
 *   returns to privilege level 3 unconditionally. So every assertion whose
 *   subject is a system call has to be made by a program, and there is no
 *   compiler to produce one — sub-task 7.5 is where a program is first built
 *   from source. Until then a self-test that needs a program assembles one, byte
 *   by byte, into an array.
 *
 *   Sub-task 7.2 did that once and sub-task 7.3 needed it a second time. Two
 *   copies of an instruction encoder is two places for a byte to be wrong, and
 *   the second copy would have been wrong in a way the first one's assertions
 *   could not see. The encoders are therefore one translation unit, and each
 *   self-test holds only the program it means to run.
 *
 *   It is in kernel/test/ because it is a test's apparatus and not the kernel's:
 *   nothing outside this directory calls it, and nothing in the image would be
 *   missed if every file here were removed.
 */

#ifndef OXYS_TEST_PROGRAM_H
#define OXYS_TEST_PROGRAM_H

#include <oxys/types.h>

/*
 * A program under construction: the array it is being written into, how much of
 * it there is, and where the next instruction goes.
 *
 * `overran` is the one field a caller must look at. Every emission below refuses
 * to write past the end of the array and records that it refused, rather than
 * writing past it — a composer that overran its own buffer inside a self-test
 * would corrupt whatever the linker placed next and the failure would appear
 * somewhere else entirely. The flag is sticky, so one check after the whole
 * program is composed covers every emission in it.
 */
typedef struct TestProgram
{
    uint8_t *image;
    size_t capacity;
    uint64_t at;
    bool overran;
} TestProgram;

/*
 * The register numbers of the encoding, which are not the order a reader would
 * guess. Intel SDM, Volume 2A, Table 2-2.
 */
#define TEST_PROGRAM_RAX 0U
#define TEST_PROGRAM_RCX 1U
#define TEST_PROGRAM_RDX 2U
#define TEST_PROGRAM_RBX 3U
#define TEST_PROGRAM_RSP 4U
#define TEST_PROGRAM_RBP 5U
#define TEST_PROGRAM_RSI 6U
#define TEST_PROGRAM_RDI 7U

/* Prepares a composition: the whole array is cleared and the cursor placed at
 * its beginning. A null array or a capacity of zero leaves a composer that
 * refuses every emission, which is what `overran` then reports. */
void TestProgramInitialise(TestProgram *program, uint8_t *image, size_t capacity);

/* Moves the cursor to an absolute offset within the image. Used to step from the
 * headers to a segment's contents, the two being at fixed places. */
void TestProgramSeek(TestProgram *program, uint64_t offset);

/* Writes bytes at an absolute offset without moving the cursor. This is how the
 * C library's own invocation block is copied into a program's text. */
void TestProgramCopy(TestProgram *program, uint64_t at, const uint8_t *bytes,
                     size_t count);

/* Writes a value at an absolute offset, little endian, without moving the
 * cursor. The ELF headers are composed with these. */
void TestProgramPut16(TestProgram *program, uint64_t at, uint16_t value);
void TestProgramPut32(TestProgram *program, uint64_t at, uint32_t value);
void TestProgramPut64(TestProgram *program, uint64_t at, uint64_t value);

/*
 * Writes the ELF64 file header at offset zero: a 64-bit little-endian executable
 * for this machine, entered at `entry`, whose program header table follows the
 * file header immediately and holds `segments` entries.
 */
void TestProgramElfHeader(TestProgram *program, uint64_t entry, uint16_t segments);

/*
 * Writes one program header, `index` entries into the table.
 *
 * The segment is always PT_LOAD, its size in the file and its size in memory are
 * the same, and its alignment is a page. A self-test has no use for a segment
 * whose memory size exceeds its file size — that is a `.bss`, and a program
 * composed by hand declares the storage it wants rather than relying upon the
 * loader to zero what it did not supply.
 */
void TestProgramSegment(TestProgram *program, uint16_t index, uint32_t flags,
                        uint64_t file_offset, uint64_t address, uint64_t size);

/*
 * The instruction forms these programs need, each emitted at the cursor, which
 * advances past it.
 *
 * They are written out one function apiece rather than driven from a table of
 * opcodes, because a table would have to be read against Intel's Volume 2
 * anyway, and a reader checking this against the manual wants the mnemonic
 * beside the byte.
 */

/* mov r32, imm32 — B8+rd. The 32-bit form zero-extends to the whole register,
 * which is why a call number or a small constant needs no REX prefix. */
void TestProgramMoveImmediate32(TestProgram *program, uint8_t reg, uint32_t value);

/* mov r64, imm64 — REX.W + B8+rd, the only form that can name an address in a
 * program's data page. */
void TestProgramMoveImmediate64(TestProgram *program, uint8_t reg, uint64_t value);

/* mov r/m64, r64 — REX.W + 89 /r, both operands registers. */
void TestProgramMoveRegister(TestProgram *program, uint8_t destination, uint8_t source);

/* add r/m64, r64 — REX.W + 01 /r. */
void TestProgramAddRegister(TestProgram *program, uint8_t destination, uint8_t source);

/* sub r/m64, r64 — REX.W + 29 /r. */
void TestProgramSubtractRegister(TestProgram *program, uint8_t destination,
                                 uint8_t source);

/* imul r64, r/m64, imm32 — REX.W + 69 /r id, the three-operand form, which
 * scales a register without a second register to hold the multiplier in. */
void TestProgramMultiplyImmediate(TestProgram *program, uint8_t reg, uint32_t value);

/*
 * call rel32 — E8 cd, the displacement being relative to the address of the
 * instruction after the call.
 *
 * The target is an offset within the image and not an address, and so is the
 * call: both segments a composed program has are loaded at addresses that differ
 * from their file offsets by the same amount, so a displacement computed in file
 * offsets is the displacement the loaded program needs.
 */
void TestProgramCall(TestProgram *program, uint64_t target_offset);

/* ud2 — 0F 0B, the two bytes the architecture guarantees will raise an invalid
 * opcode exception. It is emitted after a call that must not return, so that a
 * program which somehow came back faults at an instruction belonging to it
 * rather than executing whatever follows in the page. */
void TestProgramUndefined(TestProgram *program);

#endif /* OXYS_TEST_PROGRAM_H */
