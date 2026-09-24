<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# Test Record

One row per test run, newest first: when, where, what, and the outcome. How the
tests are run is [`TESTING.md`](TESTING.md); what each phase has been observed
to do in each environment is [`STATUS.md`](STATUS.md). The detail of a run —
what was looked at, the messages a negative test produced — is in the commit
message of the change it tested.

A **negative test** inserts a defect on purpose, observes that the tests catch
it, and reverts it; it has a row of its own, since a test never seen to fail has
established nothing. Build numbers named before 2026-09-16 refer to a register
that was cleared that day and renumbered from 1; the date identifies the image.

| Date | Where | What | Result |
| ---- | ----- | ---- | ------ |
| 2026-09-24 | QEMU q35, VirtualBox and Bochs | the shipped defaults, the fall-back, the launcher read at every opening, and `micro` saving all or nothing | Passed, every self-test under each, `config-check` included |
| 2026-09-24 | `make verify` | the defaults, the save and the write-back, the damage applied | Passed, three damages caught in one build and reverted, each target counted to one line before it was altered |
| 2026-09-24 | QEMU q35 | the launcher with an emptied configuration, and the list of windows | Passed |
| 2026-09-24 | `make verify` | a write of nothing, the damage applied | Passed, and the damage was wider than intended, which is recorded |
| 2026-09-24 | QEMU q35 | `micro` saving a file with blank lines, upon a persistent `/etc` | Passed |
| 2026-09-24 | QEMU q35, VirtualBox and Bochs | `clear` in a terminal window | Passed, seventy-eight assertions under QEMU and every self-test under VirtualBox and Bochs, with no failure |
| 2026-09-23 | QEMU q35, VirtualBox and Bochs | the persistent `/etc`, each with a disk labelled `oxys-etc` | Passed, seventy-eight assertions without a disk and seventy-one upon each run with one, and no failure |
| 2026-09-23 | `make verify` | the persistent `/etc`, the damage applied | Passed, three damages caught in one build and reverted |
| 2026-09-23 | QEMU q35, VirtualBox and Bochs | sub-task 9.7, the file manager, the viewer and the clock | Passed, seventy-seven assertions upon each and no failure; `signal-check` and `window-check` report no failure |
| 2026-09-23 | `make verify` | sub-task 9.7, the damage applied | Passed, three damages caught in one build and reverted |
| 2026-09-23 | QEMU q35, VirtualBox and Bochs | the background, minimise and full screen | Passed, seventy-five assertions upon each and no failure; `window-check` and `config-check` report no failure |
| 2026-09-23 | `make verify` | the background and the window states, the damage applied | Passed, three damages caught in one build and reverted |
| 2026-09-23 | QEMU q35, VirtualBox and Bochs | the mark and the icons at the resolution they are shown at | Passed, seventy-four assertions upon each and no failure |
| 2026-09-23 | `make verify` | the mark and the icons, the damage applied | Passed, both damages caught in one build and reverted |
| 2026-09-22 | QEMU q35, VirtualBox and Bochs | the icons of the launcher | Passed, seventy-three assertions upon each and no failure |
| 2026-09-22 | `make verify` | the icons, the damage applied | Passed, two of two, each reverted |
| 2026-09-22 | Bochs | the job-control session, failing, and what it turned out to be | Failed, then passed, and the sequence is the record |
| 2026-09-22 | Bochs | which change was the cure, established by three builds | Passed |
| 2026-09-22 |  | A harness mistake that produced three false failures | Recorded because it cost an hour and would cost it again |
| 2026-09-22 | QEMU q35 | the terminal's colours | Passed |
| 2026-09-22 | QEMU q35 | the terminal emulator, operated | Passed, and this is the sub-task: the launcher opened, `Terminal` chosen |
| 2026-09-22 | QEMU, VirtualBox and Bochs | sub-task 9.6, the assertions | Passed, seventy-two upon each and no failure |
| 2026-09-22 | `make verify` | sub-task 9.6, the negative tests | Passed, four of four, each reverted |
| 2026-09-22 | QEMU q35 | the job-control diagnostic, found by looking | Recorded, not a pass |
| 2026-09-21 | QEMU q35 | the hand-back to the console, damaged both ways | Passed, and both halves were run because the fix is only visible against what it replaced |
| 2026-09-21 | VirtualBox | the serial loopback, failing once in two boots | Recorded rather than passed |
| 2026-09-21 | Bochs and VirtualBox | the image with the hand-back | Passed, seventy-one assertions upon each, the desktop drawn and the prompt reached |
| 2026-09-21 | QEMU q35, VirtualBox and Bochs | the mark in the demonstration's window | Passed, seventy-one assertions upon each and no failure |
| 2026-09-21 | `tools/builds.sh check` | the archive rule, damaged both ways | Passed |
| 2026-09-21 | QEMU q35, VirtualBox and Bochs | the accent drawn from the shared palette | Passed, seventy-one assertions upon each and no failure |
| 2026-09-21 | QEMU q35 | the sentinel, both ways | Passed, and it is recorded because one way would not have established it |
| 2026-09-21 | QEMU q35 | the freeze the project owner reported, reproduced and diagnosed | Reproduced at the first attempt, and it is recorded in full because nothing about it looked like a loop |
| 2026-09-21 | `make verify`, `make clang-check` and `make lint` | the appearance and the terminal fix | Passed. 71 assertions and no `FAILED` under QEMU; every translation unit clean under the second compiler |
| 2026-09-21 | QEMU q35 | the appearance, judged by eye | Passed |
| 2026-09-20 | `make verify`, `make clang-check` and `make lint` | sub-task 9.5, the session | Passed. 71 assertions and no `FAILED` under QEMU |
| 2026-09-20 | QEMU q35 | sub-task 9.5, the desktop operated through the monitor | Passed, and judged by eye |
| 2026-09-20 | VirtualBox 7.2.0, headless | sub-task 9.5, two boots, and the defect the first found | Failed, then passed, and the failure was this sub-task's |
| 2026-09-20 | Bochs 3.1, two processors | sub-task 9.5 | Passed at the first attempt, run alone. 71 assertions, no `FAILED` |
| 2026-09-20 | Negative test | the session, two defects, one at a time | Caught, both |
| 2026-09-18 | `make verify`, `make clang-check` and `make lint` | sub-task 9.4, the configuration and `/etc` | Passed. 71 assertions and no `FAILED` under QEMU |
| 2026-09-18 | QEMU q35 | sub-task 9.4, the configuration obeyed | Passed, and judged by eye |
| 2026-09-18 | VirtualBox 7.2.0, headless | sub-task 9.4 | Passed. 71 assertions and no `FAILED` |
| 2026-09-18 | Bochs 3.1, two processors | sub-task 9.4 | Passed at the first attempt, run alone. 71 assertions, no `FAILED` |
| 2026-09-18 | Negative test | the configuration, two defects, one at a time | Caught, both |
| 2026-09-17 | `make verify`, `make clang-check` and `make lint` | sub-task 9.3, `init` and the shutdown | Passed. 70 assertions and no `FAILED` under QEMU |
| 2026-09-17 | QEMU q35 | sub-task 9.3, operated through the monitor and the serial line | Passed, and judged by eye |
| 2026-09-17 | VirtualBox 7.2.0, headless | sub-task 9.3 | Passed. 70 assertions and no `FAILED`; `init-check` clean |
| 2026-09-17 | Bochs 3.1, two processors | sub-task 9.3, three runs, and the defect the second found | Failed twice, then passed, and the failure was this sub-task's to fix |
| 2026-09-17 | Negative test | `init`, two defects, applied one at a time | Caught, both |
| 2026-09-17 | `make verify`, `make clang-check` and `make lint` | sub-task 9.2, the client protocol | Passed. 69 assertions and no `FAILED` under QEMU |
| 2026-09-17 | QEMU q35 | sub-task 9.2, the demonstration program operated through the monitor | Passed, and judged by eye, by the drive of 9.1 against the program |
| 2026-09-17 | VirtualBox 7.2.0, headless | sub-task 9.2, three boots | Failed once, for a reason outside this sub-task, then passed twice |
| 2026-09-17 | Bochs 3.1, two processors | sub-task 9.2, two runs | The first run failed the same session, for a different reason: the ISO was rebuilt beneath it |
| 2026-09-17 | Negative test | the client protocol, two defects, the second alone | Caught, both — and the second was masked by the first when applied together |
| 2026-09-17 | Bochs 3.1, two processors, `--enable-x86-64 --enable-smp` | sub-task 9.1, the window manager | Passed. 515 lines, 68 assertions passed or sound — the same count as the other two environments — and no |
| 2026-09-17 | VirtualBox 7.2.0, headless, two processors | sub-task 9.1, two boots | Failed once, then passed, and the failure was not this sub-task's |
| 2026-09-17 | QEMU q35, two processors | sub-task 9.1, operated through the monitor | Passed, and judged by eye |
| 2026-09-17 | Negative test | the window manager, two defects in one build | Caught, both |
| 2026-09-17 | `make verify`, `make clang-check` and `make lint` | sub-task 9.1 | Passed. 68 assertions and no `FAILED` under QEMU; every translation unit clean under the second compiler |
| 2026-09-16 |  | `Oxys 1 Alpha`: the release image, build 1 | Passed |
| 2026-09-16 | QEMU q35, the shell over the serial line | six utilities | Passed, after one correction |
| 2026-09-16 | `make verify`, `make clang-check` and `make lint` | sub-task 8.7, job control and signals | Passed, all three. 66 assertions reporting `passed` or `sound` — the sixty-sixth the signal self-test — and |
| 2026-09-16 | `make verify` | sub-task 8.7, three defects the tests found | Three caught, in the change and not the kernel that preceded it |
| 2026-09-16 | QEMU q35, the shell driven over the serial line with | sub-task 8.7 | Passed |
| 2026-09-16 | Boot under VirtualBox 7.2.0r170228 | sub-task 8.7 | Passed. 66 assertions, no verdict of `FAILED` |
| 2026-09-16 | Boot under Bochs 3.1, two processors | sub-task 8.7 | Passed, to the prompt. 66 assertions, no verdict of `FAILED` |
| 2026-09-16 | QEMU, screenshots through the monitor | the `x`, the prompt | Passed |
| 2026-09-16 | QEMU over the serial line, and VirtualBox at the PS/2 | `clear` | Passed |
| 2026-09-16 | QEMU q35, `micro` driven over the serial line | the line editor | Passed |
| 2026-09-16 | `make verify`, `make clang-check` and `make lint` | sub-task 8.6, pipelines | Passed, all three. 65 assertions reporting `passed` or `sound`, and the log's new lines: `Pipes |
| 2026-09-16 | `make verify` | sub-task 8.6, one defect and two negative tests | The defect first: the first program to fork after the child was admitted at the fork faulted in the switch — |
| 2026-09-16 | QEMU q35, the shell driven over the serial line | sub-task 8.6 | Passed |
| 2026-09-16 | Boot under VirtualBox 7.2.0r170228 | sub-task 8.6 | Passed. 65 assertions, no verdict of `FAILED` |
| 2026-09-16 | Boot under Bochs 3.1, two processors | sub-task 8.6 | Passed, to the prompt. 65 assertions, no verdict of `FAILED` |
| 2026-09-15 | `make verify`, `make clang-check` and `make lint` | sub-task 8.5, redirection | Passed, all three. 127 kernel and library translation units and 22 user ones without a diagnostic under the |
| 2026-09-15 | `make verify` | sub-task 8.5, two negative tests | Two caught |
| 2026-09-15 | QEMU q35, the shell driven over the serial line | sub-task 8.5 | Passed |
| 2026-09-15 | Boot under VirtualBox 7.2.0r170228 | sub-task 8.5 | Passed. 65 assertions, no verdict of `FAILED` |
| 2026-09-15 | Boot under Bochs 3.1, two processors | sub-task 8.5 | Passed, to the prompt. 441 lines, 65 assertions, no verdict of `FAILED` |
| 2026-09-15 | `make verify`, `make clang-check` and `make lint` | sub-task 8.4, external program execution | Passed, all three. 127 kernel and library translation units and 19 user ones without a diagnostic under the |
| 2026-09-15 | `make verify` | a defect in the system-call entry path, standing since sub-task 6.7, found by the first program the shell ran | `echo hello world` printed its line and the shell then faulted, at privilege level 3, at RIP 0 |
| 2026-09-15 | `make verify` | sub-task 8.4, two negative tests | Two caught |
| 2026-09-15 | QEMU q35, the shell driven over the serial line | sub-task 8.4 | Passed |
| 2026-09-15 | Boot under VirtualBox 7.2.0r170228 | sub-task 8.4 | Passed. 65 assertions, no verdict of `FAILED` |
| 2026-09-15 | Boot under Bochs 3.1, two processors | sub-task 8.4 | Passed, to the prompt. 443 lines, 65 assertions, no verdict of `FAILED` |
| 2026-09-15 | `make verify`, `make clang-check` and `make lint` | sub-task 8.3, the built-ins and the working directory | Passed, all three. 126 kernel and library translation units and 17 user ones without a diagnostic under the |
| 2026-09-15 | `make verify` | sub-task 8.3, three negative tests | Three caught |
| 2026-09-15 | QEMU q35, the shell driven over the serial line | sub-task 8.3 | Passed |
| 2026-09-15 | Boot under VirtualBox 7.2.0r170228 | sub-task 8.3 | Passed. 65 assertions, no verdict of `FAILED` |
| 2026-09-15 | Boot under Bochs 3.1, two processors | sub-task 8.3 | Passed, to the prompt. 425 lines, 65 assertions, no verdict of `FAILED` |
| 2026-09-15 | `make verify`, `make clang-check` and `make lint` | sub-task 8.2, the tokeniser and the parser | Passed, all three. 123 kernel and library translation units and 13 user ones without a diagnostic under the |
| 2026-09-15 | `make verify` | sub-task 8.2, two negative tests | Two caught |
| 2026-09-15 | QEMU q35, the shell driven over the serial line | sub-task 8.2 | Passed |
| 2026-09-15 | Boot under VirtualBox 7.2.0r170228 | sub-task 8.2 | Passed. 64 assertions, no verdict of `FAILED`, the default entry's screen quiet to the prompt |
| 2026-09-15 | Boot under Bochs 3.1, two processors | sub-task 8.2, and the serial defect it found | Failed once, then passed |
| 2026-09-15 | `make verify`, and a QEMU screendump of the default entry | the quiet boot | Passed |
| 2026-09-15 | `make verify`, `make clang-check` and `make lint` | sub-task 8.1, the terminal, the line editor and the shell | Passed, all three. 120 kernel and library translation units and 11 user ones without a diagnostic under the |
| 2026-09-15 | `make verify` | sub-task 8.1, three negative tests | Three caught, and the second is the finding |
| 2026-09-15 | `make verify` | two tests that stopped, because `stdin` now reads | The first run of the sub-task did not reach the banner |
| 2026-09-15 | QEMU q35, the shell driven over the serial line by a script | sub-task 8.1, the `read` that waits | Passed, and it is the property no self-test reaches |
| 2026-09-15 | Boot under VirtualBox 7.2.0r170228, two processors | sub-task 8.1 | Passed. 63 assertions, no verdict of `FAILED`, `line-check` zero, and the boot ended at the prompt |
| 2026-09-15 | Boot under Bochs 3.1, two processors | sub-task 8.1 | Passed, to the prompt. 386 lines of boot log, 63 assertions, no verdict of `FAILED` |
| 2026-09-15 | ECMA-48 and XTerm Control Sequences, read against the code | the section numbers, checked | Corrected before it was committed |
| 2026-09-14 | `make verify`, `make clang-check` and `make lint` | sub-task 7.7, the initial ramdisk | Passed, all three. 115 kernel and library translation units and 9 user ones without a diagnostic under the |
| 2026-09-14 | `make verify` | the scheduler race recorded on 2026-09-11, closed | Passed, five consecutive runs, with the fixture running upon 2 of 2 processors in every one |
| 2026-09-14 | QEMU q35 with an EXT2 disk, from the `EXT2 write self-test` | `/mnt`, which the ramdisk displaced | Passed, and it is the capability this sub-task nearly removed without saying so |
| 2026-09-14 | Boot under VirtualBox 7.2.0r170228, two processors | sub-task 7.7 | Passed. 61 assertions, no verdict of `FAILED`, and the same root filesystem |
| 2026-09-14 | Boot under Bochs 3.1 | not run, and the reason is recorded rather than the run claimed | Not run |
| 2026-09-14 | QEMU q35 with OVMF | sub-task 7.7, and it is the pre-existing position rather than a regression | Not booted, as before this sub-task |
| 2026-09-14 | `mke2fs` and `cmp -l` | how reproducible the ramdisk image actually is | Measured rather than assumed, and the claim in the `Makefile` was written from the measurement |
| 2026-09-14 | — | `e2fsck -fn` upon `build/initrd.img` | Passed |
| 2026-09-13 | Boot under Bochs 3.1, two processors | sub-task 7.6 | Passed. 60 assertions, no verdict of `FAILED`, two processors online |
| 2026-09-13 | Boot under VirtualBox 7.2.0r170228, two processors | sub-task 7.6 | Passed. 60 assertions, no verdict of `FAILED`, and the same three `-check` programs reporting zero |
| 2026-09-13 | `make verify`, `make clang-check` and `make lint` | sub-task 7.6, the utilities and the six filesystem calls | Passed, all three. 113 kernel and library translation units and 9 user ones without a diagnostic under the |
| 2026-09-13 | `make verify` | sub-task 7.6, thirteen negative tests | Ten caught, three silent, and the three silent ones are the result |
| 2026-09-13 | `make verify` | a defect standing since sub-task 6.11, found by one of 7.6's own programs | `SyscallCopyUserString` returns one refusal for two causes — memory the caller may not read |
| 2026-09-12 | Boot under Bochs 3.1, two processors | build 9, after the header and self-test reorganisation | Passed. 56 assertions reporting `passed` or `sound`, none reporting a failure |
| 2026-09-12 | Boot under VirtualBox 7.2.0r170228, two processors | build 9 | Passed. 56 assertions, none failed, the banner reached |
| 2026-09-12 | Whether VirtualBox can boot an ISO held inside WSL2 | build 9, the same image | It can, and the row above first said otherwise |
| 2026-09-12 | `make verify` and `make clang-check` | after the header corpus and the self-tests were grouped, and the two fault-screen menu entries removed | Passed, both, and identical to the run before them |
| 2026-09-11 | `make verify` | an intermittent failure in the scheduler's self-test, which is not this sub-task's | Observed once in twelve runs, and recorded because it was observed rather than because it was caused |
| 2026-09-11 | Boot under Bochs 3.1, two processors | build 5, the heap image | Passed: fifty-six assertions, no verdict of `FAILED`, both processors online |
| 2026-09-11 | Boot under VirtualBox 7.2.0r170228, two processors | build 5, the heap image | Passed: fifty-six assertions, no verdict of `FAILED`, `Heap self-test passed.` |
| 2026-09-11 | The same source built by the second compiler and booted | build 6 | Passed |
| 2026-09-11 | `make verify` | sub-task 7.3, fourteen negative tests | Twelve caught, two not, and the two that were not are the result |
| 2026-09-11 | `make verify` and `make clang-check` | sub-task 7.3, the heap and the break | Passed, both. 106 translation units without a diagnostic |
| 2026-09-11 | `make verify` | sub-task 7.2, seven negative tests | Five caught at once, two did not, and the two that did not are the result |
| 2026-09-11 | `make verify` and `make clang-check` | sub-task 7.2, the system-call wrappers | Passed, both. 102 translation units without a diagnostic |
| 2026-09-11 |  | The same source built by the second compiler and booted | Passed |
| 2026-09-11 | Boot under VirtualBox 7.2.0r170228, two processors | build 1 | Passed, and it contradicted the corpus. 294 lines of serial log, fifty-five assertions |
| 2026-09-11 | Boot under Bochs 3.1, two processors | build 1, and the first Bochs run in this project's history | Passed: fifty-five assertions, no verdict of `FAILED`, two processors online |
| 2026-09-11 | `make run-uefi` | build 1 | Failed, as designed and as [`TESTING.md`](TESTING.md), Section 3, says it must: `BdsDxe |
| 2026-09-11 | `make verify` | the scheduler's intermittency, diagnosed and repaired | The failure recorded on 2026-09-10 reproduced upon the unmodified tree at `0439d05` — `not every admitted |
| 2026-09-10 | `make verify` and `make clang-check` | sub-task 7.1, the C library's string functions | Passed, both |
| 2026-09-10 | `make verify` | sub-task 7.1, five negative tests | Four caught, one did not, and the one that did not is the result |
| 2026-09-10 | `make verify` | the scheduler self-test observed to be intermittent, upon the unmodified tree | Failed once in four runs of `make verify` upon `d7db6c9`, with no change of any kind applied |
| 2026-09-10 | `make verify` and `make clang-check` | sub-task 6.15, the scheduler | Passed, both |
| 2026-09-10 | `make verify` | the three failures the sub-task passed through, each recorded because two of them passed a test | First, `SchedulerInitialise` returned false and the kernel ran unpre-empted |
| 2026-09-10 | `make verify` | sub-task 6.14, two negative tests, by hand | One informative, one instructive |
| 2026-09-10 | `make verify` and `make clang-check` | after the documentation and the header corrections, which closes sub-task 6.14 | Passed, both |
| 2026-09-10 | `make verify` | sub-task 6.14, the application processors, which is the sub-task | Passed |
| 2026-09-10 | `make verify` | the defect the bring-up first showed, before the sub-task closed | Failed, and usefully |
| 2026-09-09 | `make verify` and `make clang-check` | after the concurrency notes were completed, which closes sub-task 6.13 | Passed, both, and identical to the run before them: ninety translation units without a diagnostic |
| 2026-09-09 | `make verify` | sub-task 6.13, the shootdown, which is the sub-task | Passed, and it is the one assertion of this sub-task that establishes behaviour rather than state |
| 2026-09-09 | `make verify` | sub-task 6.13, the per-processor area and the lock | Passed |
| 2026-09-09 | `make verify` | sub-task 6.13, the interprocessor interrupt | Passed |
| 2026-09-09 | `make verify` | sub-task 6.13, the two panics, by hand | Passed, both, and recorded field by field in [`TESTING-SYSTEM.md`](TESTING-SYSTEM.md), Section 9.3 |
| 2026-09-09 | `make verify` | sub-task 6.13, two latent defects found by the writing | Recorded because neither was a defect in the new code |
| 2026-09-09 | `make clang-check` | after sub-task 6.13 | Passed |
| 2026-09-08 | `make verify` | sub-task 6.12, the ACPI tables | Passed |
| 2026-09-08 | `make verify` | the Local APIC and the I/O APIC | Passed |
| 2026-09-08 | `make verify` | the retirement of the 8259A, which is the sub-task | Passed |
| 2026-09-08 | QEMU | the interrupt source override, read in the log | The three lines worth reading are `ISA request 0 is carried by global interrupt 2` |
| 2026-09-08 | `make verify` | sub-task 6.12, the negative tests | Passed, six of six attempted, and recorded field by field in Section 23.4 |
| 2026-09-08 | `make clang-check` | after sub-task 6.12 | Passed |
| 2026-09-07 | `make verify` | after the division of `kernel/fs/ext2.c` | Passed, and this is the test the division rested upon |
| 2026-09-07 | `make verify` | the ELF loader's two new refusals | Passed, after failing once |
| 2026-09-07 | `make verify` | the three corrected reports | Passed |
| 2026-09-07 | `make verify` | after the division of the virtual filesystem layer, the EXT2 self-test and the ATA driver | Passed, and identical to the run before them |
| 2026-09-07 | `make verify` | sub-task 6.11, the fork alone | Passed |
| 2026-09-07 | `make verify` | all four calls, by a program | Passed, and this is the sub-task |
| 2026-09-07 | QEMU | the second child's fault trace, read field by field | Passed |
| 2026-09-07 | `make verify` | sub-task 6.11, the negative tests | Passed, five of five, and two of them found defects that predated the sub-task |
| 2026-09-06 | `make verify` | the channel addressing, upon composed headers | Passed |
| 2026-09-06 | `make verify` | the storage that is not of the storage class | Passed |
| 2026-09-06 | — | QEMU q35 with `sata=off`, an `sdhci-pci` and the kernel booted from a USB drive | Passed; this is the machine that reported the fault, reproduced |
| 2026-09-06 | — | QEMU q35 with `-device qemu-xhci`, the ordinary ISO | Passed; with the board's own AHCI controller present, the report names it |
| 2026-09-06 | `make verify` | the disk diagnosis, the negative tests | Passed, all three, each reverted afterwards |
| 2026-09-06 | — | VirtualBox 7, headless, 512 MiB, legacy BIOS, screenshot | Passed; both new self-tests report soundly and the addressing change alters nothing upon a machine that was |
| 2026-09-06 | `make verify` | the backspace across a line separator | Passed |
| 2026-09-06 | `make verify` | the backspace, the negative tests | Passed, two of three, and the third is recorded as a gap rather than a pass |
| 2026-09-06 | QEMU q35, the echo loop driven through a serial socket | the fault as reported | Passed |
| 2026-09-06 | QEMU q35, the echo loop | the erase limit across a crossing | Passed |
| 2026-09-06 | — | QEMU q35, the echo loop after a hundred and thirty line feeds | Passed; this is the test of the row lengths moving with a scroll, which the boot-time self-test cannot reach |
| 2026-09-06 | VirtualBox 7, headless, 640 by 480 | through the keyboard itself | Passed |
| 2026-09-06 | — | VirtualBox 7, the same session, after seventy further Enters | Passed, and it is the second machine to confirm the row lengths move with a scroll |
| 2026-09-06 | `make verify` | sub-task 4.7, the AHCI decisions | Passed |
| 2026-09-06 | QEMU q35 | the AHCI adaptor, with no disk attached | Passed, and it exercises more than it appears to |
| 2026-09-06 | — | QEMU q35 with a 256 GiB sparse disk upon the AHCI controller | Passed |
| 2026-09-06 | — | QEMU q35 with a seeded EXT2 volume upon the AHCI disk | Passed, and this is the corroboration from outside |
| 2026-09-06 | QEMU q35 from the `disk write self-test` entry | the AHCI write path | Passed |
| 2026-09-06 | `make verify` | the AHCI driver, the negative tests | Passed, three of five, and the other two are recorded rather than claimed |
| 2026-09-06 | — | VirtualBox 7, headless, 640 by 480, an Intel ICH8-M AHCI controller | Passed, upon a second silicon design rather than a second copy of the first |
| 2026-09-06 | `make verify` | sub-task 4.8, the capacity and command arithmetic | Passed |
| 2026-09-06 | — | QEMU q35 with a 16 MiB image upon an `sdhci-pci` controller | Passed |
| 2026-09-06 | — | QEMU q35 with a 4 GiB image upon the same controller | Passed, and it reaches the other encoding by changing nothing but the size of the image: `SDHC or SDXC card |
| 2026-09-06 | QEMU q35 from the `disk write self-test` entry | the SD write path | Passed |
| 2026-09-06 | `make verify` | the SD driver, the negative tests | Passed, all three |
| 2026-09-06 | VirtualBox 7, headless | sub-task 4.8 | Not applicable, and recorded as such |
| 2026-09-06 | VirtualBox 7, headless | the serial port, which had never worked there | Fixed and confirmed |
| 2026-09-06 | VirtualBox 7 | the serial loopback self-test, five consecutive runs | Passed, five of five, with identical log sizes and no failure line |
| 2026-09-06 | `make verify` | the serial corrections upon QEMU | Passed, unchanged: `Serial self-test passed.`, 5037 characters transmitted, `realised 115200 baud` |
| 2026-09-06 | `make verify` | sub-task 6.6, the clip stack and the blend | Passed |
| 2026-09-06 | `make verify` | the compositor's damage and layer table | Passed |
| 2026-09-06 | QEMU q35, screendump | the pointer over the console | Passed, and this is what the sub-task is for |
| 2026-09-06 | — | QEMU q35, the pointer walked twelve steps from the monitor | Passed |
| 2026-09-06 | QEMU q35 | the compositor, the negative tests | Passed, all four, and two of them only because somebody looked |
| 2026-09-06 | — | QEMU q35 from the `raise a genuine page fault` entry | Passed |
| 2026-09-06 | `make verify` | sub-task 6.7, the dispatch and the validation | Passed |
| 2026-09-06 | `make verify` | the paging defect the self-test found | Fixed and asserted |
| 2026-09-06 | `make verify` | the system call, the negative tests | Passed, three of three, and one of them corrected the test rather than the code |
| 2026-09-06 | `make verify` | the assertion that was lost | Recorded rather than disguised |
| 2026-09-06 | `make verify` | sub-task 6.8, the ELF64 loader | Passed |
| 2026-09-06 | `make verify` | the loader, what it placed | Passed, and this is what the rest is built toward |
| 2026-09-06 | `make verify` | the loader, the negative tests | Passed, three of three, and one only after the test was strengthened |
| 2026-09-06 | `make verify` | sub-task 6.9, the process and thread structures | Passed |
| 2026-09-06 | `make verify` | the guard page, and `rsp0` | Passed, and both were promises `docs/design/PRIVILEGE.md` made to this sub-task |
| 2026-09-06 | `make verify` | the balance of the allocations | Passed |
| 2026-09-06 | `make verify` | the process structures, the negative tests | Passed, all three |
| 2026-09-06 | `make verify` | sub-task 6.10, the context switch | Passed |
| 2026-09-06 | `make verify` | a program at privilege level 3 | Passed, and this is the sub-task |
| 2026-09-06 | QEMU | the fault trace, read field by field | Passed |
| 2026-09-06 | `make verify` | sub-task 6.10, the negative tests | Passed, three of five, and both of the others are recorded rather than claimed |
| 2026-09-04 | `make verify` | sub-task 6.5, the mouse decoder | Passed |
| 2026-09-04 | `make verify` | the mouse decoder, the negative tests | Passed, both |
| 2026-09-04 | `make verify` | the pointer, upon a surface composed in memory | Passed |
| 2026-09-04 | `make verify` | the pointer, the negative tests | Passed, both, and the first of them found a real weakness in the code rather than in the test |
| 2026-09-04 | QEMU screendump 1280 by 800 | the pointer, a person's judgement | Passed |
| 2026-09-04 | — | VirtualBox 640 by 480, headless, screenshot | Passed |
| 2026-09-04 | `make verify` | the 8042 given one owner | Passed, with the keyboard's own self-test unchanged: the same decoder assertions, the same modifiers |
| 2026-09-04 | `make verify` | the disposition of every exception | Passed, and it corrects a real fault rather than adding a feature |
| 2026-09-04 | `make verify` | the disposition, the negative test | Passed |
| 2026-09-04 | `make verify` | the fault screen table, narrowed | Passed |
| 2026-09-04 | QEMU screendump | a fault the font found | The general screen rendered with three replacement boxes in the middle of a sentence |
| 2026-09-04 | QEMU screendump | the narrowed screens | Passed |
| 2026-09-04 | `make verify` | the drawing optimisation | Passed |
| 2026-09-04 | `make verify` | the drawing optimisation, the negative tests | Passed, both |
| 2026-09-04 | `make verify` | the fault screen table | Passed |
| 2026-09-04 | QEMU screendump | the fault screens, a person's judgement | Passed at 1280 by 800 for the page fault, general protection fault, double fault |
| 2026-09-04 | QEMU | the fault screens, the wiring, end to end | Passed |
| 2026-09-04 | QEMU screendump | a fault found by looking, not by asserting | The first correct-looking screen was displayed wrongly |
| 2026-09-04 | VirtualBox 7, headless, 512 MiB, legacy BIOS | the fault screens at 640 by 480 | Passed |
| 2026-09-04 | `make verify` | sub-task 6.4, the font and the console | Passed; twenty-seven assertions |
| 2026-09-04 | QEMU screendump | sub-task 6.4, the half a person judges | Passed at 1280 by 800, 160 by 100 characters |
| 2026-09-04 | `make verify` | sub-task 6.4, the negative test | Passed; glyph `0x4F` (`'O'`) was given the bytes of glyph `0x30` (`'0'`) |
| 2026-09-04 | VirtualBox 7, headless, 512 MiB, legacy BIOS | sub-task 6.4 | Passed, and it is the result this sub-task was for |
| 2026-09-04 | QEMU screendump | sub-tasks 6.2 and 6.3, the figures, through the new menu entry | Passed |
| 2026-09-03 | `make verify` | sub-task 6.3, the drawing primitives | Passed; sixty-five assertions against a surface composed in memory |
| 2026-09-03 | `make verify` | sub-task 6.3, the assertion that clipping does not move a line | Passed |
| 2026-09-03 | QEMU screendump | sub-task 6.3, the figure a person judges | Passed at 1280 by 800 |
| 2026-09-03 | `make verify` | sub-task 6.3, the negative test | Passed; with rows addressed by the surface's width instead of its pitch |
| 2026-09-03 | `make verify` | sub-task 6.2, the framebuffer self-test | Passed |
| 2026-09-03 | QEMU screendump | sub-task 6.2, the half a kernel cannot assert | Passed; the captured image is 1280 by 800 |
| 2026-09-03 | `make verify` | sub-task 6.2, the negative test | Passed; with the page attribute table given write-back instead of write-combining |
| 2026-09-03 | VirtualBox 7, headless, 512 MiB, legacy BIOS | sub-task 6.2 | Passed, upon a hypervisor whose boot loader chose a different mode entirely: 640 by 480 |
| 2026-09-03 | VirtualBox | the diagnostic path, after sub-task 6.2 | Nothing readable remains |
| 2026-09-03 | GRUB `gfxpayload` | a claim disproved rather than a test passed | GRUB 2.12 ignores `gfxpayload` for a multiboot2 image |
| 2026-09-03 | `make verify` | the display self-test, after sub-task 6.2 | Skipped, as intended, the adapter being in a graphics mode |
| 2026-09-03 | VirtualBox 7, headless, 512 MiB, two processors, legacy BIOS | sub-task 6.1 | Passed; the machine reached `Phase 6 initialisation complete` and the echo loop |
| 2026-09-03 | VirtualBox | the serial channel | Not available; the kernel reports `Serial adapter: absent |
| 2026-09-03 | `make verify` | sub-task 6.1, the privilege self-test | Passed; forty-eight assertions |
| 2026-09-03 | `make verify` | sub-task 6.1, the interrupt stack table exercised | Passed; vector 200 raised without an interrupt stack table entry built its trap frame outside the |
| 2026-09-03 | `make verify` | sub-task 6.1, the transition exercised | Passed; `SYSCALL` executed from privilege level 0 reached the entry point `IA32_LSTAR` names |
| 2026-09-03 | `make verify` | sub-task 6.1, the negative test | Passed; with `RFLAGS_INTERRUPT_ENABLE` removed from `SYSCALL_FLAG_MASK` |
| 2026-09-03 | `make all` | build with the full diagnostic regime, after sub-task 6.1 | Passed; no diagnostics, including from the `_Static_assert` upon the size of `TaskStateSegment` |
| 2026-09-02 | `make verify` | sub-task 5.8, the filesystem self-test | Passed; some ninety assertions upon two composed volumes |
| 2026-09-02 | `make verify` | sub-task 5.8, the mount | Passed; a second volume, identical to the first but for the owner of one file |
| 2026-09-02 | `make verify` | sub-task 5.8, the mark a mount leaves | Passed; the state read back out of the medium after a writable mount has the clean bit clear and the error |
| 2026-09-02 | QEMU i440fx with an image from `mke2fs -d` | the root mount, read-only | Passed; the volume mounted at `/` as `ata0`, block size 1024, read-only, and its root listed as inodes 2, 2 |
| 2026-09-02 | QEMU i440fx with `ext2-write-test` | the root mount, writable, never withdrawn | Passed; `dumpe2fs -h` afterwards reported `not clean` — and not "with errors" — with `Mount count: 1` |
| 2026-09-02 | QEMU i440fx with `ext2-write-test` | the write probe and the clean withdrawal | Passed; 5000 bytes were written to `/oxys-write-test` through a descriptor and read back identically through |
| 2026-09-02 | QEMU i440fx with a 4096-byte-block image | the same, at the other block size | Passed; mounted as block 4096, the root listed identically |
| 2026-09-02 | `e2fsck -fn` over a volume the layer had created and | a defect found | Initially failed; `e2fsck` reported inodes 16 and 17 as "part of the orphaned inode list" |
| 2026-09-02 | `make verify` | sub-task 5.4, the directory self-test | Passed; the root of a composed volume yields `.`, `..`, `file` and `sub` with the inode number, file type |
| 2026-09-02 | QEMU i440fx with an image from `mke2fs -d` | a root directory of 46 entries in two blocks | Passed; the kernel listed the entries with the inode numbers |
| 2026-09-02 | — | QEMU i440fx, the probe path set to `//sub/deeper/../deeper/buried` for one boot | Passed; resolved to inode 56, a regular file of 2 bytes |
| 2026-09-02 | QEMU i440fx with an image from `mke2fs -d` | a root directory of 9000 entries in 530 blocks | Passed; the kernel counted 9003 entries, which `debugfs -R "ls -l /"` confirms — the 9000 files, `.` |
| 2026-09-02 | `make verify` | the allocator self-test, after the integer-wrap corrections | Passed; a page count one beyond the arena's capacity |
| 2026-09-02 | `make verify` | the allocator self-test, after the full-range check upon `KernelPagesFree` | Passed; a legitimate four-page range is allocated, written, read back, released |
| 2026-09-02 | `make verify` | sub-task 5.5, the file self-test | Passed; a 1500-byte composed file reads exactly across the boundary between its two blocks |
| 2026-09-02 | QEMU i440fx with an image from `mke2fs -d` | reading a regular file | Passed; `/content.txt` resolved to inode 12 of 22 bytes and its first sixteen bytes read `0x4F 0x78 0x79 0x73 |
| 2026-09-02 | QEMU i440fx | both forms of symbolic link upon a real volume | Passed; `/shortlink` reported inode 17, 4 bytes, a target held within its inode reading `deep` |
| 2026-09-02 | — | QEMU i440fx, the probe path set to `/shortlink/deeper/buried.txt` for one boot | Passed; resolved to inode 15, a regular file of 7 bytes, reading `0x62 0x75 0x72 0x69 0x65 0x64 0xA` |
| 2026-09-02 | `make verify` | sub-task 5.6, the write self-test | Passed; both bitmaps report the volume as composed |
| 2026-09-02 | QEMU i440fx from the `EXT2 write self-test` GRUB entry | writing a real volume | Passed; the kernel emptied `/oxys-write-test` and wrote 8192 bytes into it, reporting inode 13, 16 sectors |
| 2026-09-02 | — | `e2fsck -fn` upon the volume the kernel had written | Passed with no errors through all five passes, including Pass 5 |
| 2026-09-02 | `debugfs dump` upon the same volume | the contents written | Passed; all 8192 bytes match the expected pattern, each byte derived from its own offset |
| 2026-09-02 | `make verify` | sub-task 5.7, the name self-test | Passed; an insertion yields exactly one entry more when the whole directory is traversed and the name |
| 2026-09-02 | QEMU i440fx from the `EXT2 write self-test` GRUB entry | creating and removing names upon a real volume | Passed; within one boot the kernel created `/oxys-made` (inode 14), created `within` (inode 15) inside it |
| 2026-09-02 | — | `e2fsck -fn` upon the volume after creation and removal | Passed with no errors through all five passes |
| 2026-09-01 | `make verify` | sub-task 4.2, the display self-test | Passed; the adapter reports its colour configuration at `0x03D4`, blinking is disabled |
| 2026-09-01 | QEMU `-serial stdio` | the backspace across a row boundary | Passed; `ab`, a line feed, `cd` and four backspaces erased both rows' characters in turn |
| 2026-09-01 | QEMU `sendkey` | the backspace across a row boundary | Passed; the same sequence delivered as scan codes produced an identical echo |
| 2026-09-01 | QEMU `-serial stdio` and `sendkey` | the backspace consumes the separator alone | Passed; after `ab`, a line feed and `cd` |
| 2026-09-01 | `make verify` | sub-task 4.3, the bus self-test | Passed; mechanism one answers its own probe, an absent function reads as all ones |
| 2026-09-01 | `make verify` | sub-task 4.4, the disk self-test upon q35 | Passed; no ATA device answers upon that board, the self-test reports as much and asserts nothing |
| 2026-09-01 | QEMU i440fx with a 256 GiB sparse image | the disk self-test | Passed; a disk of 536870912 sectors with 48-bit addressing was identified upon the primary master and the |
| 2026-09-01 | QEMU i440fx | the seeded content, confirmed from outside | Passed; sectors 0, 1 and 0x10000001 of the image were seeded upon the host and each was read back by the |
| 2026-09-01 | QEMU i440fx with `disk-write-test` | the write path | Passed; a pattern written to the final sector read back byte for byte |
| 2026-09-01 | `make verify` | sub-task 4.5, the block self-test | Passed; a device of memory is registered and withdrawn |
| 2026-09-01 | QEMU i440fx | the ATA disk presented through the block layer | Passed; the disk of the primary master registered as `ata0`, 536870912 blocks of 512 bytes, writable |
| 2026-09-01 | `make verify` | sub-task 4.6, the buffer self-test | Passed; a block held is not read again, a modified block does not reach the device until it is written back |
| 2026-09-01 | `make verify` | sub-task 5.1, the volume self-test | Passed; every field of a composed superblock is read from the offset the format defines |
| 2026-09-01 | — | QEMU q35 and QEMU i440fx with a disk, after the `LOAD` segments were separated by permission | Passed; every self-test reported as before and the kernel reached its completion banner upon both boards |
| 2026-09-01 | QEMU i440fx with an image from `mke2fs -d` | a root directory of 900 entries in 40 blocks | Passed; the root inode reported mode `0x41ED`, 40960 bytes, 3 links and 82 sectors, and blocks 580, 616, 640 |
| 2026-09-01 | QEMU i440fx with an image from `mke2fs -d` | a root directory of 9000 entries in 500 blocks | Passed; the root reported 512000 bytes and 1006 sectors, the prefix 772, 786-796, 798, and `[268]=1056` |
| 2026-09-01 | QEMU i440fx with an image from `mke2fs` | 1024-byte blocks, sub-task 5.2 | Passed; group 0 reported block bitmap at 66, inode bitmap at 67, inode table at 68, 7599 free blocks |
| 2026-09-01 | QEMU i440fx with an image from `mke2fs` | 4096-byte blocks, sub-task 5.2 | Passed; one group with bitmaps at blocks 6 and 7, inode table at block 8 |
| 2026-09-01 | QEMU i440fx with an image from `mke2fs` | 1024-byte blocks | Passed; the kernel reported 16384 blocks of 1024 bytes with 15211 free |
| 2026-09-01 | QEMU i440fx with an image from `mke2fs` | 4096-byte blocks | Passed; 20000 blocks of 4096 bytes, one group |
| 2026-09-01 | — | QEMU i440fx with a disk holding no filesystem | Passed; refused with *the volume bears no EXT2 magic number*, and the kernel proceeded |
| 2026-08-31 | `make verify` | sub-task 3.5, the interrupt controller self-test | Passed; the controllers are remapped, the mask is honoured, a spurious request is recognised |
| 2026-08-31 | `make verify` | sub-task 3.6, the interval timer self-test | Passed; divisor 1193 realising 1000.152 Hz |
| 2026-08-31 | `make verify` | sub-task 3.7, the keyboard self-test | Passed; the controller and port self-tests pass, and the decoder |
| 2026-08-31 | QEMU `sendkey` | the keyboard interrupt path, end to end | Passed; the keystrokes `h e l l o spc o x y s` were echoed upon the serial port as `hello oxys` |
| 2026-08-31 | `make verify` | the display self-test | Passed; the backspace, the tabulation |
| 2026-08-31 | `make verify` | sub-task 4.1, the serial self-test | Passed; the request line is claimed and unmasked, a sequence returns unaltered through local loopback |
| 2026-08-31 | QEMU `-serial stdio` | the serial receive path, end to end | Passed; `serial-in-works`, typed upon the host, was received by interrupt and echoed back |
| 2026-08-31 | QEMU `sendkey` | the backspace, end to end | Passed; `o x y s spc b a d` followed by three backspaces and `g o o d` produced `oxys bad` then the erasing |
| 2026-08-30 | `make all` | build with `-Wall -Wextra -Werror` | Passed; no diagnostics |
| 2026-08-30 | — | `grub-file --is-x86-multiboot2` | Passed; the image is Multiboot2 compliant |
| 2026-08-30 | `make iso` | ISO generation | Passed |
| 2026-08-30 | `make verify` | QEMU boot and serial assertion | Passed |
| 2026-08-30 | QEMU screendump | VGA text-mode rendering | Passed; the banner reads `Oxys-OS` |
