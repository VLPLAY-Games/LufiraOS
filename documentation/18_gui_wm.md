# GUI and Window Manager

This document describes the kernel side of LufiraOS's GUI: a generic mailbox IPC primitive, the GUI syscall surface built on top of it, and the client/WM wire protocol. The window manager itself — the compositor, z-ordering, drag/resize/maximize, the taskbar, desktop icons — is **not kernel code**. It's an ordinary userspace process, `wm.c`, in the sibling repository [`lufira-packages`](https://github.com/VLPLAY-Games/lufira-packages), that happens to be the one registered caller of the privileged syscalls described here. This document covers the kernel-side mechanism; see that repository for the compositor's own internals.

---

## Table of Contents

1. [Overview](#overview)
2. [Mailbox IPC](#mailbox-ipc)
3. [The WM Wire Protocol](#the-wm-wire-protocol)
4. [GUI Syscalls](#gui-syscalls)
5. [Input Routing](#input-routing)
6. [Process Exit and Orphaned Windows](#process-exit-and-orphaned-windows)
7. [Dependencies](#dependencies)
8. [Known Limitations](#known-limitations)
9. [Conclusion](#conclusion)

---

## Overview

Before this existed, two unrelated processes had no way to share memory (`shm.c` only supports fork-time `MAP_SHARED`) or exchange a message (pipes only work between processes related by `fork()`, via inherited file descriptors) at all. That gap is exactly why the first GUI implementation lived entirely in the kernel (`kernel/system/gui/gui.c`, since removed): there was no other way for a client process and a compositor to talk to each other. **Mailbox IPC** (`kernel/system/ipc/mailbox.c`) closes that gap — one message mailbox per process, addressed by pid, usable between any two processes — and the GUI is now just its first real client, not a special case baked into the kernel.

The pieces, bottom to top:
- **Mailbox IPC** — generic, not GUI-specific. `SYS_IPC_SEND`/`SYS_IPC_RECV`.
- **The WM wire protocol** (`wm_protocol.h`) — a request/reply struct shape built *on top of* mailbox IPC, understood by both the kernel (which relays client syscalls into it) and `wm.c` (which parses and answers it). Not itself a syscall.
- **GUI syscalls** (`SYS_WIN_*`, `SYS_WM_REGISTER`, `SYS_FB_*`) — what an ordinary client program actually calls; each is a thin RPC wrapper that packs a request, sends it to the registered WM process's mailbox, and blocks for the reply.
- **Input routing** (`kernel/drivers/input/input.c`) — raw keyboard/mouse events are pushed into the registered WM's mailbox directly from the PS/2/USB HID code path, bypassing the old text-console input queue entirely while a WM is registered.

---

## Mailbox IPC

`kernel/system/ipc/mailbox.c`/`.h` — one fixed-size mailbox per process, 32 slots of up to 96 bytes each:

```c
#define MAX_IPC_MSG_PAYLOAD   96
#define MAX_IPC_MAILBOX_SLOTS 32

typedef struct {
    uint32_t sender_pid;
    uint32_t len;
    uint8_t payload[MAX_IPC_MSG_PAYLOAD];
} ipc_msg_t;
```

**API** (kernel-internal; userspace reaches this only via `SYS_IPC_SEND`/`SYS_IPC_RECV`, see below):

| Function | Description |
|----------|-------------|
| `int mailbox_send(uint32_t dest_pid, uint32_t sender_pid, const void *data, uint32_t len)` | Copies a message into `dest_pid`'s mailbox. `sender_pid == 0` is a reserved sentinel meaning "from the kernel itself" (real pids never reach 0 — pid 0 is the idle process). `0` on success, `-1` if `dest_pid` doesn't exist or `len` exceeds the payload limit, `-2` if the destination's mailbox is full (32 slots is far more than any realistic number of simultaneous WM clients needs). |
| `int mailbox_recv(void *out, int timeout_ms)` | Dequeues the calling process's own next message. `timeout_ms < 0` blocks indefinitely with zero CPU cost (a real `PROCESS_BLOCKED` state, woken explicitly by a matching `mailbox_send()` — not a poll loop); `0` returns `-1` immediately if empty; `>0` polls in 1-tick steps up to that budget. `0` on success, `-1` on timeout/empty. |

**Why this needed its own locking, unlike a plain queue:** `mailbox_send()` is called not just from ordinary syscall context (one process messaging another) but *directly from interrupt handlers* — specifically, the keyboard/mouse IRQ path (`input.c`, see [Input Routing](#input-routing) below) pushes straight into the registered WM's mailbox. The ring buffer's head/tail/count are therefore mutated from contexts that can genuinely interrupt each other, so every access is wrapped in a `cli`/`sti` pair (`mailbox_lock()`/`unlock()`) — the same pattern already used by `console_write_lock()` in `vfs.c`, for the same reason: without it, a timer or keyboard IRQ could land mid-push/pop in another context and corrupt the ring indices.

A process is single-threaded, so exactly one `waiter` slot per mailbox (a process blocked in its own `mailbox_recv()`) is sufficient — it can never be blocked in two receives at once.

---

## The WM Wire Protocol

`kernel/system/ipc/wm_protocol.h` (kernel side) and its byte-for-byte mirror `libc/include/lufira/wm_protocol.h` (userspace side) define the message shapes carried *inside* mailbox IPC payloads. Both sides must agree on the exact layout — there's no serialization, just a packed struct copied through `mailbox_send()`/`mailbox_recv()`.

**Client RPC request/reply** — a client's `SYS_WIN_*` call becomes one of these, sent to the registered WM's mailbox with `sender_pid` set to the caller's own pid:

```c
typedef struct __attribute__((packed)) {
    uint32_t opcode;   // WM_OP_WIN_* below
    int32_t a[6];       // opcode-specific integer args
    char str[64];        // opcode-specific string arg (e.g. a window title)
} wm_request_t;

typedef struct __attribute__((packed)) {
    int32_t result;                       // window id, 0/-1, or 0/1 for poll_event
    int32_t ev_type, ev_x, ev_y, ev_key;  // only meaningful for WM_OP_WIN_POLL_EVENT
} wm_reply_t;
```

| Opcode | Meaning |
|--------|---------|
| `WM_OP_WIN_CREATE` (1) | `a[0..3]` = x, y, w, h; `str` = title. |
| `WM_OP_WIN_DESTROY` (2) | `a[0]` = window id. |
| `WM_OP_WIN_FILL` (3) | `a[0]` = window id, `a[1]` = color. |
| `WM_OP_WIN_DRAW_RECT` (4) | `a[0]` = window id, `a[1..4]` = x, y, w, h, `a[5]` = color. |
| `WM_OP_WIN_DRAW_TEXT` (5) | `a[0]` = window id, `a[1..2]` = x, y, `a[3]` = color; `str` = text. |
| `WM_OP_WIN_POLL_EVENT` (6) | `a[0]` = window id. |
| `WM_OP_WIN_MOVE` (7) | `a[0]` = window id, `a[1..2]` = x, y. |

Since every client sends exactly one request and immediately blocks for the reply before sending another (no client makes a second call before the first answers), there is no request/reply correlation ID anywhere in this protocol — one mailbox, at most one request in flight per client, so the next message received back is unambiguously the answer.

**Kernel-originated messages** — sent with `sender_pid == WM_SENDER_KERNEL` (0), distinguishing them from a real client's RPC request on arrival:

| Opcode | Meaning |
|--------|---------|
| `WM_INPUT_KEY` (100) | `a[0]` = key code — raw keyboard input, see [Input Routing](#input-routing). |
| `WM_INPUT_MOUSE` (101) | `a[0]`/`a[1]` = absolute accumulated x/y, `a[2]` = button bitmask. |
| `WM_NOTIFY_PROCESS_EXIT` (102) | `a[0]` = pid of a process that just exited — see [Process Exit and Orphaned Windows](#process-exit-and-orphaned-windows). |

---

## GUI Syscalls

These are what an ordinary client program actually links against (`libc/include/lufira/syscall.h`); each one's kernel-side implementation (`syscall.c`) is a thin wrapper: pack a `wm_request_t`, `mailbox_send()` it to `process_get_wm_pid()`, block on `mailbox_recv()` for the `wm_reply_t`, unpack the result. See [`13_syscalls.md`](13_syscalls.md#gui--window-manager-50-62) for the full per-syscall reference; summarized here:

| Syscall | Purpose |
|---------|---------|
| `SYS_WIN_CREATE` (50) | Create a window; fails if no WM is registered. |
| `SYS_WIN_DESTROY` (51) | Close a window (ownership-checked by the WM). |
| `SYS_WIN_FILL` (52) | Fill a window's whole client area with one color. |
| `SYS_WIN_DRAW_RECT` (53) | Filled rectangle in client-area coordinates. |
| `SYS_WIN_DRAW_TEXT` (54) | Bitmap-font text (the same 8×8 font as the text console). |
| `SYS_WIN_POLL_EVENT` (55) | Non-blocking poll for the next queued input/resize/close event. |
| `SYS_WIN_MOVE` (56) | Move a window. |
| `SYS_IPC_SEND` / `SYS_IPC_RECV` (57/58) | The generic mailbox primitive itself, exposed directly — not GUI-specific, but how the WM-client RPC above is actually carried. |
| `SYS_WM_REGISTER` (59) | Become *the* WM process. Exactly one at a time — a second caller fails. |
| `SYS_FB_INFO` (60) | Screen width/height/pixel format, for sizing a compositor buffer. |
| `SYS_FB_PRESENT` (61) | WM-only: push a (possibly partial, dirty-rectangle) frame buffer to the real framebuffer. |
| `SYS_FB_FONT` (62) | Copy out the console's own 8×8 bitmap font, so client-side text rendering matches the text console's. |

`SYS_FB_PRESENT` and `SYS_FB_INFO` are privileged — the kernel checks the caller's pid against `process_get_wm_pid()` and refuses anyone else. Everything else (`SYS_WIN_*`) is open to any process; what's actually *allowed* (e.g. only the owning process may destroy its own window) is enforced by `wm.c` itself when it answers the RPC, not by the kernel.

---

## Input Routing

While a WM is registered, `kernel/drivers/input/input.c` pushes every keyboard and mouse event straight into the WM's mailbox as a `WM_INPUT_KEY`/`WM_INPUT_MOUSE` message (`sender_pid = WM_SENDER_KERNEL`) instead of the old path — the text console's input ring buffer and the dead kernel-native shell never see a single keystroke while a WM owns the screen. This is a deliberate behavioral switch, not a bug: exactly one of "text console" or "GUI" owns input at a time, matching how any real windowing system behaves (a background text terminal doesn't see keys typed into a foreground GUI).

The WM itself decides focus/routing from there — which window (if any) a given key goes to, or whether a mouse-down lands on a window, the taskbar, or the desktop — entirely in userspace (`wm.c`). The kernel has no concept of "focus"; it just hands raw events to whichever process registered itself as the WM.

When no WM is registered, input flows exactly as it always did (the text console's normal input queue, [`14_shell_commands.md`](14_shell_commands.md)).

---

## Process Exit and Orphaned Windows

A client that crashes or gets `kill`ed doesn't get a chance to close its own windows first. Rather than the kernel reaching directly into WM state to clean them up (which was possible in the old kernel-resident GUI, but isn't once the WM is just another userspace process), `process_exit()`/signal-driven termination sends the WM a `WM_NOTIFY_PROCESS_EXIT` message instead, and `wm.c` is responsible for finding and destroying every window owned by that pid itself.

---

## Dependencies

| Component | Depends On | Purpose |
|-----------|------------|---------|
| Mailbox IPC | Process table (`system/process/process.h`) | `mailbox_send()` looks up `dest_pid`; each `process_t` owns one `ipc_mailbox_t`. |
| GUI syscalls | Mailbox IPC, `process_get_wm_pid()` | Every `SYS_WIN_*`/`SYS_FB_*` is an RPC over mailbox IPC addressed to the registered WM pid. |
| Input routing | PS/2 keyboard/mouse drivers, USB HID (`07_drivers.md`) | Both input paths funnel into the same `input.c` dispatch, which checks `process_get_wm_pid()` on every event. |
| `wm.c` (userspace, `lufira-packages`) | GUI syscalls, `SYS_FB_*` | The actual compositor — not documented here, see that repository. |

---

## Known Limitations

- **Exactly one WM process at a time**, enforced by `SYS_WM_REGISTER` — no multi-seat, no nested/secondary compositors.
- **96-byte mailbox messages, 32 slots per process** — fine for the small fixed-shape RPC/input messages this protocol actually carries, not a general-purpose large-message IPC mechanism.
- **No per-request correlation ID** — relies on the "one request in flight per client" invariant holding; a client that ever sent a second `SYS_WIN_*` call before the first replied would misparse the response to the first as the response to the second. Every current client (`gui_widgets.c` and all GUI apps) is written to respect this.
- **The kernel enforces no window-ownership/focus policy** — that's entirely `wm.c`'s responsibility; a kernel bug could in principle let a malicious client address an RPC that `wm.c` doesn't validate, since the kernel itself only authenticates `SYS_FB_*` (WM-only), not the contents of a `SYS_WIN_*` request.

---

## Conclusion

The kernel's half of the GUI is deliberately thin: a generic mailbox primitive, a privileged framebuffer-present syscall, and a set of RPC wrappers that relay window operations to whichever process registered itself as the window manager. All of the actual compositing, input focus, drag/resize, taskbar, and app logic lives in ordinary userspace code in `lufira-packages`, exercising the same syscall surface any other program could.

For more details, refer to `kernel/system/ipc/mailbox.c`/`wm_protocol.h`, the `SYS_WIN_*`/`SYS_FB_*`/`SYS_WM_REGISTER`/`SYS_IPC_*` implementations in `kernel/system/syscall/syscall.c`, and `apps/wm.c` in `lufira-packages`.

---

**Document Version:** 1.0
**Last Updated:** October 2026
**Project:** LufiraOS
