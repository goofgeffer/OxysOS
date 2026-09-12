/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/test/program.c
 * Purpose: Composes an ELF64 program byte by byte — the file header, the program
 *          header table, and the handful of instruction forms a self-test's
 *          program needs — for the tests that must run code at privilege level 3
 *          and have no compiler to produce it with.
 * Key functions: TestProgramInitialise, TestProgramElfHeader, TestProgramSegment,
 *          TestProgramMoveImmediate32, TestProgramMoveImmediate64,
 *          TestProgramMoveRegister, TestProgramAddRegister,
 *          TestProgramSubtractRegister, TestProgramMultiplyImmediate,
 *          TestProgramCall, TestProgramUndefined.
 * References: kernel/test/program.h states what this implements and why it is
 *          one translation unit rather than a copy inside each test.
 *   - Intel 64 and IA-32 Architectures Software Developer's Manual, Volume 2A,
 *     Table 2-2: the register numbers of the ModR/M byte.
 *   - Intel SDM, Volume 2A and 2B: "MOV", "ADD", "SUB", "IMUL", "CALL" and
 *     "UD2", from which each encoding below is taken.
 *   - ELF-64 Object File Format, version 1.5 draft 2: the file header's fields
 *     and the program header's, at the offsets <oxys/elf.h> decodes them from.
 *
 * Every write goes through one bounds check.
 *
 *   TestProgramRoom is the only place this file touches the array, and every
 *   emission asks it first. A composer that ran off the end of its buffer would
 *   write into whatever the linker placed after it, and the failure would appear
 *   in an unrelated subsystem long after the self-test that caused it had
 *   reported success. The refusal is recorded rather than reported at each call
 *   site, so that a program is composed as a sequence of statements and checked
 *   once at the end.
 */

#include "program.h"

#include <oxys/elf.h>
#include <oxys/memory.h>

/*
 * Whether `count` bytes may be written at `at`, recording the refusal where they
 * may not.
 *
 * The sum is guarded because both operands come from a caller's arithmetic: a
 * displacement computed wrongly could produce an offset near the greatest
 * representable value, and `at + count` would then wrap to a small number that
 * passes every comparison against the capacity.
 */
static bool TestProgramRoom(TestProgram *program, uint64_t at, size_t count)
{
    if ((program == NULL) || (program->image == NULL))
    {
        return false;
    }

    if ((at >= program->capacity) || ((uint64_t)count > (program->capacity - at)))
    {
        program->overran = true;

        return false;
    }

    return true;
}

void TestProgramInitialise(TestProgram *program, uint8_t *image, size_t capacity)
{
    if (program == NULL)
    {
        return;
    }

    program->image = image;
    program->capacity = (image == NULL) ? 0U : capacity;
    program->at = 0U;
    program->overran = false;

    for (size_t index = 0U; index < program->capacity; ++index)
    {
        image[index] = 0U;
    }
}

void TestProgramSeek(TestProgram *program, uint64_t offset)
{
    if (program == NULL)
    {
        return;
    }

    if (offset > (uint64_t)program->capacity)
    {
        program->overran = true;

        return;
    }

    program->at = offset;
}

void TestProgramCopy(TestProgram *program, uint64_t at, const uint8_t *bytes,
                     size_t count)
{
    if ((bytes == NULL) || !TestProgramRoom(program, at, count))
    {
        return;
    }

    for (size_t index = 0U; index < count; ++index)
    {
        program->image[at + index] = bytes[index];
    }
}

/* Whether `count` bytes may be written at the cursor. The null check is here and
 * not in TestProgramRoom because the cursor must be read to form the argument,
 * and reading it through a null pointer is the one failure a bounds check placed
 * after the read cannot catch. */
static bool TestProgramCursorRoom(TestProgram *program, size_t count)
{
    if ((program == NULL) || (program->image == NULL))
    {
        return false;
    }

    return TestProgramRoom(program, program->at, count);
}

/* One byte, for the emitters. It is not exported: a caller that wants to place a
 * byte of data places it itself, and a caller that wants an instruction asks for
 * the instruction. */
static void TestProgramPut8(TestProgram *program, uint64_t at, uint8_t value)
{
    if (!TestProgramRoom(program, at, 1U))
    {
        return;
    }

    program->image[at] = value;
}

void TestProgramPut16(TestProgram *program, uint64_t at, uint16_t value)
{
    if (!TestProgramRoom(program, at, 2U))
    {
        return;
    }

    program->image[at] = (uint8_t)(value & 0xFFU);
    program->image[at + 1U] = (uint8_t)((value >> 8) & 0xFFU);
}

void TestProgramPut32(TestProgram *program, uint64_t at, uint32_t value)
{
    if (!TestProgramRoom(program, at, 4U))
    {
        return;
    }

    for (uint64_t index = 0U; index < 4U; ++index)
    {
        program->image[at + index] = (uint8_t)((value >> (index * 8U)) & 0xFFU);
    }
}

void TestProgramPut64(TestProgram *program, uint64_t at, uint64_t value)
{
    if (!TestProgramRoom(program, at, 8U))
    {
        return;
    }

    for (uint64_t index = 0U; index < 8U; ++index)
    {
        program->image[at + index] = (uint8_t)((value >> (index * 8U)) & 0xFFU);
    }
}

void TestProgramElfHeader(TestProgram *program, uint64_t entry, uint16_t segments)
{
    if (!TestProgramRoom(program, 0U, ELF_HEADER_BYTES))
    {
        return;
    }

    program->image[0] = 0x7FU;
    program->image[1] = (uint8_t)'E';
    program->image[2] = (uint8_t)'L';
    program->image[3] = (uint8_t)'F';
    program->image[4] = (uint8_t)ELF_CLASS_64;
    program->image[5] = (uint8_t)ELF_DATA_LITTLE_ENDIAN;
    program->image[6] = (uint8_t)ELF_VERSION_CURRENT;

    TestProgramPut16(program, 16U, (uint16_t)ELF_TYPE_EXECUTABLE);
    TestProgramPut16(program, 18U, (uint16_t)ELF_MACHINE_X86_64);
    TestProgramPut32(program, 20U, ELF_VERSION_CURRENT);
    TestProgramPut64(program, 24U, entry);
    TestProgramPut64(program, 32U, ELF_HEADER_BYTES);
    TestProgramPut16(program, 52U, (uint16_t)ELF_HEADER_BYTES);
    TestProgramPut16(program, 54U, (uint16_t)ELF_PROGRAM_HEADER_BYTES);
    TestProgramPut16(program, 56U, segments);
}

void TestProgramSegment(TestProgram *program, uint16_t index, uint32_t flags,
                        uint64_t file_offset, uint64_t address, uint64_t size)
{
    const uint64_t at =
        (uint64_t)ELF_HEADER_BYTES + ((uint64_t)index * (uint64_t)ELF_PROGRAM_HEADER_BYTES);

    if (!TestProgramRoom(program, at, ELF_PROGRAM_HEADER_BYTES))
    {
        return;
    }

    TestProgramPut32(program, at + 0U, ELF_SEGMENT_LOAD);
    TestProgramPut32(program, at + 4U, flags);
    TestProgramPut64(program, at + 8U, file_offset);
    TestProgramPut64(program, at + 16U, address);

    /* The physical address, which this loader does not use and every file
     * carries. It is set to the virtual address, which is what a linker emits
     * for an executable that is not loaded by a boot loader. */
    TestProgramPut64(program, at + 24U, address);

    TestProgramPut64(program, at + 32U, size);
    TestProgramPut64(program, at + 40U, size);
    TestProgramPut64(program, at + 48U, PAGE_SIZE);
}

/* ------------------------------------------------------------- instructions */

void TestProgramMoveImmediate32(TestProgram *program, uint8_t reg, uint32_t value)
{
    if (!TestProgramCursorRoom(program, 5U))
    {
        return;
    }

    TestProgramPut8(program, program->at, (uint8_t)(0xB8U + reg));
    TestProgramPut32(program, program->at + 1U, value);
    program->at += 5U;
}

void TestProgramMoveImmediate64(TestProgram *program, uint8_t reg, uint64_t value)
{
    if (!TestProgramCursorRoom(program, 10U))
    {
        return;
    }

    TestProgramPut8(program, program->at, 0x48U);
    TestProgramPut8(program, program->at + 1U, (uint8_t)(0xB8U + reg));
    TestProgramPut64(program, program->at + 2U, value);
    program->at += 10U;
}

/*
 * The three register-to-register forms differ in one opcode byte apiece and
 * share their ModR/M: 11 reg r/m, the source in the reg field and the
 * destination in the r/m field. Intel SDM, Volume 2A, Table 2-2.
 */
static void TestProgramRegisterForm(TestProgram *program, uint8_t opcode,
                                    uint8_t destination, uint8_t source)
{
    if (!TestProgramCursorRoom(program, 3U))
    {
        return;
    }

    TestProgramPut8(program, program->at, 0x48U);
    TestProgramPut8(program, program->at + 1U, opcode);
    TestProgramPut8(program, program->at + 2U,
                    (uint8_t)(0xC0U | (uint8_t)(source << 3) | destination));
    program->at += 3U;
}

void TestProgramMoveRegister(TestProgram *program, uint8_t destination, uint8_t source)
{
    TestProgramRegisterForm(program, 0x89U, destination, source);
}

void TestProgramAddRegister(TestProgram *program, uint8_t destination, uint8_t source)
{
    TestProgramRegisterForm(program, 0x01U, destination, source);
}

void TestProgramSubtractRegister(TestProgram *program, uint8_t destination,
                                 uint8_t source)
{
    TestProgramRegisterForm(program, 0x29U, destination, source);
}

void TestProgramMultiplyImmediate(TestProgram *program, uint8_t reg, uint32_t value)
{
    if (!TestProgramCursorRoom(program, 7U))
    {
        return;
    }

    TestProgramPut8(program, program->at, 0x48U);
    TestProgramPut8(program, program->at + 1U, 0x69U);
    TestProgramPut8(program, program->at + 2U,
                    (uint8_t)(0xC0U | (uint8_t)(reg << 3) | reg));
    TestProgramPut32(program, program->at + 3U, value);
    program->at += 7U;
}

void TestProgramCall(TestProgram *program, uint64_t target_offset)
{
    int64_t displacement;

    if (!TestProgramCursorRoom(program, 5U))
    {
        return;
    }

    displacement = (int64_t)target_offset - (int64_t)(program->at + 5U);

    TestProgramPut8(program, program->at, 0xE8U);
    TestProgramPut32(program, program->at + 1U, (uint32_t)(int32_t)displacement);
    program->at += 5U;
}

void TestProgramUndefined(TestProgram *program)
{
    if (!TestProgramCursorRoom(program, 2U))
    {
        return;
    }

    TestProgramPut8(program, program->at, 0x0FU);
    TestProgramPut8(program, program->at + 1U, 0x0BU);
    program->at += 2U;
}
