/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: libc/include/config.h
 * Purpose: Declares the system configuration format of sub-task 9.4 and the
 *          parser that reads it — the format `init` takes the list of services
 *          from and the desktop takes its appearance from — and the seam
 *          between the parsing, which runs anywhere, and the file it is read
 *          from, which only a program may open.
 * Key definitions: OxysConfig, OxysConfigEntry, CONFIG_ENTRIES_MAXIMUM,
 *          CONFIG_SECTION_MAXIMUM, CONFIG_KEY_MAXIMUM, CONFIG_VALUE_MAXIMUM,
 *          CONFIG_FAULTS_MAXIMUM, OxysConfigParse, OxysConfigRead,
 *          OxysConfigCount, OxysConfigValue, OxysConfigNumber,
 *          OxysConfigBoolean, OxysConfigFaultCount, OxysConfigFaultLine,
 *          OxysConfigFaultReason.
 * References:
 *   - docs/design/CONFIG.md: the format, every decision in it, and the
 *     assertions made upon this parser.
 *   - libc/include/line.h: the same seam, drawn for the same reason — what can
 *     be asserted by the kernel's boot-time self-test is held apart from what
 *     only a program at privilege level 3 can reach.
 *
 * The format, in one paragraph.
 *
 *   A line at a time. `# …` is a comment and so is the remainder of any line
 *   after an unquoted `#`; `[name]` opens a section; `key = value` sets a key
 *   within the section opened last; blank lines are nothing. A value runs to
 *   the end of the line with the space either side of it removed, or is a
 *   quoted string where its own spaces matter. A section may be repeated, and
 *   that is how a list is written — the services `init` starts are a `[service]`
 *   block each.
 *
 * Why a line at a time, and not a format with brackets around the whole.
 *
 *   A nested format has one point of failure: a brace that is missing makes the
 *   *file* unreadable rather than the line. This file is read by the first user
 *   process, at boot, upon a machine with nobody yet able to ask what went
 *   wrong — so a format in which one bad character costs everything is a format
 *   in which one bad character costs the machine. Here a line that cannot be
 *   read costs that line: it is recorded with its number and its reason, the
 *   parse continues, and the program decides whether it can proceed without it.
 *   Every one of the faults is retrievable, because a setting silently ignored
 *   is a setting somebody will spend an evening looking for.
 *
 * Why sections rather than dotted keys.
 *
 *   `service.run` and `desktop.scale` would need no sections, but a list of
 *   services would then need `service.1.run`, and a person hand-writing a file
 *   would be numbering things a machine could count. A repeated section is the
 *   simplest list a line-oriented format has, and it is the one thing here that
 *   a later settings application — sub-task 9.8 — must be able to add to
 *   without rewriting the file around it.
 *
 * Why the store is fixed and the caller supplies it.
 *
 *   OxysConfig is some kilobytes and is meant to be a static or a file-scope
 *   object, not a stack one. It allocates nothing: a parser that called malloc
 *   would be a parser that can fail for a second reason at the moment the
 *   machine is least able to report it, and the bounds below are large enough
 *   for a configuration a person wrote and small enough to be honest about.
 *   Everything beyond them is refused and recorded as a fault, never truncated
 *   silently.
 */

#ifndef OXYS_LIBC_CONFIG_H
#define OXYS_LIBC_CONFIG_H

#include <stdbool.h>
#include <stddef.h>

/*
 * The bounds. A configuration that exceeds one of these is not truncated: the
 * offending line is refused and recorded, and everything else is still read.
 */
#define CONFIG_ENTRIES_MAXIMUM 64U
#define CONFIG_SECTION_MAXIMUM 31U
#define CONFIG_KEY_MAXIMUM     31U
#define CONFIG_VALUE_MAXIMUM   127U
#define CONFIG_FAULTS_MAXIMUM  8U

/* The longest file this will read, in bytes. */
#define CONFIG_TEXT_MAXIMUM 4096U

/*
 * One setting: the section it stood in, which of the sections of that name it
 * stood in counting from zero, and the key and value themselves.
 *
 * `occurrence` is what makes a repeated section a list. The third `[service]`
 * block's keys all carry occurrence 2, and nothing else distinguishes them —
 * which is why a key written twice within one block is a fault rather than a
 * second element, there being no way to tell it from a mistake.
 */
typedef struct OxysConfigEntry
{
    char section[CONFIG_SECTION_MAXIMUM + 1U];
    size_t occurrence;
    char key[CONFIG_KEY_MAXIMUM + 1U];
    char value[CONFIG_VALUE_MAXIMUM + 1U];
} OxysConfigEntry;

/* A line that could not be read, kept so that it can be reported. */
typedef struct OxysConfigFault
{
    size_t line;
    const char *reason;
} OxysConfigFault;

typedef struct OxysConfig
{
    OxysConfigEntry entries[CONFIG_ENTRIES_MAXIMUM];
    size_t count;

    OxysConfigFault faults[CONFIG_FAULTS_MAXIMUM];
    size_t fault_count;

    /* Faults beyond CONFIG_FAULTS_MAXIMUM are counted and not kept, so that a
     * file which is wrong everywhere still reports how wrong it is. */
    size_t faults_dropped;
} OxysConfig;

/*
 * Parses `text`, which need not be terminated by a newline, into `config`.
 * Everything previously in `config` is discarded. Returns whether the whole of
 * the text was read without a fault; a false return is not a refusal — what
 * could be read is there, and the faults say what could not.
 *
 * This half touches nothing but memory, which is what lets the kernel's
 * boot-time self-test assert the whole of the format before there is a
 * userland to read a file in.
 */
bool OxysConfigParse(OxysConfig *config, const char *text, size_t length);

/*
 * Reads the file at `path` and parses it. Returns false where the file cannot
 * be opened or read, in which case `config` is left empty and a program must
 * decide whether it has a default to fall back upon; a file that was read but
 * has faults returns the same thing OxysConfigParse would.
 *
 * A file longer than CONFIG_TEXT_MAXIMUM is read as far as that and the
 * remainder recorded as a fault, rather than being cut off in silence.
 */
bool OxysConfigRead(OxysConfig *config, const char *path);

/*
 * How many sections of that name the file held — which is how many elements a
 * list has. Zero where there are none, and a caller wanting the one-of-a-kind
 * case reads occurrence 0 and ignores this.
 */
size_t OxysConfigCount(const OxysConfig *config, const char *section);

/*
 * The value of `key` within the `occurrence`th section of that name, or null
 * where there is none. The string belongs to `config` and lives as long as it
 * does; it is not copied.
 */
const char *OxysConfigValue(const OxysConfig *config, const char *section, size_t occurrence,
                            const char *key);

/*
 * The same, as a number or as a truth. Each returns `fallback` where the key is
 * absent **or where its value is not one** — a configuration that says
 * `scale = wide` gets the fallback and not a zero, because a zero is a number
 * somebody might have meant.
 *
 * A truth is `yes`, `no`, `true`, `false`, `on` or `off`, in any case. A number
 * is decimal, optionally signed, and nothing else: there is no radix prefix,
 * nothing here needing one.
 */
long OxysConfigNumber(const OxysConfig *config, const char *section, size_t occurrence,
                      const char *key, long fallback);
bool OxysConfigBoolean(const OxysConfig *config, const char *section, size_t occurrence,
                       const char *key, bool fallback);

/* The faults, for a program that means to report them. */
size_t OxysConfigFaultCount(const OxysConfig *config);
size_t OxysConfigFaultLine(const OxysConfig *config, size_t index);
const char *OxysConfigFaultReason(const OxysConfig *config, size_t index);

#endif /* OXYS_LIBC_CONFIG_H */
