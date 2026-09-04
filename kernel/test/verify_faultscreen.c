/*
 * File: kernel/test/verify_faultscreen.c
 * Purpose: Asserts the table of fault screens of sub-task 6.4: that each severe
 *          fault has a screen of its own, that no two of them are the same, and
 *          that every one of them is complete and will fit the narrowest display
 *          this kernel is tested upon.
 * Key functions: KernelVerifyFaultScreen.
 * References:
 *   - docs/design/GRAPHICS.md, Section 25: every assertion below, paired with
 *     the silent failure it catches.
 *   - Intel 64 and IA-32 Architectures Software Developer's Manual, Volume 3A,
 *     Table 6-1: the exceptions that must each have a screen.
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
 * screen would not notice: that a fourteenth entry added in a later phase
 * carries all four of its sentences, that it has not been given a colour or a
 * title another entry already has, and that its title will not run off the edge
 * of a 640-pixel display which the person judging it was not using.
 *
 * Nothing here draws. Drawing would set the flag that records a screen as having
 * been shown, and a real fault later in the same boot would then find the screen
 * already taken and print nothing.
 */

#include <oxys/kernel.h>
#include <oxys/verify.h>
#include <oxys/faultscreen.h>

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
 * The vectors that must each have a screen written for them.
 *
 * These are the severe faults: the ones from which this kernel cannot continue
 * and which a person will therefore actually meet. An entry deleted from the
 * table by accident would leave its fault falling back to the general screen,
 * which still names the vector and so would not look broken — it would merely be
 * less useful than it was the day before, silently.
 */
static const uint64_t KernelFaultRequiredVectors[] = { 0U,  6U,  8U,  10U, 11U,
                                                       12U, 13U, 14U, 17U, 18U };

#define KERNEL_FAULT_REQUIRED_COUNT                                                      \
    (sizeof(KernelFaultRequiredVectors) / sizeof(KernelFaultRequiredVectors[0]))

void KernelVerifyFaultScreen(void)
{
    const size_t count = FaultScreenEntryCount();

    KernelFaultScreenSucceeded = true;

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

    /* Every severe fault has a screen written for it, and so does the panic the
     * kernel raises itself. */
    for (size_t index = 0U; index < KERNEL_FAULT_REQUIRED_COUNT; ++index)
    {
        bool found = false;

        for (size_t entry = 0U; entry < count; ++entry)
        {
            const FaultScreenEntry *const candidate = FaultScreenEntryAt(entry);

            if ((candidate != NULL) &&
                (candidate->vector == KernelFaultRequiredVectors[index]))
            {
                found = true;
                break;
            }
        }

        if (!found)
        {
            KernelWriteString("  a severe fault has no screen of its own, at vector ");
            KernelWriteHexadecimal(KernelFaultRequiredVectors[index]);
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

    KernelWriteString(KernelFaultScreenSucceeded ? "Fault screen self-test passed.\n"
                                                 : "Fault screen self-test FAILED.\n");
}
