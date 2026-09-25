/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: libc/config/edit.c
 * Purpose: Changes one setting within the text of a configuration file, in
 *          place, leaving every other line — the comments above all — exactly
 *          as it was written. Added on 2026-09-25 for the settings application
 *          of sub-task 9.8.
 * Key functions: OxysConfigEdit.
 * References:
 *   - libc/include/config.h: the format, and the contract of OxysConfigEdit.
 *   - docs/design/CONFIG.md: why an edit is made to the text and not by
 *     writing the parsed settings back.
 *
 * Why the text, and not the parsed settings written out again.
 *
 *   The shipped files are mostly comments: what each key means, what its
 *   values may be, and why `/bin/sh` is not in the launcher. A file written
 *   back from the parsed settings would keep the settings and lose all of that,
 *   so the first time a person pressed Save in the settings application their
 *   `/etc` would stop explaining itself. Edited as text, the one line that
 *   changes is the only one that does.
 *
 * It runs anywhere, like the parser: no call is made, so the kernel's boot-time
 * self-test asserts it directly (kernel/test/libc/config.c).
 */

#include <config.h>
#include <string.h>

static bool EditIsSpace(char c)
{
    return (c == ' ') || (c == '\t') || (c == '\r');
}

static char EditLower(char c)
{
    return ((c >= 'A') && (c <= 'Z')) ? (char)(c - 'A' + 'a') : c;
}

/* Whether `span` characters at `text` are `name`, without regard to case, as
 * the parser compares sections and keys. */
static bool EditSame(const char *text, size_t span, const char *name)
{
    size_t index = 0U;

    for (; index < span; ++index)
    {
        if ((name[index] == '\0') || (EditLower(text[index]) != EditLower(name[index])))
        {
            return false;
        }
    }

    return name[index] == '\0';
}

/* Replaces `removed` characters at `at` with `inserted` of `added`; false,
 * with nothing changed, where the result would not fit. */
static bool EditSplice(char *text, size_t *length, size_t capacity, size_t at, size_t removed,
                       const char *inserted, size_t added)
{
    const size_t result = *length - removed + added;

    if (result > capacity)
    {
        return false;
    }

    (void)memmove(&text[at + added], &text[at + removed], *length - at - removed);
    (void)memcpy(&text[at], inserted, added);
    *length = result;

    return true;
}

size_t OxysConfigEdit(char *text, size_t length, size_t capacity, const char *section,
                      size_t occurrence, const char *key, const char *value)
{
    char line[CONFIG_KEY_MAXIMUM + CONFIG_VALUE_MAXIMUM + 8U];
    size_t written;
    size_t at = 0U;
    size_t seen = 0U;
    bool inside = false;
    bool found_block = false;
    size_t after_block_setting = 0U;

    if ((text == NULL) || (section == NULL) || (key == NULL) || (length > capacity) ||
        (key[0] == '\0') || (strlen(key) > CONFIG_KEY_MAXIMUM))
    {
        return CONFIG_EDIT_FAILED;
    }

    /* A value that would read back as something else is refused: a newline
     * ends the line, a `#` begins a comment, and space at either end is
     * trimmed by the parser. */
    if (value != NULL)
    {
        const size_t span = strlen(value);

        if ((span > CONFIG_VALUE_MAXIMUM) ||
            ((span > 0U) && (EditIsSpace(value[0]) || EditIsSpace(value[span - 1U]))) ||
            (strchr(value, '\n') != NULL) || (strchr(value, '#') != NULL))
        {
            return CONFIG_EDIT_FAILED;
        }
    }

    while (at < length)
    {
        size_t end = at;
        size_t first = at;

        while ((end < length) && (text[end] != '\n'))
        {
            ++end;
        }

        while ((first < end) && EditIsSpace(text[first]))
        {
            ++first;
        }

        if ((first < end) && (text[first] == '['))
        {
            size_t close = first + 1U;
            size_t name = first + 1U;
            size_t name_end;

            if (inside)
            {
                break;
            }

            while ((close < end) && (text[close] != ']'))
            {
                ++close;
            }

            while ((name < close) && EditIsSpace(text[name]))
            {
                ++name;
            }

            name_end = close;

            while ((name_end > name) && EditIsSpace(text[name_end - 1U]))
            {
                --name_end;
            }

            if (EditSame(&text[name], name_end - name, section))
            {
                if (seen == occurrence)
                {
                    inside = true;
                    found_block = true;
                    after_block_setting = (end < length) ? end + 1U : end;
                }

                ++seen;
            }
        }
        else if (inside && (first < end) && (text[first] != '#'))
        {
            size_t equals = first;
            size_t key_end;

            while ((equals < end) && (text[equals] != '='))
            {
                ++equals;
            }

            key_end = equals;

            while ((key_end > first) && EditIsSpace(text[key_end - 1U]))
            {
                --key_end;
            }

            if ((equals < end) && EditSame(&text[first], key_end - first, key))
            {
                const size_t removed = ((end < length) ? end + 1U : end) - at;

                if (value == NULL)
                {
                    return EditSplice(text, &length, capacity, at, removed, "", 0U)
                               ? length
                               : CONFIG_EDIT_FAILED;
                }

                /* The line up to and including the equals sign and one space
                 * is kept, so that its indentation and alignment are too. */
                written = (size_t)(equals + 1U - at);

                if ((written + 1U + strlen(value) + 1U) > sizeof line)
                {
                    return CONFIG_EDIT_FAILED;
                }

                (void)memcpy(line, &text[at], written);
                line[written++] = ' ';
                (void)memcpy(&line[written], value, strlen(value));
                written += strlen(value);
                line[written++] = '\n';

                return EditSplice(text, &length, capacity, at, removed, line, written)
                           ? length
                           : CONFIG_EDIT_FAILED;
            }

            after_block_setting = (end < length) ? end + 1U : end;
        }

        at = (end < length) ? end + 1U : end;
    }

    if (value == NULL)
    {
        return found_block ? length : CONFIG_EDIT_FAILED;
    }

    /* The key is new to its block: written after the block's last setting, so
     * that the comments trailing the block, which describe what follows it,
     * stay with what they describe. */
    written = strlen(key);
    (void)memcpy(line, key, written);
    (void)memcpy(&line[written], " = ", 3U);
    written += 3U;
    (void)memcpy(&line[written], value, strlen(value));
    written += strlen(value);
    line[written++] = '\n';

    if (found_block)
    {
        if ((length + 1U + written) > capacity)
        {
            return CONFIG_EDIT_FAILED;
        }

        /* A last line with no newline is given one before anything follows. */
        if ((after_block_setting == length) && (length > 0U) && (text[length - 1U] != '\n'))
        {
            if (!EditSplice(text, &length, capacity, length, 0U, "\n", 1U))
            {
                return CONFIG_EDIT_FAILED;
            }

            after_block_setting = length;
        }

        return EditSplice(text, &length, capacity, after_block_setting, 0U, line, written)
                   ? length
                   : CONFIG_EDIT_FAILED;
    }

    /* A block that does not exist is made only as the next of its kind. */
    if (occurrence != seen)
    {
        return CONFIG_EDIT_FAILED;
    }

    {
        char header[CONFIG_SECTION_MAXIMUM + 4U];
        const size_t name = strlen(section);
        size_t made = 0U;

        if ((name > CONFIG_SECTION_MAXIMUM) || ((length + name + 4U + written) > capacity))
        {
            return CONFIG_EDIT_FAILED;
        }

        if ((length > 0U) && (text[length - 1U] != '\n'))
        {
            header[made++] = '\n';
        }

        header[made++] = '[';
        (void)memcpy(&header[made], section, name);
        made += name;
        header[made++] = ']';
        header[made++] = '\n';

        if (!EditSplice(text, &length, capacity, length, 0U, header, made))
        {
            return CONFIG_EDIT_FAILED;
        }
    }

    return EditSplice(text, &length, capacity, length, 0U, line, written) ? length
                                                                           : CONFIG_EDIT_FAILED;
}
