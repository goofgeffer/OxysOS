/*
 * File: kernel/test/verify_faultscreen.c
 * Purpose: Asserts what is to be done about each exception — resumed,
 *          terminating the program that raised it, or fatal to the kernel — and
 *          the table of fault screens that the last of those three draws.
 * Key functions: KernelVerifyFaultScreen.
 * References:
 *   - docs/design/GRAPHICS.md, Section 25: every assertion below, paired with
 *     the silent failure it catches.
 *   - Intel 64 and IA-32 Architectures Software Developer's Manual, Volume 3A,
 *     Table 6-1 and Section 6.5: the exceptions, and their classification as
 *     faults, traps and aborts, from which the dispositions follow.
 *
 * The disposition is asserted here and not where the exceptions are handled,
 * because it is the thing the screens depend upon and because it cannot be
 * exercised any other way. Half of it concerns faults raised at privilege level
 * 3, and there is no code outside the kernel to raise one until sub-task 6.10.
 * `ExceptionDispositionOf` is a pure function of a vector and a code segment
 * selector, so it can be asked the question for a privilege level that does not
 * yet exist — which is the same idiom sub-task 6.1 used to exercise SYSCALL from
 * privilege level 0.
 *
 * What can be asserted here, and what cannot.
 *
 * This asserts the table and not the drawing. A screen is a page of text, and
 * whether a page reads well is not a thing a kernel can determine about itself
 * — no more than it can determine that anything appeared upon the display at
 * all, which is the limitation Section 8 records for the framebuffer. The pages
 * are judged by a person, through the demonstration entry point, and
 * docs/project/TESTING.md, Section 19, records that procedure.
 *
 * What is asserted is everything about the table that a person reading one
 * screen would not notice: that an entry added in a later phase carries all four
 * of its sentences, that it has not been given a colour or a title another entry
 * already has, that its title will not run off the edge of a 640-pixel display
 * which the person judging it was not using, and — the assertion that ties the
 * two halves of this file together — **that no screen exists for a fault that
 * can never be fatal**, such a page being one nobody could ever see and a claim
 * that the kernel treats as a catastrophe something it does not.
 *
 * Nothing here draws. Drawing would set the flag that records a screen as having
 * been shown, and a real fault later in the same boot would then find the screen
 * already taken and print nothing.
 */

#include <oxys/kernel.h>
#include <oxys/verify.h>
#include <oxys/faultscreen.h>
#include <oxys/exceptions.h>
#include <oxys/font.h>

static bool KernelFaultScreenSucceeded;

static void KernelFaultScreenRequire(bool condition, const char *statement)
{
    if (!condition)
    {
        KernelWriteString("  ");
        KernelWriteString(statement);
        KernelWriteString("\n");
        KernelFaultScreenSucceeded = false;
    }
}

/* The length of a null-terminated string. There is no string library in this
 * kernel and this is the only thing here that needs one. */
static size_t KernelFaultScreenLength(const char *text)
{
    size_t length = 0U;

    while (text[length] != '\0')
    {
        ++length;
    }

    return length;
}

/* Whether two null-terminated strings are the same. */
static bool KernelFaultScreenSame(const char *first, const char *second)
{
    size_t index = 0U;

    while ((first[index] != '\0') && (first[index] == second[index]))
    {
        ++index;
    }

    return first[index] == second[index];
}

/*
 * The narrowest display this kernel has been handed, in pixels.
 *
 * VirtualBox supplies 640 by 480 where QEMU supplies 1280 by 800, and a title
 * that fits the one and not the other would be found by whichever of the two the
 * person judging the screens did not use. The banner is drawn at twice life size
 * upon a display this narrow, so a title may occupy at most 640 / (8 * 2) = 40
 * characters.
 */
#define KERNEL_FAULT_NARROW_WIDTH   640U
#define KERNEL_FAULT_NARROW_SCALE   2U
#define KERNEL_FAULT_TITLE_MAXIMUM  (KERNEL_FAULT_NARROW_WIDTH / (8U * KERNEL_FAULT_NARROW_SCALE))

/*
 * The vectors that are fatal to the kernel whatever privilege level raised them,
 * and which must therefore each have a screen written for them.
 *
 * These are the aborts — the double fault and the machine check, which the
 * architecture permits no resumption from — the non-maskable interrupt, which is
 * hardware announcing a condition rather than a program erring, and the two
 * descriptor-table faults, whose malformed structure is the kernel's own however
 * it was reached.
 *
 * An entry deleted from the table by accident would leave its fault falling back
 * to the general screen, which still names the vector and so would not look
 * broken — it would merely be less useful than it was the day before, silently.
 */
static const uint64_t KernelFaultAlwaysFatal[] = { 2U, 8U, 10U, 11U, 18U };

#define KERNEL_FAULT_ALWAYS_FATAL_COUNT                                                  \
    (sizeof(KernelFaultAlwaysFatal) / sizeof(KernelFaultAlwaysFatal[0]))

/*
 * The faults that belong to whoever raised them.
 *
 * Each of these is an ordinary mistake of a program: a divide by zero, an
 * instruction that is not one, an operation the protection rules forbid, a
 * memory access with no translation, an unaligned access. Raised at privilege
 * level 3 the mistake is the program's and must cost the program alone; raised
 * at privilege level 0 the same mistake was the kernel's, and there is nothing
 * smaller than the machine to abandon.
 *
 * That asymmetry is the whole of the classification, and it is asserted here for
 * both privilege levels because the privilege level 3 half of it cannot be
 * reached by any other means: there is no code outside the kernel to raise such
 * a fault until sub-task 6.10, and this is a pure function of its arguments, so
 * it can be asked the question without one.
 */
static const uint64_t KernelFaultBelongsToProgram[] = { 0U, 5U, 6U, 12U, 13U, 14U, 17U };

#define KERNEL_FAULT_PROGRAM_COUNT                                                       \
    (sizeof(KernelFaultBelongsToProgram) / sizeof(KernelFaultBelongsToProgram[0]))

/* A code segment selector for each privilege level, as the processor would have
 * pushed it. */
#define KERNEL_FAULT_KERNEL_CS UINT64_C(0x08)
#define KERNEL_FAULT_USER_CS   UINT64_C(0x2B)

/*
 * Asserts the disposition of every exception, at both privilege levels.
 *
 * This is the assertion that the fault screens are reserved for what they claim
 * to be reserved for. The screens announce that the system has stopped, and
 * showing one for a fault that ought to have cost a single program would be a
 * lie about what happened — so what may draw one is decided here, by vector and
 * by privilege level together, and asserted before any of it is drawn.
 */
static void KernelVerifyFaultDisposition(void)
{
    KernelFaultScreenRequire(ExceptionCameFromUserMode(KERNEL_FAULT_USER_CS),
                             "a privilege level 3 selector was not recognised as being "
                             "outside the kernel");
    KernelFaultScreenRequire(!ExceptionCameFromUserMode(KERNEL_FAULT_KERNEL_CS),
                             "a privilege level 0 selector was taken for one outside the "
                             "kernel, so every kernel fault would be blamed upon a "
                             "program");

    KernelFaultScreenRequire(ExceptionOnlyOutsideKernel(17U),
                             "the alignment check is not recognised as a fault the "
                             "processor raises only outside the kernel");
    KernelFaultScreenRequire(!ExceptionOnlyOutsideKernel(14U),
                             "the page fault is claimed to be raisable only outside the "
                             "kernel, which would forbid it a screen it needs");

    /* The traps resume. A trap reports the state after its instruction, so
     * returning does not re-enter it. */
    for (uint64_t vector = 3U; vector <= 4U; ++vector)
    {
        if (ExceptionDispositionOf(vector, KERNEL_FAULT_KERNEL_CS) !=
            EXCEPTION_DISPOSITION_RESUME)
        {
            KernelWriteString("  a trap is not resumable, at vector ");
            KernelWriteHexadecimal(vector);
            KernelWriteString("\n");
            KernelFaultScreenSucceeded = false;
        }
    }

    /* The unconditional kernel threats are fatal at both privilege levels. */
    for (size_t index = 0U; index < KERNEL_FAULT_ALWAYS_FATAL_COUNT; ++index)
    {
        const uint64_t vector = KernelFaultAlwaysFatal[index];

        if ((ExceptionDispositionOf(vector, KERNEL_FAULT_KERNEL_CS) !=
             EXCEPTION_DISPOSITION_FATAL) ||
            (ExceptionDispositionOf(vector, KERNEL_FAULT_USER_CS) !=
             EXCEPTION_DISPOSITION_FATAL))
        {
            KernelWriteString("  a fault that threatens the kernel whatever raised it is "
                              "not fatal, at vector ");
            KernelWriteHexadecimal(vector);
            KernelWriteString("\n");
            KernelFaultScreenSucceeded = false;
        }
    }

    /*
     * The asymmetry: a program's mistake costs the program, and the same mistake
     * made by the kernel costs the machine.
     */
    for (size_t index = 0U; index < KERNEL_FAULT_PROGRAM_COUNT; ++index)
    {
        const uint64_t vector = KernelFaultBelongsToProgram[index];

        if (ExceptionDispositionOf(vector, KERNEL_FAULT_USER_CS) !=
            EXCEPTION_DISPOSITION_TERMINATE)
        {
            KernelWriteString("  a program's own fault would halt the machine rather than "
                              "the program, at vector ");
            KernelWriteHexadecimal(vector);
            KernelWriteString("\n");
            KernelFaultScreenSucceeded = false;
        }

        if (ExceptionDispositionOf(vector, KERNEL_FAULT_KERNEL_CS) !=
            EXCEPTION_DISPOSITION_FATAL)
        {
            KernelWriteString("  a fault raised within the kernel would be blamed upon a "
                              "program, at vector ");
            KernelWriteHexadecimal(vector);
            KernelWriteString("\n");
            KernelFaultScreenSucceeded = false;
        }
    }

    /*
     * The kernel is never treated more leniently than a program, for any vector
     * at all — including the ones Intel reserves and the ones a later extension
     * may introduce. A fault the kernel has no account of is one it cannot know
     * its state after.
     */
    for (uint64_t vector = 0U; vector < 32U; ++vector)
    {
        const ExceptionDisposition kernel =
            ExceptionDispositionOf(vector, KERNEL_FAULT_KERNEL_CS);
        const ExceptionDisposition user =
            ExceptionDispositionOf(vector, KERNEL_FAULT_USER_CS);

        if ((kernel != EXCEPTION_DISPOSITION_RESUME) &&
            (kernel != EXCEPTION_DISPOSITION_TERMINATE) &&
            (kernel != EXCEPTION_DISPOSITION_FATAL))
        {
            KernelWriteString("  a vector has no disposition at all, at vector ");
            KernelWriteHexadecimal(vector);
            KernelWriteString("\n");
            KernelFaultScreenSucceeded = false;
            continue;
        }

        if (kernel < user)
        {
            KernelWriteString("  a fault is treated more leniently within the kernel than "
                              "outside it, at vector ");
            KernelWriteHexadecimal(vector);
            KernelWriteString("\n");
            KernelFaultScreenSucceeded = false;
        }

        /* Nothing raised outside the kernel terminates the kernel, save the
         * conditions that are not a program's doing at all. */
        if ((user == EXCEPTION_DISPOSITION_FATAL) &&
            (kernel != EXCEPTION_DISPOSITION_FATAL))
        {
            KernelWriteString("  a fault outside the kernel is fatal where the same fault "
                              "within it is not, at vector ");
            KernelWriteHexadecimal(vector);
            KernelWriteString("\n");
            KernelFaultScreenSucceeded = false;
        }
    }
}

void KernelVerifyFaultScreen(void)
{
    const size_t count = FaultScreenEntryCount();

    KernelFaultScreenSucceeded = true;

    KernelVerifyFaultDisposition();

    KernelFaultScreenRequire(count > 0U, "the fault screen table is empty");

    /*
     * Nothing has drawn a screen. If something had, a fault later in this boot
     * would find the screen taken and would draw nothing at all — the failure
     * being invisible precisely when it matters.
     */
    KernelFaultScreenRequire(!FaultScreenWasDrawn(),
                             "a fault screen has already been drawn, so a real fault "
                             "would be refused the display");

    for (size_t index = 0U; index < count; ++index)
    {
        const FaultScreenEntry *const entry = FaultScreenEntryAt(index);

        if (entry == NULL)
        {
            KernelFaultScreenRequire(false,
                                     "the table reported more entries than it will yield");
            break;
        }

        /*
         * Every sentence is present. A screen missing its account or its
         * direction still draws, and draws a blank space where the reader
         * expected the one thing the page was for.
         */
        if ((entry->title == NULL) || (entry->mnemonic == NULL) ||
            (entry->meaning == NULL) || (entry->examine == NULL) ||
            (KernelFaultScreenLength(entry->title) == 0U) ||
            (KernelFaultScreenLength(entry->meaning) == 0U) ||
            (KernelFaultScreenLength(entry->examine) == 0U))
        {
            KernelWriteString("  a fault screen is missing its title, its account or its "
                              "direction, at vector ");
            KernelWriteHexadecimal(entry->vector);
            KernelWriteString("\n");
            KernelFaultScreenSucceeded = false;
            continue;
        }

        /*
         * Every character of every sentence is one the font can draw.
         *
         * This caught a real fault and is kept for that reason. The prose of
         * this project uses an em dash, and one reached a string literal rather
         * than staying in a comment: in UTF-8 that is three bytes, none of them
         * within the printable ASCII the face covers, so the sentence rendered
         * with three replacement boxes in the middle of it. The fault was
         * visible only to somebody looking at the page — the boxes are the font
         * behaving exactly as designed — and it is the kind of thing that
         * reaches a fault screen and is found by the person the screen was
         * written for, at the worst possible moment.
         */
        {
            const char *const sentences[3] = { entry->title, entry->meaning,
                                               entry->examine };

            for (size_t which = 0U; which < 3U; ++which)
            {
                for (size_t at = 0U; sentences[which][at] != '\0'; ++at)
                {
                    const uint8_t code = (uint8_t)sentences[which][at];

                    if ((code < FONT_FIRST_CODE) || (code > FONT_LAST_CODE))
                    {
                        KernelWriteString("  a fault screen's text holds a character the "
                                          "font cannot draw, at vector ");
                        KernelWriteHexadecimal(entry->vector);
                        KernelWriteString(", code ");
                        KernelWriteHexadecimal(code);
                        KernelWriteString("\n");
                        KernelFaultScreenSucceeded = false;
                        break;
                    }
                }
            }
        }

        /* The title fits the narrowest display at the scale used there. */
        if (KernelFaultScreenLength(entry->title) > KERNEL_FAULT_TITLE_MAXIMUM)
        {
            KernelWriteString("  a fault screen's title will run off a 640-pixel display, "
                              "at vector ");
            KernelWriteHexadecimal(entry->vector);
            KernelWriteString("\n");
            KernelFaultScreenSucceeded = false;
        }

        /* The evidence names only panels that exist. A flag outside the defined
         * set draws nothing and looks exactly like a screen that was meant to
         * carry no evidence. */
        if ((entry->evidence &
             ~(FAULT_EVIDENCE_FAULT_ADDRESS | FAULT_EVIDENCE_SELECTOR |
               FAULT_EVIDENCE_OPCODE | FAULT_EVIDENCE_STACK | FAULT_EVIDENCE_OPERANDS |
               FAULT_EVIDENCE_CONTROL)) != 0U)
        {
            KernelWriteString("  a fault screen asks for a panel that does not exist, at "
                              "vector ");
            KernelWriteHexadecimal(entry->vector);
            KernelWriteString("\n");
            KernelFaultScreenSucceeded = false;
        }
    }

    /*
     * No two screens are the same screen.
     *
     * This is the assertion the whole arrangement exists for. The point of a
     * table of fault screens rather than one screen for every fault is that a
     * reader can tell the faults apart, and two entries sharing a title or a
     * colour would quietly undo that — a copy-and-paste of a row, with the
     * vector changed and the identity not.
     */
    for (size_t first = 0U; first < count; ++first)
    {
        const FaultScreenEntry *const one = FaultScreenEntryAt(first);

        for (size_t second = first + 1U; second < count; ++second)
        {
            const FaultScreenEntry *const other = FaultScreenEntryAt(second);

            if ((one == NULL) || (other == NULL))
            {
                break;
            }

            if (one->vector == other->vector)
            {
                KernelWriteString("  two fault screens claim the same vector, ");
                KernelWriteHexadecimal(one->vector);
                KernelWriteString("\n");
                KernelFaultScreenSucceeded = false;
            }

            if (KernelFaultScreenSame(one->title, other->title))
            {
                KernelWriteString("  two fault screens share a title, at vectors ");
                KernelWriteHexadecimal(one->vector);
                KernelWriteString(" and ");
                KernelWriteHexadecimal(other->vector);
                KernelWriteString("\n");
                KernelFaultScreenSucceeded = false;
            }

            if ((one->red == other->red) && (one->green == other->green) &&
                (one->blue == other->blue))
            {
                KernelWriteString("  two fault screens share a colour, at vectors ");
                KernelWriteHexadecimal(one->vector);
                KernelWriteString(" and ");
                KernelWriteHexadecimal(other->vector);
                KernelWriteString("\n");
                KernelFaultScreenSucceeded = false;
            }
        }
    }

    /* Every fault that threatens the kernel whatever raised it has a screen
     * written for it. */
    for (size_t index = 0U; index < KERNEL_FAULT_ALWAYS_FATAL_COUNT; ++index)
    {
        bool found = false;

        for (size_t entry = 0U; entry < count; ++entry)
        {
            const FaultScreenEntry *const candidate = FaultScreenEntryAt(entry);

            if ((candidate != NULL) &&
                (candidate->vector == KernelFaultAlwaysFatal[index]))
            {
                found = true;
                break;
            }
        }

        if (!found)
        {
            KernelWriteString("  a fault that threatens the kernel has no screen of its "
                              "own, at vector ");
            KernelWriteHexadecimal(KernelFaultAlwaysFatal[index]);
            KernelWriteString("\n");
            KernelFaultScreenSucceeded = false;
        }
    }

    /*
     * **Every screen must be reachable.**
     *
     * A screen exists to be drawn, and one is drawn only for a fault whose
     * disposition is fatal. An entry for a vector that can never be fatal — an
     * alignment check, which the processor raises only at privilege level 3, or
     * anything else that always belongs to the program that caused it — is a
     * page nobody will ever see, and its presence would suggest the kernel
     * treats that fault as a catastrophe when it does not.
     */
    for (size_t entry = 0U; entry < count; ++entry)
    {
        const FaultScreenEntry *const candidate = FaultScreenEntryAt(entry);

        if ((candidate == NULL) || (candidate->vector == FAULT_SCREEN_SOFTWARE))
        {
            continue;
        }

        if (ExceptionDispositionOf(candidate->vector, KERNEL_FAULT_KERNEL_CS) !=
            EXCEPTION_DISPOSITION_FATAL)
        {
            KernelWriteString("  a screen exists for a fault that is never fatal, so it "
                              "can never be drawn, at vector ");
            KernelWriteHexadecimal(candidate->vector);
            KernelWriteString("\n");
            KernelFaultScreenSucceeded = false;
        }

        /*
         * And the architectural half of the same rule. A fault the processor
         * raises only at privilege level 3 can never be the kernel's, so a
         * screen for it would state that the kernel treats a program's mistake
         * as the end of the machine.
         */
        if (ExceptionOnlyOutsideKernel(candidate->vector))
        {
            KernelWriteString("  a screen exists for a fault the processor raises only "
                              "outside the kernel, at vector ");
            KernelWriteHexadecimal(candidate->vector);
            KernelWriteString("\n");
            KernelFaultScreenSucceeded = false;
        }
    }

    {
        bool software = false;

        for (size_t entry = 0U; entry < count; ++entry)
        {
            const FaultScreenEntry *const candidate = FaultScreenEntryAt(entry);

            if ((candidate != NULL) && (candidate->vector == FAULT_SCREEN_SOFTWARE))
            {
                software = true;
            }
        }

        KernelFaultScreenRequire(software,
                                 "there is no screen for a panic the kernel raises "
                                 "itself, so it would be shown one written for a "
                                 "processor exception that did not occur");
    }

    /* An index past the end yields nothing rather than reading past the table. */
    KernelFaultScreenRequire(FaultScreenEntryAt(count) == NULL,
                             "the table yielded an entry past its own end");

    KernelWriteString(KernelFaultScreenSucceeded
                          ? "Fault disposition and screen self-test passed.\n"
                          : "Fault disposition and screen self-test FAILED.\n");
}
