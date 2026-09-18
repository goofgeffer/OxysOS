/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: libc/config/config.c
 * Purpose: Implements the parser of the system configuration format of
 *          sub-task 9.4: the line, the section, the key and the value, the
 *          quoting, the faults that are recorded rather than raised, and the
 *          lookups by which a program asks for a setting.
 * Key functions: OxysConfigParse, OxysConfigCount, OxysConfigValue,
 *          OxysConfigNumber, OxysConfigBoolean, OxysConfigFaultCount,
 *          OxysConfigFaultLine, OxysConfigFaultReason.
 * References:
 *   - libc/include/config.h: the format, and why it is a line at a time.
 *   - docs/design/CONFIG.md: the design and every assertion made upon this.
 *
 * Nothing here calls anything but the string functions of sub-task 7.1. It
 * touches no descriptor, allocates nothing, and reads no global: the file it
 * parses is a buffer somebody else obtained, which is what lets the kernel's
 * boot-time self-test assert the whole of the format upon text composed in
 * memory, before there is a program to read a file.
 *
 * The parse never stops upon a fault, and that is the decision this file is
 * shaped around. A configuration read at boot by the first user process cannot
 * be allowed to fail whole: what is wrong is recorded with its line number and
 * skipped, and the caller decides what it can do without.
 */

#include <config.h>
#include <string.h>

/* The faults, as text, so that every occurrence of one reads the same. */
static const char ConfigFaultNoSection[] = "a setting before any section";
static const char ConfigFaultNoEquals[] = "neither a section, a comment, nor key = value";
static const char ConfigFaultEmptyKey[] = "a setting with no name before the equals sign";
static const char ConfigFaultLongSection[] = "a section name beyond the bound";
static const char ConfigFaultLongKey[] = "a key beyond the bound";
static const char ConfigFaultLongValue[] = "a value beyond the bound";
static const char ConfigFaultUnclosedSection[] = "a section that is not closed by a bracket";
static const char ConfigFaultUnclosedQuote[] = "a quoted value that is not closed";
static const char ConfigFaultDuplicateKey[] = "a key given twice within one section";
static const char ConfigFaultFull[] = "more settings than the store holds";
static const char ConfigFaultTruncated[] = "the file is longer than the parser reads";

static bool ConfigIsSpace(char c)
{
    return (c == ' ') || (c == '\t') || (c == '\r');
}

static char ConfigLower(char c)
{
    return ((c >= 'A') && (c <= 'Z')) ? (char)(c - 'A' + 'a') : c;
}

/* Compares without regard to case: a section, a key and a truth are all
 * written by a person, and `[Service]` meaning something other than
 * `[service]` would be a distinction nobody expects a configuration to make. */
static bool ConfigSame(const char *first, const char *second)
{
    size_t index = 0U;

    while ((first[index] != '\0') && (second[index] != '\0'))
    {
        if (ConfigLower(first[index]) != ConfigLower(second[index]))
        {
            return false;
        }

        ++index;
    }

    return first[index] == second[index];
}

static void ConfigRecordFault(OxysConfig *config, size_t line, const char *reason)
{
    if (config->fault_count < CONFIG_FAULTS_MAXIMUM)
    {
        config->faults[config->fault_count].line = line;
        config->faults[config->fault_count].reason = reason;
        ++config->fault_count;
    }
    else
    {
        ++config->faults_dropped;
    }
}

/* Copies at most `capacity` characters and says whether the whole fitted. */
static bool ConfigCopyBounded(char *destination, size_t capacity, const char *source,
                              size_t length)
{
    if (length > capacity)
    {
        return false;
    }

    for (size_t index = 0U; index < length; ++index)
    {
        destination[index] = source[index];
    }

    destination[length] = '\0';

    return true;
}

/*
 * The span of a value, with the comment removed and the ends trimmed, or a
 * quoted string taken whole.
 *
 * A `#` within a quoted value is a `#` and not a comment, which is the only
 * reason quoting exists at all: a path or a message with a hash in it would
 * otherwise be cut at it with nothing said. Everything else a person writes
 * needs no quotes, and requiring them would make every line noisier for the
 * sake of the one line in a hundred that needs one.
 */
static bool ConfigValueSpan(const char *line, size_t length, size_t *start, size_t *span,
                            const char **fault)
{
    size_t at = 0U;
    size_t end;

    while ((at < length) && ConfigIsSpace(line[at]))
    {
        ++at;
    }

    if ((at < length) && (line[at] == '"'))
    {
        const size_t opening = at + 1U;

        ++at;

        while ((at < length) && (line[at] != '"'))
        {
            ++at;
        }

        if (at >= length)
        {
            *fault = ConfigFaultUnclosedQuote;

            return false;
        }

        *start = opening;
        *span = at - opening;

        return true;
    }

    end = at;

    for (size_t index = at; index < length; ++index)
    {
        if (line[index] == '#')
        {
            break;
        }

        if (!ConfigIsSpace(line[index]))
        {
            end = index + 1U;
        }
    }

    *start = at;
    *span = (end > at) ? (end - at) : 0U;

    return true;
}

/* Whether this section and occurrence already carry the key. */
static bool ConfigHasKey(const OxysConfig *config, const char *section, size_t occurrence,
                         const char *key)
{
    for (size_t index = 0U; index < config->count; ++index)
    {
        const OxysConfigEntry *const entry = &config->entries[index];

        if ((entry->occurrence == occurrence) && ConfigSame(entry->section, section) &&
            ConfigSame(entry->key, key))
        {
            return true;
        }
    }

    return false;
}

/* One line, already stripped of its newline. Returns whether it was read. */
static bool ConfigParseLine(OxysConfig *config, const char *line, size_t length, size_t number,
                            char *section, size_t *occurrence, bool *have_section)
{
    size_t at = 0U;
    size_t equals;
    size_t key_start;
    size_t key_end;
    size_t value_start;
    size_t value_span;
    const char *fault = NULL;

    while ((at < length) && ConfigIsSpace(line[at]))
    {
        ++at;
    }

    if ((at >= length) || (line[at] == '#'))
    {
        return true;
    }

    /* --- A section. --- */

    if (line[at] == '[')
    {
        const size_t name_start = at + 1U;
        size_t name_end = name_start;

        while ((name_end < length) && (line[name_end] != ']'))
        {
            ++name_end;
        }

        if (name_end >= length)
        {
            ConfigRecordFault(config, number, ConfigFaultUnclosedSection);

            return false;
        }

        {
            size_t start = name_start;
            size_t end = name_end;

            while ((start < end) && ConfigIsSpace(line[start]))
            {
                ++start;
            }

            while ((end > start) && ConfigIsSpace(line[end - 1U]))
            {
                --end;
            }

            if (!ConfigCopyBounded(section, CONFIG_SECTION_MAXIMUM, &line[start], end - start))
            {
                ConfigRecordFault(config, number, ConfigFaultLongSection);

                return false;
            }
        }

        /*
         * The occurrence is how many sections of this name have already been
         * opened, which is what turns a repeated section into a list. It is
         * counted from the entries rather than kept in a table of names,
         * because the entries are the only record there is and a second one
         * could disagree with them.
         */
        *occurrence = 0U;

        for (size_t index = 0U; index < config->count; ++index)
        {
            if (ConfigSame(config->entries[index].section, section) &&
                (config->entries[index].occurrence >= *occurrence))
            {
                *occurrence = config->entries[index].occurrence + 1U;
            }
        }

        *have_section = true;

        return true;
    }

    /* --- A setting. --- */

    if (!*have_section)
    {
        ConfigRecordFault(config, number, ConfigFaultNoSection);

        return false;
    }

    equals = length;

    for (size_t index = at; index < length; ++index)
    {
        if (line[index] == '=')
        {
            equals = index;
            break;
        }

        /* A comment cannot begin a setting, and a line with no equals sign
         * before its comment is a line that says nothing. */
        if (line[index] == '#')
        {
            break;
        }
    }

    if (equals >= length)
    {
        ConfigRecordFault(config, number, ConfigFaultNoEquals);

        return false;
    }

    key_start = at;
    key_end = equals;

    while ((key_end > key_start) && ConfigIsSpace(line[key_end - 1U]))
    {
        --key_end;
    }

    if (key_end == key_start)
    {
        ConfigRecordFault(config, number, ConfigFaultEmptyKey);

        return false;
    }

    if (config->count >= CONFIG_ENTRIES_MAXIMUM)
    {
        ConfigRecordFault(config, number, ConfigFaultFull);

        return false;
    }

    {
        OxysConfigEntry *const entry = &config->entries[config->count];

        if (!ConfigCopyBounded(entry->key, CONFIG_KEY_MAXIMUM, &line[key_start],
                               key_end - key_start))
        {
            ConfigRecordFault(config, number, ConfigFaultLongKey);

            return false;
        }

        if (!ConfigValueSpan(&line[equals + 1U], length - equals - 1U, &value_start, &value_span,
                             &fault))
        {
            ConfigRecordFault(config, number, fault);

            return false;
        }

        if (!ConfigCopyBounded(entry->value, CONFIG_VALUE_MAXIMUM, &line[equals + 1U + value_start],
                               value_span))
        {
            ConfigRecordFault(config, number, ConfigFaultLongValue);

            return false;
        }

        if (ConfigHasKey(config, section, *occurrence, entry->key))
        {
            ConfigRecordFault(config, number, ConfigFaultDuplicateKey);

            return false;
        }

        (void)strcpy(entry->section, section);
        entry->occurrence = *occurrence;
        ++config->count;
    }

    return true;
}

bool OxysConfigParse(OxysConfig *config, const char *text, size_t length)
{
    char section[CONFIG_SECTION_MAXIMUM + 1U];
    size_t occurrence = 0U;
    bool have_section = false;
    size_t at = 0U;
    size_t number = 1U;
    bool whole = true;

    if (config == NULL)
    {
        return false;
    }

    config->count = 0U;
    config->fault_count = 0U;
    config->faults_dropped = 0U;
    section[0] = '\0';

    if (text == NULL)
    {
        return false;
    }

    while (at <= length)
    {
        size_t end = at;

        while ((end < length) && (text[end] != '\n'))
        {
            ++end;
        }

        if (!ConfigParseLine(config, &text[at], end - at, number, section, &occurrence,
                             &have_section))
        {
            whole = false;
        }

        if (end >= length)
        {
            break;
        }

        at = end + 1U;
        ++number;
    }

    return whole;
}

/* ------------------------------------------------------------- the lookups */

size_t OxysConfigCount(const OxysConfig *config, const char *section)
{
    size_t highest = 0U;
    bool any = false;

    if ((config == NULL) || (section == NULL))
    {
        return 0U;
    }

    for (size_t index = 0U; index < config->count; ++index)
    {
        if (ConfigSame(config->entries[index].section, section))
        {
            any = true;

            if (config->entries[index].occurrence > highest)
            {
                highest = config->entries[index].occurrence;
            }
        }
    }

    return any ? (highest + 1U) : 0U;
}

const char *OxysConfigValue(const OxysConfig *config, const char *section, size_t occurrence,
                            const char *key)
{
    if ((config == NULL) || (section == NULL) || (key == NULL))
    {
        return NULL;
    }

    for (size_t index = 0U; index < config->count; ++index)
    {
        const OxysConfigEntry *const entry = &config->entries[index];

        if ((entry->occurrence == occurrence) && ConfigSame(entry->section, section) &&
            ConfigSame(entry->key, key))
        {
            return entry->value;
        }
    }

    return NULL;
}

long OxysConfigNumber(const OxysConfig *config, const char *section, size_t occurrence,
                      const char *key, long fallback)
{
    const char *const value = OxysConfigValue(config, section, occurrence, key);
    size_t at = 0U;
    bool negative = false;
    long result = 0;

    if (value == NULL)
    {
        return fallback;
    }

    if ((value[at] == '-') || (value[at] == '+'))
    {
        negative = (value[at] == '-');
        ++at;
    }

    if (value[at] == '\0')
    {
        return fallback;
    }

    /*
     * A value that is not entirely a number gives the fallback rather than what
     * its leading digits happen to say. `scale = 2x` is a mistake, and a parser
     * that read 2 from it would be obeying something nobody wrote.
     */
    while (value[at] != '\0')
    {
        if ((value[at] < '0') || (value[at] > '9'))
        {
            return fallback;
        }

        result = (result * 10) + (value[at] - '0');
        ++at;
    }

    return negative ? -result : result;
}

bool OxysConfigBoolean(const OxysConfig *config, const char *section, size_t occurrence,
                       const char *key, bool fallback)
{
    const char *const value = OxysConfigValue(config, section, occurrence, key);

    if (value == NULL)
    {
        return fallback;
    }

    if (ConfigSame(value, "yes") || ConfigSame(value, "true") || ConfigSame(value, "on"))
    {
        return true;
    }

    if (ConfigSame(value, "no") || ConfigSame(value, "false") || ConfigSame(value, "off"))
    {
        return false;
    }

    return fallback;
}

size_t OxysConfigFaultCount(const OxysConfig *config)
{
    return (config == NULL) ? 0U : config->fault_count;
}

size_t OxysConfigFaultLine(const OxysConfig *config, size_t index)
{
    if ((config == NULL) || (index >= config->fault_count))
    {
        return 0U;
    }

    return config->faults[index].line;
}

const char *OxysConfigFaultReason(const OxysConfig *config, size_t index)
{
    if ((config == NULL) || (index >= config->fault_count))
    {
        return NULL;
    }

    return config->faults[index].reason;
}

/*
 * The one fault this file records that the parse itself does not produce: the
 * reading half of libc/config/system.c calls it where a file was longer than
 * the parser reads. It is declared here and there rather than in the header,
 * no program having any business calling it.
 */
void OxysConfigRecordTruncation(OxysConfig *config, size_t line);

void OxysConfigRecordTruncation(OxysConfig *config, size_t line)
{
    ConfigRecordFault(config, line, ConfigFaultTruncated);
}
