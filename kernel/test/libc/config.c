/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/test/libc/config.c
 * Purpose: Asserts the parsing half of the configuration format of sub-task
 *          9.4 upon text composed in memory — the section, the key, the value,
 *          the comment, the quoting, the repeated section that is a list, the
 *          conversions, and every fault the parser records rather than raises —
 *          and then runs config-check at privilege level 3 for the half that
 *          reads a file.
 * Key functions: KernelVerifyConfig.
 * References:
 *   - docs/design/CONFIG.md: every assertion here paired with the
 *     silent failure it would catch.
 *   - libc/include/config.h: the seam, and why the parsing is apart from the
 *     file it is read from.
 *   - userland/config-check/main.c: the program this runs.
 *
 * This is the arrangement of kernel/test/libc/line.c and of the heap's test
 * before it: the half that is ordinary C is asserted by giving it a buffer,
 * which asserts the code the library ships rather than a description of it, and
 * the half that reaches a descriptor is asserted by a program. Nothing here is
 * a reconstruction of what it tests, and no hook was added to the library to
 * make it possible.
 */

#include <oxys/kernel.h>
#include <oxys/test/verify.h>
#include <oxys/exec/elf.h>
#include <oxys/fs/vfs.h>
#include <oxys/proc/process.h>

#include <config.h>
#include <string.h>

extern const uint8_t KernelProgramConfigCheckBegin[];
extern const uint8_t KernelProgramConfigCheckEnd[];

static bool VerifyConfigSucceeded;

static void VerifyConfigRequire(bool condition, const char *statement)
{
    if (!condition)
    {
        KernelWriteString("  ");
        KernelWriteString(statement);
        KernelWriteString("\n");
        VerifyConfigSucceeded = false;
    }
}

/* One store, reused: it is some kilobytes, and a second would be a second
 * thing to keep in step for no assertion's sake. */
static OxysConfig VerifyConfigStore;

static bool VerifyConfigParse(const char *text)
{
    size_t length = 0U;

    while (text[length] != '\0')
    {
        ++length;
    }

    return OxysConfigParse(&VerifyConfigStore, text, length);
}

/* Whether the key holds exactly that value. */
static bool VerifyConfigHolds(const char *section, size_t occurrence, const char *key,
                             const char *expected)
{
    const char *const value = OxysConfigValue(&VerifyConfigStore, section, occurrence, key);

    return (value != NULL) && (strcmp(value, expected) == 0);
}

/* The whole of the format, upon one text that exercises every rule. */
static void VerifyConfigFormat(void)
{
    const bool whole = VerifyConfigParse(
        "# a comment, and the line below is blank\n"
        "\n"
        "[system]\n"
        "  banner   =   spaces either side are not the value  \n"
        "empty =\n"
        "hashed = \"a value # with a hash\"\n"
        "trailing = kept # and this is not\n"
        "\n"
        "[Service]\n"
        "run = /bin/one\n"
        "restart = YES\n"
        "count = -12\n"
        "\n"
        "[service]\n"
        "run = /bin/two\n"
        "count = 7x\n");

    VerifyConfigRequire(whole, "a text with no fault in it reported one");
    VerifyConfigRequire(OxysConfigFaultCount(&VerifyConfigStore) == 0U,
                        "a text with no fault in it recorded one");

    /* The value is trimmed, and a comment after it is not part of it. */
    VerifyConfigRequire(VerifyConfigHolds("system", 0U, "banner",
                                          "spaces either side are not the value"),
                        "the space either side of a value was not removed");
    VerifyConfigRequire(VerifyConfigHolds("system", 0U, "trailing", "kept"),
                        "a comment after a value was kept as part of it");
    VerifyConfigRequire(VerifyConfigHolds("system", 0U, "empty", ""),
                        "a key with no value is not the empty value");

    /* A quoted value keeps what would otherwise begin a comment. */
    VerifyConfigRequire(VerifyConfigHolds("system", 0U, "hashed", "a value # with a hash"),
                        "a hash within a quoted value was taken for a comment");

    /* A repeated section is a list, and the case of its name is not part of
     * it: `[Service]` and `[service]` are the same section. */
    VerifyConfigRequire(OxysConfigCount(&VerifyConfigStore, "service") == 2U,
                        "two sections of one name were not counted as two");
    VerifyConfigRequire(OxysConfigCount(&VerifyConfigStore, "SERVICE") == 2U,
                        "a section was found only in the case it was written in");
    VerifyConfigRequire(VerifyConfigHolds("service", 0U, "run", "/bin/one") &&
                            VerifyConfigHolds("service", 1U, "run", "/bin/two"),
                        "the two blocks' keys are not kept apart by their occurrence");
    VerifyConfigRequire(OxysConfigCount(&VerifyConfigStore, "absent") == 0U,
                        "a section that is not there was counted");
    VerifyConfigRequire(OxysConfigValue(&VerifyConfigStore, "service", 2U, "run") == NULL,
                        "a block beyond the last was answered");

    /* The conversions, and the fallback where a value is not of the kind. */
    VerifyConfigRequire(OxysConfigNumber(&VerifyConfigStore, "service", 0U, "count", 99) == -12,
                        "a negative number was not read");
    VerifyConfigRequire(OxysConfigNumber(&VerifyConfigStore, "service", 1U, "count", 99) == 99,
                        "a value that is not entirely a number gave its leading digits");
    VerifyConfigRequire(OxysConfigNumber(&VerifyConfigStore, "service", 0U, "absent", 99) == 99,
                        "an absent key did not give the fallback");
    VerifyConfigRequire(OxysConfigBoolean(&VerifyConfigStore, "service", 0U, "restart", false),
                        "`YES` was not read as a truth");
    VerifyConfigRequire(!OxysConfigBoolean(&VerifyConfigStore, "service", 0U, "count", false),
                        "a value that is not a truth did not give the fallback");
}

/* Each fault, one at a time, with its line number. */
static void VerifyConfigFaults(void)
{
    VerifyConfigRequire(!VerifyConfigParse("[system]\nkey\n"),
                        "a line that is not key = value was accepted");
    VerifyConfigRequire((OxysConfigFaultCount(&VerifyConfigStore) == 1U) &&
                            (OxysConfigFaultLine(&VerifyConfigStore, 0U) == 2U) &&
                            (OxysConfigFaultReason(&VerifyConfigStore, 0U) != NULL),
                        "the fault was not recorded against the line it stands upon");

    VerifyConfigRequire(!VerifyConfigParse("key = value\n"),
                        "a setting before any section was accepted");

    VerifyConfigRequire(!VerifyConfigParse("[system\nkey = value\n"),
                        "a section with no closing bracket was accepted");

    VerifyConfigRequire(!VerifyConfigParse("[system]\n = value\n"),
                        "a setting with no key was accepted");

    VerifyConfigRequire(!VerifyConfigParse("[system]\nkey = \"unclosed\n"),
                        "a quoted value with no closing quote was accepted");

    VerifyConfigRequire(!VerifyConfigParse("[system]\nkey = one\nkey = two\n"),
                        "a key given twice within one section was accepted");
    VerifyConfigRequire(VerifyConfigHolds("system", 0U, "key", "one"),
                        "the second of two keys of one name replaced the first");

    /*
     * The parse carries on past a fault, which is the whole point of the
     * format: what follows a bad line is still read.
     */
    VerifyConfigRequire(!VerifyConfigParse("[system]\nbroken\nkey = value\n"),
                        "a text with one bad line reported no fault");
    VerifyConfigRequire(VerifyConfigHolds("system", 0U, "key", "value"),
                        "a bad line stopped the parse; what followed it was lost");

    /* A name beyond the bound is refused rather than cut, and so is a value. */
    {
        static char text[CONFIG_VALUE_MAXIMUM + 64U];
        size_t at = 0U;

        (void)strcpy(text, "[system]\nkey = ");
        at = strlen(text);

        for (size_t index = 0U; index < (CONFIG_VALUE_MAXIMUM + 8U); ++index)
        {
            text[at] = 'v';
            ++at;
        }

        text[at] = '\n';
        text[at + 1U] = '\0';

        VerifyConfigRequire(!VerifyConfigParse(text), "a value beyond the bound was accepted");
        VerifyConfigRequire(OxysConfigValue(&VerifyConfigStore, "system", 0U, "key") == NULL,
                            "a value beyond the bound was kept, cut to the bound");
    }

    /* The store is bounded, and the entries beyond it are refused and counted
     * rather than written past the end. Each setting is named by two letters,
     * so that no two of the sixty-six share a name and the refusal is the
     * bound's doing and not the duplicate rule's. */
    {
        static char text[(CONFIG_ENTRIES_MAXIMUM + 4U) * 8U];
        size_t at = 0U;

        (void)strcpy(text, "[system]\n");
        at = strlen(text);

        for (size_t index = 0U; index < (CONFIG_ENTRIES_MAXIMUM + 2U); ++index)
        {
            text[at] = (char)('a' + (int)(index / 26U));
            text[at + 1U] = (char)('a' + (int)(index % 26U));
            text[at + 2U] = '=';
            text[at + 3U] = '1';
            text[at + 4U] = '\n';
            at += 5U;
        }

        text[at] = '\0';

        VerifyConfigRequire(!VerifyConfigParse(text),
                            "a store filled beyond its bound reported no fault");
        VerifyConfigRequire(VerifyConfigStore.count == CONFIG_ENTRIES_MAXIMUM,
                            "the store took more entries than it holds");
    }

    /* A file wrong in more places than the faults it keeps counts the rest. */
    VerifyConfigRequire(!VerifyConfigParse("[a]\n1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n"),
                        "ten bad lines reported no fault");
    VerifyConfigRequire((OxysConfigFaultCount(&VerifyConfigStore) == CONFIG_FAULTS_MAXIMUM) &&
                            (VerifyConfigStore.faults_dropped ==
                             (10U - CONFIG_FAULTS_MAXIMUM)),
                        "the faults beyond the store's bound were not counted");
}

/* ---------------------------------------------------------- the program */

static Thread *VerifyConfigBoot;

static bool VerifyConfigRun(int64_t *status)
{
    const uint64_t length =
        (uint64_t)(KernelProgramConfigCheckEnd - KernelProgramConfigCheckBegin);
    static const char name[] = "config-check";
    ProcessArguments arguments;
    Process *process;
    Thread *thread;
    ElfImage loaded;
    uint64_t stack;
    const size_t open_before = VfsOpenFileCount();

    arguments.argument_count = 1U;
    arguments.environment_count = 0U;
    arguments.argument[0] = 0U;
    arguments.storage_used = (uint32_t)sizeof name;

    for (size_t index = 0U; index < sizeof name; ++index)
    {
        arguments.storage[index] = name[index];
    }

    process = ProcessCreate(name, NULL);

    if (process == NULL)
    {
        VerifyConfigRequire(false, "a process could not be created for config-check");

        return false;
    }

    if (ElfLoad(&process->space, KernelProgramConfigCheckBegin, length, &loaded) != ELF_OK)
    {
        ProcessDestroy(process);
        VerifyConfigRequire(false, "config-check did not load");

        return false;
    }

    ProcessRecordImage(process, &loaded);
    stack = ProcessCreateUserStack(process, &arguments);

    if (stack == 0U)
    {
        ProcessDestroy(process);
        VerifyConfigRequire(false, "config-check was given no stack");

        return false;
    }

    thread = ThreadCreate(process, loaded.entry, stack);

    if ((thread == NULL) || !ThreadStart(thread))
    {
        ProcessDestroy(process);
        VerifyConfigRequire(false, "config-check could not be started");

        return false;
    }

    VerifyConfigRequire(process->state == PROCESS_EXITED,
                        "config-check's process was not marked as ended");

    *status = process->exit_status;

    ProcessDestroy(process);

    VerifyConfigRequire(VfsOpenFileCount() == open_before,
                        "config-check left an open file behind it");

    return true;
}

void KernelVerifyConfig(void)
{
    int64_t status = 0;

    VerifyConfigSucceeded = true;

    KernelWriteString("Configuration: asserting the format upon text in memory, then running "
                      "config-check.\n");

    VerifyConfigFormat();
    VerifyConfigFaults();

    VerifyConfigBoot = ThreadAdoptCurrent("boot");

    if (VerifyConfigBoot == NULL)
    {
        VerifyConfigRequire(false, "the kernel's own flow of control could not be adopted");
    }
    else
    {
        if (VerifyConfigRun(&status) && (status != 0))
        {
            KernelWriteString("  config-check FAILED: the status was ");
            KernelWriteHexadecimal((uint64_t)status);
            KernelWriteString(" and not zero.\n");
            VerifyConfigSucceeded = false;
        }

        ThreadDestroy(VerifyConfigBoot);
        VerifyConfigBoot = NULL;
    }

    KernelWriteString(VerifyConfigSucceeded
                          ? "Configuration self-test passed: every rule of the format, every "
                            "fault recorded against its line, and the files /etc ships read "
                            "from a program.\n"
                          : "Configuration self-test FAILED.\n");
}
