<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# The Window Manager and its Client Protocol

**Phase**: sub-tasks 9.1 (the manager) and 9.2 (the client protocol) of
[`../project/PLAN.md`](../project/PLAN.md); minimise and full screen after 9.7.
**Source**: [`../../graphics/window.c`](../../graphics/window.c),
[`../../graphics/client.c`](../../graphics/client.c) and their headers; the calls in
[`../../kernel/arch/x86_64/syscall/syscall.c`](../../kernel/arch/x86_64/syscall/syscall.c);
the wrappers in [`../../libc/syscall/calls.c`](../../libc/syscall/calls.c);
`KernelServiceDisplay` in [`../../kernel/kernel.c`](../../kernel/kernel.c);
`TerminalAttachKeyboard` in [`../../kernel/terminal/terminal.c`](../../kernel/terminal/terminal.c);
the demonstration [`../../userland/windows/main.c`](../../userland/windows/main.c).
**Specifications**: none governs a window manager. Three notions are taken, as
notions only, from the X Window System Protocol (X11R7.7): keyboard input has one
scope (*input focus*), siblings obscure one another in an order (*stacking order*),
and a press binds the pointer to its window until every button is released
(Chapter 11). The appearance follows [`../project/INSPIRATIONS.md`](../project/INSPIRATIONS.md).

A table of windows on one screen: the order they stack in, which holds the
keyboard, where each key and pointer event goes, the frame drawn around each, and
the six-plus calls through which a program owns a window. The layers, the session's
claim on the display and `window_text` are [`SESSION.md`](SESSION.md).

## 1. A window

A **content** surface the owner draws into, and a **frame** the manager draws: a
title band `WINDOW_TITLE_HEIGHT` (24) pixels high and a one-pixel border. The owner
never draws the frame, so every window has the same one, which is how a person
recognises a window.

- The content is a packed surface of the screen's pixel size, from the heap.
  Nothing reaches the screen until the manager composes, so drawing is never seen
  half done.
- **Every coordinate an owner sees is its content's own.** While a button binds the
  pointer, positions may lie outside the content (negative or beyond its extent).
- **At most `WINDOW_CAPACITY` (16) windows**; a seventeenth is refused.
- Titles are cut at `WINDOW_TITLE_CAPACITY` (31) characters and clipped short of the
  controls.

## 2. Stack, focus and routing

**The stack is an array** of identifiers, bottom first. Raising removes and
appends; a hit test walks from the top. An array's order cannot be made circular by
a defect, unlike a list's pointers. Windows stack within their layer
([`SESSION.md`](SESSION.md)): root, ordinary, panel.

**The focus** receives keys; with none, a key is discarded and counted. An ordinary
window gets it when created, when pressed, and when the holder is destroyed or
minimised (the topmost remaining ordinary window). Roots and panels never take it:
nothing on the panel reads keys, and a launcher that took the focus handed it back
to the panel on closing. A transfer tells the loser before the gainer, so two
windows are never drawn focused at once. The focused window's band is drawn in the
stronger colour.

**The pointer.** A movement goes to the window under it, in content coordinates; a
movement over the ground goes nowhere. **A press in a content raises, focuses,
delivers, and binds the pointer**: until every button is up, movements and releases
go to that window wherever the pointer is, or a drag ending over another window
would leave the first believing a button is held for ever. **A press in the band is
the manager's**: on a control it acts (Section 5); elsewhere it starts a drag, and
the window follows the pointer, its owner told nothing.

- **A close event is sent, never a destruction.** Closing may lose what the owner
  holds (an unsaved file), so the owner decides.
- **A move is confined**: the top never above the screen, the bottom never below,
  and 48 pixels always on screen horizontally, so a window can always be recovered.
- **A queue of `WINDOW_EVENT_CAPACITY` (32) events per window** drops the newest
  beyond that and counts it; overwriting the oldest could deliver a release with no
  press.

## 3. Appearance

**Flat.** A band, a one-pixel border, a title, controls; depth is the stacking
order only, with no bevel or shadow. **The close control is a disc** 10 pixels
across (the one curve in the frame, [`DRAWING.md`](DRAWING.md)); its reach is a
25-pixel square. **The title** is the system face at twice its size, 16 pixels in a
24-pixel band.

**Colours come from [`../../art/palette.h`](../../art/palette.h)**, shared with the
boot screen, the session and `/bin/windows`, so the frame and what it stands on are
one decision:

| Use | Colour | Why |
| --- | ------ | --- |
| Ground | `OXYS_GROUND`, a yellow | The same as the boot screen and desktop. |
| Paper | `OXYS_PAPER`, a warm white | Pure white glares beside dark text of this weight. |
| Border | `OXYS_BORDER`, a dark brown | An edge against both ground and band. |
| Focused band | `OXYS_BAR`, a stronger, more orange yellow | Differs from the ground in lightness and hue at once. |
| Other bands | `OXYS_BAR_QUIET`, the same, paler | Focus shown by strength, not hue. |
| Focused title and controls | `OXYS_INK`, a dark brown | Legible on the bar. |
| Other titles and controls | `OXYS_DIM` | Present, not being read. |

## 4. Where it runs

**From the bootstrap processor's tick**, in `KernelServiceDisplay` beside the
terminal: every pending mouse movement is routed (all, so a press and release in
one tick both arrive), every key is routed, sleepers are woken if anything was
routed, and whatever changed is composed into the back buffer and presented with
the pointer. A quiet tick does nothing. Owners draw between ticks in their own
calls. The tick, like `KernelWriteString`'s lock, runs with interrupts masked, so
composition and diagnostics cannot interleave on the one processor that does either
([`COMPOSITOR.md`](COMPOSITOR.md)); a kernel thread would be pre-empted
mid-composition.

**What the screen is for** is decided by the boot entry:

| GRUB entry | Option | The screen |
| ---------- | ------ | ---------- |
| `Oxys-OS` | none | The window manager's; the serial shell runs on the serial line. |
| `Oxys-OS (Shell-only)` | `shell-only` | The shell's, quiet: banner, prompt, pointer. |
| `Oxys-OS (Shell Diagnostics)` | `diagnostics` | The shell's, after the boot log. |

The shell also takes the screen if there is no compositor or no root, and says why.
**The terminal gives up the keyboard** to the manager (`TerminalAttachKeyboard`),
or each keystroke would reach two readers, each acting on half; the serial line is
still read.

## 5. Minimise and full screen

Beside the close control, from the right: **full screen** (a square, two
overlapping squares while full) and **minimise** (a bar). Each reach is the close
control's square, centres 26 pixels apart so no pixel belongs to two. A frame
narrower than 120 pixels carries the close control alone; otherwise a press meant
to drag it would hit a control. Unlike the close, these act directly: neither loses
anything.

- **Minimise** hides the window: not composed, not hit, cannot hold the focus, and
  releases a binding. It keeps its content, queue and owner. The owner is not told.
- **Restore** shows it if hidden, then raises and focuses it.
- **Full** gives the window the **work area**: the screen less the rows of every
  panel-layer window standing against the top edge (the clock's box) or the
  bottom edge (the bar), however wide. Panels are recognised by where they stand;
  the launcher opens above the bar and touches neither edge, so it leaves no
  hole. The band stays, so the control to undo it
  is there. The old position and extent are kept for undoing. A full window cannot
  be dragged. **The content becomes a new surface**: the old pixels copied where
  they fit, the rest paper, so the window is never blank while its owner redraws;
  the owner is sent `WINDOW_EVENT_RESIZE` with the new width and height in `x` and
  `y`. If the heap cannot supply the surface, nothing changes and the call is
  `ENOMEM`.
- **Roots are told.** Creating, destroying, minimising, restoring or making full an
  ordinary window, and passing the focus between ordinary windows, puts one
  `WINDOW_EVENT_WINDOWS` in each root's queue, **never two**: it means "look again",
  and a stream of them would fill the queue and drop a press.

## 6. The client protocol

A process owns windows through system calls (numbers in
[`PRIVILEGE.md`](PRIVILEGE.md)):

| Call | Does |
| ---- | ---- |
| `window_create` | Makes a window of a given content size and title; returns its number. |
| `window_destroy` | Destroys one of the caller's windows. |
| `window_move` | Moves one. |
| `window_blit` | Copies a rectangle of the caller's pixels into the content. |
| `window_event` | Takes one event, from one window or `SYSCALL_WINDOW_ANY`, optionally waiting. |
| `window_screen` | Reports the screen's size. |
| `window_state` | Minimise, restore, full, not full. |
| `window_list` | Lists ordinary windows (the session only). |

`window_session` and `window_text` are [`SESSION.md`](SESSION.md).

**What crosses.** A surface does not: it describes kernel memory, and a shared
mapping would give the program memory whose lifetime is the window's. Instead a
**rectangle of pixels in one format** crosses: `window_blit` copies, validating the
whole rectangle first, then writes through a surface over the content. **The
client's pixel is `0x00RRGGBB`** whatever the screen's format, encoded on the way in
by `FramebufferEncode`, so a program does not break when the loader picks another
mode. **An event** crosses as a `SyscallWindowEvent`, converted field by field, so
the manager's structure can change without the ABI moving.

- **`SYSCALL_WINDOW_ANY`** lets a program with several windows sleep for all; the
  event names its window, and the scan starts after the window last served, so a
  busy window cannot starve a quiet one.
- **Ownership.** A window records its creating process (kernel windows record zero,
  which no process is). Acting on another process's window is `EBADF`, as for a
  descriptor one does not hold. `window_state` also lets the session act on any
  ordinary window; ownership is judged before the layer, so a stranger learns only
  `EBADF`. `window_list` is `EPERM` except to the session.
- **A process's windows are destroyed when it ends**, beside its descriptors: nobody
  would draw on them or drain them.
- **The wait** (`SYSCALL_WINDOW_WAIT`) sleeps on one channel for all windows, tested
  and entered in one masked section; the tick wakes every sleeper when it routed
  anything, and each rechecks its own queue. A signal ends it with `EINTR`. A caller
  with no thread to sleep on is refused with `ENOTSUP`. A program waiting on its
  window and a pipe together uses `poll` ([`TERMINAL.md`](TERMINAL.md)).

**`/bin/windows`**, the launcher's *Windows* entry, is the demonstration: an *Oxys*
window showing the mark, a *Pointer* window whose disc follows the pointer and grows
while a button is held (and vanishes, still receiving, when dragged out while
bound), and a *Keys* window drawing a tile per character. It asks the screen's size
and doubles its scale on screens at least 1,024 wide, and ends when its last window
is closed. Its settings are `/etc/desktop.conf` ([`CONFIG.md`](CONFIG.md)).

## Verification

`KernelVerifyWindows` in [`../../kernel/test/gfx/windows.c`](../../kernel/test/gfx/windows.c)
runs on a 160 × 120 memory screen with a pitch beyond its width and a sentinel in
the padding. **Stacking is asserted by reading the pixel where two windows
overlap**, not by asking the manager, which could keep the order right and draw it
wrong.

| Property asserted | The failure it would catch |
| ----------------- | -------------------------- |
| A second window stands above the first and holds the focus; its frame is content plus band and border. | A new window drawn beneath; a band press landing in the content. |
| The first window is told it gained then lost the focus; the second that it gained. | A loser still drawn focused. |
| After composition the overlap shows the upper window, the lower's own area the lower, the ground the ground, the bands their focus colours, the corner the border. | Painter's order reversed; focus drawn on the wrong window. |
| The hit test in the overlap names the upper window; on the ground, none. | A hit test from the bottom. |
| A press in the lower window raises, focuses, notifies both, delivers in content coordinates and binds; the raise marks the frame changed and it is then drawn on top. | Screen coordinates delivered; a raise never redrawn. |
| While bound, movement over the other window and the release go to the bound one; afterwards, movement goes to the window beneath. | A binding ending at the frame edge; a binding never ending. |
| A key goes only to the focus. | Keys to the pointer's window, or to all. |
| A band drag moves the window by the pointer's movement, delivers nothing, marks old and new frames, and the old frame shows the ground. | A trail. |
| The close control delivers one close event and destroys and binds nothing. | The manager discarding an unsaved file. |
| Moves off the top-left and bottom-right are held at the confining positions. | A window lost off screen. |
| Destroying the focus holder passes focus down and says so; destroying the last leaves none, and a key is discarded and counted. | Keys into a queue nobody drains. |
| Sixteen windows are accepted and a seventeenth refused; content below 16 pixels is refused; every window made is released. | A table overrun; a test leaving the table full. |
| A queue holds 32 and drops the newest beyond, counting it. | A release with no press. |
| The root is told once, however many changes happened. | Notices filling the root's queue and dropping a press. |
| The panel takes the focus neither when made nor when pressed, and still receives the press. | Typing into nowhere after the launcher closes. |
| Full gives the frame the work area and the content the frame less band and border; the owner is told; the old content is kept; a full window is not dragged; pressed again, it is restored exactly. | A full window under or over the panel; blank until redrawn; not restorable. |
| The work area keeps the rows of a panel-layer bar at the foot and a box at the top right, and none for a launcher touching neither edge. | A window made full under the bar or the clock, its controls covered. |
| A minimised window is not hit or drawn and gives up the focus to the topmost remaining; the root is told. A restored one is shown, raised and focused. | Hidden windows taking keys; a chosen window left behind another. |
| A root and a panel are refused minimise and full; a frame under 120 pixels has only the close control. | A desktop minimised with nothing to restore it. |
| **Nothing wrote into the padding.** | Composition escaping the surface. |

`KernelVerifyClients` in [`../../kernel/test/gfx/client.c`](../../kernel/test/gfx/client.c)
runs `window-check` ([`../../userland/window-check/main.c`](../../userland/window-check/main.c))
at privilege level 3, with a kernel thread (the **hand**, pinned to the bootstrap
processor) that runs while the program sleeps, reads its pixels off the screen and
injects a key.

| Property asserted | The failure it would catch |
| ----------------- | -------------------------- |
| `window_screen` reports the test surface; a bad address is `EFAULT`. | Windows placed by guess. |
| A window's first event is the focus arriving; an empty queue reports none. | A create returning a dead number. |
| A whole-content blit succeeds; one outside the content is `EINVAL`; bad addresses are `EFAULT`. | Silent clipping; the kernel reading a bad pointer. |
| **The pixels are on screen** where the content stands (red the column, green the row, blue `0x5A`). | A wrong pitch shearing the image; swapped channels. |
| Focus passes between two windows of one process via `ANY`, naming the window, and back on destruction; `ANY` with nothing queued reports none. | Events in the wrong queue, duplicated or misattributed. |
| Destroying twice, a dead number, and the kernel's own window are `EBADF`; an unknown flag is `EINVAL`. | A program reaching another's window. |
| A move succeeds; one past the coordinate limit is `EINVAL`. | A truncated coordinate. |
| The wait ends with the injected key, after exactly one sleep. | A wait returning at once, or never. |
| The window left standing is gone at the program's end, before `ProcessDestroy`; the kernel's own survives. | A window outliving its owner. |
| `window_state` on its own window does all four actions; an unknown action is `EINVAL`, a dead number `EBADF`. | The call acting on others' windows. |
| `window_list` is `EPERM` without the session. | Every program reading every title. |
| A call from no process is `EBADF`; no open file or child is left; the hand ran and saw the program sleep. | A pass from a test that never slept. |

With the ownership check removed, the kernel's own window can be destroyed by the
program; with the release at exit removed (tested **alone**, since with both
damages the first masks the second), the program's window survives it.

## Limitations

1. The whole round runs in the timer's interrupt handler; a screen-sized window
   dragged across the screen composes a million pixels a tick.
2. One damage rectangle; a window touching it is drawn whole, clipped.
3. No resize by hand, no title change.
4. Focus follows a press only; no key moves it, no window can request it.
5. No rounded corners or asymmetry, both wanted by `INSPIRATIONS.md`.
6. The mode is the loader's; `/bin/windows` scales, nothing else does.
7. Every pixel a program draws is copied and converted one at a time; no shared
   mapping.
8. "Full" keeps the title band and the panel; there is no true full-screen mode.
9. Panels are recognised by touching the top or bottom edge; a panel-layer window
   against a side edge keeps no columns.
10. The resize event is advisory: into a smaller content, an old-size blit is
    refused.
11. A minimised window's owner is not told.
12. No lock on the table, stack and queues; filled by the tick and drained by calls
    on the bootstrap processor with interrupts masked ([`CONCURRENCY.md`](CONCURRENCY.md)).
