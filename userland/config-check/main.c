/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: userland/config-check/main.c
 * Purpose: Asserts the half of the configuration parser of sub-task 9.4 that
 *          only a program can reach — the file opened, read and parsed — and
 *          that the two files the system ships in /etc say what `init` and the
 *          desktop are built to read. Ends with the number of assertions that
 *          failed.
 * Key functions: main, ConfigRequire.
 * References:
 *   - libc/include/config.h: the seam; the parsing half is asserted by
 *     kernel/test/libc/config.c, which needs no program.
 *   - docs/design/CONFIG.md.
 *
 * Why this asserts the shipped files and not only the reading.
 *
 *   A parser that reads a file correctly and a system whose configuration says
 *   what its programs expect are two different properties, and the second is
 *   the one that breaks silently: a key renamed in `init` and not in
 *   `/etc/system.conf` leaves a machine that boots to a bare screen with every
 *   test still passing. So the shipped files are read here and asserted to
 *   carry the keys the programs look for.
 */

#include <config.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syscall.h>

static int ConfigFailures;

static void ConfigRequire(bool condition, const char *statement)
{
    if (!condition)
    {
        ++ConfigFailures;
        (void)printf("  %s FAILED.\n", statement);
    }
}

static OxysConfig Config;

#define CONFIG_SCRATCH "/tmp-config-check.conf"

/* Writes a file, so that the reading may be asserted against text this program
 * chose rather than against whatever the system happens to ship. */
static bool ConfigWrite(const char *path, const char *text)
{
    const int64_t descriptor =
        OxysOpen(path, SYSCALL_OPEN_WRITE | SYSCALL_OPEN_CREATE | SYSCALL_OPEN_TRUNCATE, 0644U);
    size_t length = 0U;
    int64_t written;

    if (descriptor < 0)
    {
        return false;
    }

    while (text[length] != '\0')
    {
        ++length;
    }

    written = OxysWrite((int)descriptor, text, length);
    (void)OxysClose((int)descriptor);

    return (written == (int64_t)length);
}

int main(void)
{
    (void)printf("config-check: the configuration read from a file, from privilege level 3.\n");

    /* --- A file this program wrote is read back as it was written. --- */

    ConfigRequire(ConfigWrite(CONFIG_SCRATCH,
                              "# a comment\n"
                              "[system]\n"
                              "banner = hello there\n"
                              "\n"
                              "[service]\n"
                              "name = first\n"
                              "run  = /bin/one\n"
                              "\n"
                              "[service]\n"
                              "name = second\n"
                              "run  = /bin/two\n"
                              "restart = never\n"),
                  "the scratch configuration could not be written");

    ConfigRequire(OxysConfigRead(&Config, CONFIG_SCRATCH),
                  "a file this program wrote was not read without fault");
    ConfigRequire(OxysConfigFaultCount(&Config) == 0U, "a file with no faults reported one");

    {
        const char *const banner = OxysConfigValue(&Config, "system", 0U, "banner");

        ConfigRequire((banner != NULL) && (strcmp(banner, "hello there") == 0),
                      "a value with a space within it did not survive the file");
    }

    ConfigRequire(OxysConfigCount(&Config, "service") == 2U,
                  "the two service blocks were not counted as two");

    {
        const char *const first = OxysConfigValue(&Config, "service", 0U, "run");
        const char *const second = OxysConfigValue(&Config, "service", 1U, "run");

        ConfigRequire((first != NULL) && (strcmp(first, "/bin/one") == 0),
                      "the first service's run is not the first block's");
        ConfigRequire((second != NULL) && (strcmp(second, "/bin/two") == 0),
                      "the second service's run is not the second block's");
    }

    ConfigRequire(OxysConfigValue(&Config, "service", 0U, "restart") == NULL,
                  "a key of the second block was found in the first");

    /* --- A file that is not there leaves the configuration empty. --- */

    ConfigRequire(!OxysConfigRead(&Config, "/no-such-configuration.conf"),
                  "a file that does not exist was reported as read");
    ConfigRequire((OxysConfigCount(&Config, "service") == 0U) &&
                      (OxysConfigValue(&Config, "system", 0U, "banner") == NULL),
                  "a failed read left the previous file's settings standing");

    (void)OxysUnlink(CONFIG_SCRATCH);

    /*
     * --- The files the system ships say what its programs read. ---
     *
     * Read from /share/defaults/etc, the copies of etc/ the build stages, and
     * not from /etc: with a persistent /etc attached, /etc holds the person's
     * own files, and a test of those would fail a boot for a person's edit.
     */

    ConfigRequire(OxysConfigRead(&Config, "/share/defaults/etc/system.conf"),
                  "/share/defaults/etc/system.conf could not be read without fault");
    ConfigRequire(OxysConfigCount(&Config, "service") >= 1U,
                  "/share/defaults/etc/system.conf names no service at all");

    {
        bool session = false;

        for (size_t index = 0U; index < OxysConfigCount(&Config, "service"); ++index)
        {
            const char *const run = OxysConfigValue(&Config, "service", index, "run");

            ConfigRequire(run != NULL, "a service in /etc/system.conf has no run");

            /* Since sub-task 9.5 the service `init` starts is the session,
             * which then starts what its launcher offers; it was the window
             * demonstration before. */
            if ((run != NULL) && (strcmp(run, "/bin/session") == 0))
            {
                const char *const needs = OxysConfigValue(&Config, "service", index, "needs");

                session = true;
                ConfigRequire((needs != NULL) && (strcmp(needs, "display") == 0),
                              "the session service does not say it needs a display");
            }
        }

        ConfigRequire(session, "/share/defaults/etc/system.conf does not name /bin/session as a service");
    }

    ConfigRequire(OxysConfigRead(&Config, "/share/defaults/etc/desktop.conf"),
                  "/share/defaults/etc/desktop.conf could not be read without fault");
    ConfigRequire(OxysConfigValue(&Config, "desktop", 0U, "scale") != NULL,
                  "/share/defaults/etc/desktop.conf does not carry the scale the desktop reads");
    ConfigRequire(OxysConfigValue(&Config, "desktop", 0U, "accent") != NULL,
                  "/share/defaults/etc/desktop.conf does not carry the accent the desktop reads");

    /* --- The session's file, of sub-task 9.5, says what the session reads. --- */

    ConfigRequire(OxysConfigRead(&Config, "/share/defaults/etc/session.conf"),
                  "/share/defaults/etc/session.conf could not be read without fault");
    ConfigRequire(OxysConfigValue(&Config, "session", 0U, "scale") != NULL,
                  "/share/defaults/etc/session.conf does not carry the scale the session reads");
    ConfigRequire(OxysConfigCount(&Config, "launch") >= 1U,
                  "/share/defaults/etc/session.conf offers the launcher nothing to start");

    for (size_t index = 0U; index < OxysConfigCount(&Config, "launch"); ++index)
    {
        ConfigRequire(OxysConfigValue(&Config, "launch", index, "run") != NULL,
                      "a launcher entry in /etc/session.conf has no run");
    }

    /*
     * The background, of 2026-09-23, names a file that is there. A path typed
     * wrongly costs the desktop its picture and says so only upon a standard
     * error nobody watches; the picture itself is judged by the kernel's
     * self-test of the image, which reads the same file.
     */
    {
        const char *const background = OxysConfigValue(&Config, "session", 0U, "background");
        const int64_t descriptor =
            (background != NULL) ? OxysOpen(background, SYSCALL_OPEN_READ, 0U) : -1;

        ConfigRequire(descriptor >= 0, "/share/defaults/etc/session.conf names no background, or one that is "
                                       "not upon the ramdisk");

        if (descriptor >= 0)
        {
            (void)OxysClose((int)descriptor);
        }
    }

    /*
     * The shipped copies at /share/defaults/etc, of 2026-09-24: each is there,
     * reads without fault, and the session's offers something to launch. They
     * are what the session falls back to when the file at /etc/session.conf
     * offers nothing, and what a person copies back to undo an edit; a
     * fall-back that was itself missing or empty would be a desktop with an
     * empty launcher again, which is the failure it exists to prevent.
     */
    ConfigRequire(OxysConfigRead(&Config, "/share/defaults/etc/system.conf") &&
                      OxysConfigRead(&Config, "/share/defaults/etc/desktop.conf"),
                  "a shipped copy at /share/defaults/etc could not be read without fault");
    ConfigRequire(OxysConfigRead(&Config, "/share/defaults/etc/session.conf") &&
                      (OxysConfigCount(&Config, "launch") >= 1U),
                  "the shipped /share/defaults/etc/session.conf offers the launcher nothing");

    (void)printf("config-check: %d assertion(s) failed.\n", ConfigFailures);

    return ConfigFailures;
}
