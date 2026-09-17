/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/include/oxys/gfx/client.h
 * Purpose: Declares the client side of the window manager, sub-task 9.2: the
 *          five calls by which a process creates, moves, draws upon, destroys
 *          and receives events upon a window, the ownership that binds a window
 *          to the process that made it, and the wait a program sleeps in until
 *          an event arrives.
 * Key definitions: WindowClientCreate, WindowClientDestroy, WindowClientMove,
 *          WindowClientBlit, WindowClientEvent, WindowClientScreen,
 *          WindowClientReleaseProcess,
 *          WindowClientWakeAll, WindowClientReport.
 * References:
 *   - kernel/abi/oxys/syscall_abi.h: the five calls, their arguments and their
 *     results, which are the whole of what a program is entitled to know.
 *   - docs/design/WINDOWS.md, Section 10: the protocol, and what the first
 *     real client decided about the interface of Section 2.
 *
 * What crosses the boundary, and what does not.
 *
 *   A surface does not. GraphicsSurface describes kernel memory, and a program
 *   cannot be handed one; what a program can be handed is the promise that a
 *   rectangle of pixels it supplies, in one format whatever the screen's, will
 *   arrive in its window. So the protocol is a copy — window_blit — and the
 *   surface abstraction stays where it was, on this side, where the copy is
 *   made with the same primitive the console draws with. Section 10 of the
 *   design document records that as the answer to the question sub-task 6.6
 *   left open, and what a shared mapping would have cost instead.
 *
 *   A queue does not cross either. The manager keeps it; window_event takes
 *   one entry across, converted field by field into the structure the ABI
 *   declares, so that the manager's event may change without the ABI moving.
 *
 * Every call takes the arguments as the entry path hands them — raw registers
 * — and returns what the call returns, so that syscall.c dispatches and
 * decides nothing.
 */

#ifndef OXYS_GFX_CLIENT_H
#define OXYS_GFX_CLIENT_H

#include <oxys/types.h>

int64_t WindowClientCreate(uint64_t geometry_address, uint64_t title_address);
int64_t WindowClientDestroy(uint64_t window);
int64_t WindowClientMove(uint64_t window, int64_t x, int64_t y);
int64_t WindowClientBlit(uint64_t window, uint64_t area_address, uint64_t pixels_address);
int64_t WindowClientEvent(uint64_t window, uint64_t event_address, uint64_t flags);
int64_t WindowClientScreen(uint64_t geometry_address);

/*
 * Destroys every window a process owns, at its ending. Called by the process
 * layer beside the release of its descriptors, for the same reason: a window
 * whose owner has ended is a window nobody will ever draw upon or drain.
 */
void WindowClientReleaseProcess(uint64_t process_id);

/*
 * Wakes every program asleep in window_event. Called by whoever routed events
 * into the windows' queues — the bootstrap processor's tick, and a self-test
 * that injects one — after the round, so that a sleeper re-tests its queue.
 * One channel for every window rather than one each, because a wake is a
 * broadcast that says only that something may have changed, and the sleepers
 * are few.
 */
void WindowClientWakeAll(void);

/* Accounting, for the report and for the self-test. */
uint64_t WindowClientCallCount(void);
uint64_t WindowClientRefusalCount(void);
uint64_t WindowClientSleepCount(void);

void WindowClientReport(void);

#endif /* OXYS_GFX_CLIENT_H */
