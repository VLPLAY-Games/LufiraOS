# LufiraOS Driver Subsystem

This document describes the device drivers included in the LufiraOS kernel. The drivers are written in C and interact directly with hardware via memory‑mapped I/O, port I/O, and PCI configuration space. They are designed to be modular, lightweight, and suitable for a hobby operating system.

---

## Table of Contents

1. [Overview](#overview)
2. [Common Infrastructure](#common-infrastructure)
3. [Console Driver](#console-driver)
4. [Serial UART Driver (16550)](#serial-uart-driver-16550)
5. [Disk Drivers (ATA PIO / AHCI / NVMe)](#disk-drivers-ata-pio--ahci--nvme)
6. [Keyboard Driver (PS/2)](#keyboard-driver-ps2)
7. [Mouse Driver (PS/2)](#mouse-driver-ps2)
8. [USB (xHCI + HID + Mass Storage)](#usb-xhci--hid--mass-storage)
9. [Input Dispatcher](#input-dispatcher)
10. [Network Drivers (RTL8139 / e1000 / e1000e)](#network-drivers-rtl8139--e1000--e1000e)
11. [PCI Bus Driver](#pci-bus-driver)
12. [Userspace Driver Infrastructure](#userspace-driver-infrastructure)
13. [Audio Drivers (AC’97 / Intel HDA)](#audio-drivers-ac97--intel-hda)
14. [Driver Initialisation Sequence](#driver-initialisation-sequence)

---

## Overview

The driver subsystem provides hardware abstraction for essential peripherals:

- **Console** – graphical text output with a custom 8×8 font, 256‑color palette, cursor and scrollback.
- **Serial** – a polled 16550 UART driver (COM1) that mirrors every console line out over the serial port, for logging/debugging without a graphical display.
- **Disk** – ATA PIO read/write for raw sector access (primary IDE channel), plus AHCI SATA and NVMe drivers for controllers that don't expose a legacy IDE interface at all, with automatic fallback (AHCI → NVMe → legacy ATA PIO).
- **Keyboard** – PS/2 keyboard with scancode translation, modifier handling, and IRQ1 interrupt support.
- **Mouse** – PS/2 mouse initialisation and packet decoding.
- **USB** – an xHCI host‑controller driver providing boot‑protocol HID keyboard/mouse input, Bulk‑Only Transport USB Mass Storage (block read/write), and post-boot hotplug attach/detach, unified with PS/2 through a shared input dispatcher.
- **Network** – an RTL8139 Fast Ethernet driver, plus Intel e1000/e1000e drivers for a second NIC family. **As of v0.9.5, these no longer live in the kernel at all** — they, and the whole protocol stack built on top of them, moved into a userspace daemon, `netd` (`lufira-packages/daemons/netd/`); see [Userspace Driver Infrastructure](#userspace-driver-infrastructure) below and [`16_networking.md`](16_networking.md) for the full Ethernet/ARP/IP/ICMP/TCP/UDP/DNS/TLS/HTTP stack and its DHCP client. `ifconfig`/`ping`/`wget` (`lufira-packages`) are userspace consumers of `netd`'s syscall surface, alongside `dlpg sync`/`dlpg upgrade` via `SYS_NET_FETCH`.
- **PCI** – bus enumeration, configuration space access, and BAR (Base Address Register) management.
- **Audio** – AC’97 (Intel ICH‑compatible) and Intel HDA controllers, both with DMA‑based playback and tone generation. **As of v0.9.5, also moved out of the kernel**, into a second userspace daemon, `soundd` (`lufira-packages/daemons/soundd.c`), which tries HDA first, then AC’97.

Drivers are designed to be initialised early in the kernel boot process, after the physical memory manager (PMM) and interrupt descriptor table (IDT) are set up. All drivers are polled or interrupt‑driven; the console is used for debugging and user interaction. Most drivers' verbose status output is gated by **developer mode** (see [`04_logging.md`](04_logging.md)) — by default only errors and a handful of high-level "ready/not ready" lines are shown; enable it with the `devmode` shell command or `make debug` to see per‑device/per‑register detail.

---

## Common Infrastructure

Drivers rely on a small set of common facilities:

- **`lib/types.h`** – standard integer types (`uint8_t`, `uint16_t`, `uint32_t`, `uint64_t`).
- **`lib/string.h`** – memory and string functions (`memcpy`, `memset`, `strlen`).
- **`lib/stdarg.h`** – variadic argument handling for `printf`.
- **`drivers/console/console.h`** – console output functions (`printf`, `put_char`, etc.).
- **`system/mm/pmm.h`** – physical memory manager for allocating DMA‑safe pages. `pmm_alloc_page()` hands out a single 4 KiB page at a time with no guarantee of physical contiguity between calls (used by AC’97, and by xHCI for its per-structure pages). `pmm_alloc_contiguous_pages(count)` allocates `count` *physically contiguous* pages in one call — added for the RTL8139 driver, whose receive ring is a single scatter‑gather‑incapable physical buffer; see [RTL8139 Network Driver](#rtl8139-network-driver).

All drivers are compiled into the kernel image and initialised by `kernel.c` after hardware detection.

---

## Console Driver

The console driver provides a graphical text‑mode interface using the framebuffer set up by the bootloader. It supports:

- **8×8 bitmap font** with a custom character set (ASCII 32–126).
- **256‑color palette** (16 standard VGA colours + 6×6×6 RGB cube + 24 grays).
- **Scrollback buffer** – up to 256 lines, with scrolling (PgUp/PgDn via Ctrl+Up/Down).
- **Blinking cursor** – underscore style, programmable blink rate.
- **Colour management** – foreground/background colours can be set by palette index or direct RGB.
- **Helper functions** – `printf`, `clear_screen`, `set_cursor_position`, etc.

### Key Data Structures

- `ColorPair` – stores foreground and background colours (both RGB values and palette indices).
- `console_history_cell_t` – a cell in the scrollback buffer (character + colour indices).
- `console_history[CONSOLE_HISTORY_LINES][CONSOLE_MAX_COLUMNS]` – scrollback storage.

### Public API

| Function | Description |
|----------|-------------|
| `initialize_console(BootInfo*)` | Sets up framebuffer, palette, clears screen. |
| `put_char(char)` | Prints a single character, handles newline, backspace, tab. |
| `printf(const char*, ...)` | Formatted output (supports `%s`, `%d`, `%u`, `%x`, `%p`). |
| `set_color_by_index(ConsoleColor, ConsoleColor)` | Set text/bg using palette indices (0–255). |
| `set_color_by_rgb(uint32_t, uint32_t)` | Set text/bg using 24‑bit RGB (converted to framebuffer format). |
| `draw_cursor()` / `erase_cursor()` | Manually show/hide cursor. |
| `console_scroll_up()` / `console_scroll_down()` | Scroll the scrollback buffer (triggered by Ctrl+Arrow keys). |
| `clear_entire_screen()` | Fill the whole framebuffer with current background colour. |

The console is used by all other drivers for logging and user feedback.

---

## Serial UART Driver (16550)

`drivers/serial/serial.c`/`.h` drives a 16550 UART on COM1 (I/O port `0x3F8`) — entirely polled, no interrupts, same discipline as every other driver in this chapter. Its purpose is debug output independent of the graphical console: `put_char()` (`console.c`) mirrors every character written to the screen out over this port, so `qemu ... -serial stdio` shows full kernel output even with no video, and `serial_init()` is deliberately the very first thing `_start()` calls in `kernel.c` — before the console, before the GDT — so the mirror catches boot output from the first line onward.

### Initialisation and presence detection

`serial_init()` programs the port for 115200 baud / 8N1 (divisor `1`), enables the FIFO (`FCR_ENABLE_FIFO`), then runs the standard way to detect a real 16550 without ever seeing it: an internal **loopback test** (`MCR_LOOPBACK`) — write a known byte, read it back, confirm it matches — before normal mode (`MCR_NORMAL`) is restored. Without this check, a system with no serial port at all would make `serial_putc()` spin forever waiting for a Line Status Register "THR empty" bit that would never appear.

### Public API (`drivers/serial/serial.h`)

| Function | Description |
|----------|--------------|
| `void serial_init(void)` | Programs the UART and runs the loopback presence test. Safe to call even if there is no port at all. |
| `int serial_available(void)` | Returns 1 if the loopback test passed. |
| `void serial_putc(char c)` | Blocking (bounded-retry) single-character write; no-op if `serial_available()` is false. |
| `void serial_puts(const char *s)` | Writes a whole string via repeated `serial_putc()`. |
| `int serial_try_getc(void)` | Returns the next received byte if one is already buffered, or -1 — the same "never block the kernel" pattern used by `usb_poll()`/`net_poll()`. |

---

## Disk Drivers (ATA PIO / AHCI / NVMe)

The disk driver provides low‑level sector access to an ATA hard disk or CD‑ROM using Programmed I/O (PIO) mode on the primary IDE channel (I/O ports `0x1F0–0x1F7`). It implements **LBA28** addressing and supports both read and write operations.

### Features

- **Block I/O** – read/write one or more sectors (512 bytes each).
- **LBA addressing** – up to 128 GiB (28‑bit LBA).
- **Busy‑wait loops** – waits for BSY and DRQ flags.
- **Error detection** – returns negative on timeout or device error.

### Public API

| Function | Description |
|----------|-------------|
| `disk_read_sectors(uint32_t lba, uint8_t count, void *buffer)` | Reads `count` sectors from LBA into buffer. |
| `disk_write_sectors(uint32_t lba, uint8_t count, const void *buffer)` | Writes `count` sectors from buffer to LBA. |

**Notes:**
- The driver assumes a single master drive on the primary channel.
- No DMA or interrupt support – purely synchronous.
- Used by higher‑level filesystem code (e.g., FAT32, LufiraFS) if present.

### AHCI SATA Driver (`drivers/disk/ahci.c`)

Added alongside legacy ATA PIO for machines whose SATA controller exposes no legacy IDE compatibility mode at all. Found via `pci_find_class_if()` (class `0x01`, subclass `0x06`, prog-if `0x01`); only the **first** controller and its **first** SATA port with a device attached (signature `SATA_SIG_ATA`, ATAPI/empty ports skipped) are brought up — one port, one in-flight command (slot 0), polled completion (`PxCI`/`PxIS.TFES`) instead of interrupts, same reasoning as every other driver here (no MSI/MSI-X capability parsing in this kernel).

- **DMA model** – rather than scatter-gather over the caller's own buffer, one physically-contiguous 128 KiB **bounce buffer** (`AHCI_BOUNCE_PAGES` = 32 pages, enough for the 255-sector max of one command) is memcpy'd to/from on every read/write — simpler and safer than validating an arbitrary caller buffer's physical contiguity.
- **Commands** – `READ DMA EXT`/`WRITE DMA EXT` (LBA48, `0x25`/`0x35`) via a Register FIS built into a single Command Table/PRDT pair in slot 0.
- **Waiting for completion** – a hybrid wait (`ahci_wait_tfd_free()`): a short fast busy-spin first (catches the common case of a near-instant completion with no delay at all), falling back to `pit_wait_ms()`-based polling (real PIT ticks, not a fixed iteration count) if the command is still outstanding — replacing an earlier pure fixed-iteration busy-wait that was far too slow under QEMU/TCG emulation and could stall text rendering visibly during boot.

Public API (`drivers/disk/ahci.h`): `ahci_init()`, `ahci_available()`, `ahci_read_sectors(uint32_t lba, uint8_t sector_count, void *buffer)`, `ahci_write_sectors(uint32_t lba, uint8_t sector_count, const void *buffer)` — same shape as the ATA PIO functions above, so `disk.c` can dispatch to whichever controller was actually found.

### NVMe Driver (`drivers/disk/nvme.c`)

A second fallback after AHCI, for drives with no AHCI compatibility either. One controller, one namespace (NSID 1, which must report a 512-byte LBA with no metadata — otherwise the controller is treated as unusable and `disk.c` falls through to the next driver), one admin queue pair and one I/O queue pair (`QID` 0/1), each only 2 entries deep (at most one command in flight). Like AHCI, it's fully polled: NVMe completion is a **phase-bit flip** written by the controller into an ordinary memory-resident completion queue entry — a built-in polling mechanism, not a replacement for an interrupt the kernel would otherwise take. Data transfer uses PRP (Physical Region Page) addressing into a 128 KiB physically-contiguous bounce buffer, with a pre-built PRP List page covering pages 2-32 (the buffer never moves, so the list is built once).

Two reliability fixes landed alongside the driver's introduction: the same fixed-iteration-busy-wait problem AHCI had (replaced with the same fast-spin/PIT-wait hybrid, shared helper in `disk.c`), and an Identify-Namespace read that could occasionally observe a torn DMA payload (`lbads==0`, physically impossible) despite a successful completion status — now retried a few times instead of trusted on the first read.

Public API (`drivers/disk/nvme.h`): `nvme_init()`, `nvme_available()`, `nvme_read_sectors(...)`, `nvme_write_sectors(...)` — identical shape to the AHCI/ATA PIO functions.

### Driver Fallback Chain

`disk_init()` (`drivers/disk/disk.c`) tries `ahci_init()`, then `nvme_init()`, then falls back to legacy ATA PIO if neither succeeds; `disk_read_sectors()`/`disk_write_sectors()` dispatch to whichever one is actually available (`ahci_available()` → `nvme_available()` → ATA PIO) — `LufiraOS-Builder`'s `--disk-mode ahci|nvme|ide` picks which of the three a QEMU run actually exercises (see [`05_build_system.md`](05_build_system.md)), but all three remain probed/available in every build.

---

## Keyboard Driver (PS/2)

The keyboard driver handles a standard PS/2 keyboard connected to port `0x60`/`0x64`. It translates scancodes (set 1) into ASCII characters and control codes.

### Key Features

- **Scancode translation** – supports regular keys, shifted symbols, and Caps Lock.
- **Modifier keys** – Shift, Ctrl, Alt (left and right) are tracked.
- **Extended scancodes** – handles `0xE0` prefix for arrow keys and special keys.
- **Interrupt‑driven** – `keyboard_irq_handler()` is called from IRQ1.
- **Input buffer** – stores characters for the shell.

### Public API

| Function | Description |
|----------|-------------|
| `keyboard_init()` | Resets controller, enables interrupts, tests presence. |
| `keyboard_irq_handler()` | IRQ1 handler; reads scancodes and processes them. |
| `keyboard_scancode_to_key(uint8_t)` | Converts raw scancode to ASCII or key code (arrows). |
| `keyboard_ctrl_pressed()` | Returns 1 if Ctrl is currently held down. |
| `keyboard_is_initialized()` | Returns 1 if keyboard was successfully detected. |

**Key codes for arrow keys** are defined as `KEY_LEFT_ARROW`, etc., and are passed to the shell for line editing.

The driver also maintains a global `input_buffer` used by the shell for command input.

**Known Issue:** holding a key down does not repeat it — only the initial keypress is registered. This applies to both the PS/2 and USB HID paths; see [README.md § Known Issues](../README.md#known-issues-and-limitations).

---

## Mouse Driver (PS/2)

The mouse driver initialises a PS/2 mouse (auxiliary device) and processes standard 3‑byte packets (with 4‑byte extensions not yet supported). It uses the same PS/2 controller as the keyboard.

### Features

- **Auto‑detection** – sends reset and enable commands.
- **Packet decoding** – extracts relative movement (X, Y) and button states (left, right, middle).
- **Simple state** – maintains absolute coordinates (clamped to screen) and button mask.
- **Interrupt‑driven** – `mouse_irq_handler()` is called from IRQ12.

### Public API

| Function | Description |
|----------|-------------|
| `mouse_init()` | Enables the mouse, sets sample rate, and waits for ACK. |
| `mouse_irq_handler()` | IRQ12 handler; reads packets and updates coordinates. |
| `mouse_is_initialized()` | Returns 1 if mouse is ready. |

**Note:** The driver does not currently expose the mouse state to userspace; it is a stub for future GUI integration.

---

## USB (xHCI + HID + Mass Storage)

LufiraOS speaks USB through an **xHCI** (eXtensible Host Controller Interface) driver — PCI class `0x0C` (Serial Bus Controller), subclass `0x03` (USB), prog‑if `0x30`. It replaces the earlier **UHCI** driver (`drivers/usb/uhci.c`/`.h`, deleted). The overall architecture is carried over deliberately from UHCI — PCI discovery, controller reset, synchronous enumeration via control transfers, boot‑protocol HID, and polling from `usb_poll()` once per PIT tick instead of real interrupts — but the underlying mechanism is completely different: instead of a Frame List of Queue Heads/Transfer Descriptors, xHCI uses a **Command Ring**, an **Event Ring**, and a per‑device **Device Context**/**Input Context**, all built from **TRBs** (Transfer Request Blocks). This version adds a second capability UHCI never had: **USB Mass Storage**.

### xHCI Host Controller (`drivers/usb/xhci.c`)

**Register model** (all accessed via the MMIO BAR0, mapped into `KERNEL_MMIO_BASE` space with a simple bump allocator since the controller is the only consumer):

- **Capability Registers** (fixed offset from BAR0) – `CAPLENGTH` (size of this block, used to locate the Operational Registers), `HCSPARAMS1` (max device slots/ports), `HCSPARAMS2` (scratchpad buffer count), `HCCPARAMS1` (context size: 32 or 64 bytes), `DBOFF`/`RTSOFF` (offsets to the Doorbell and Runtime register blocks).
- **Operational Registers** (`CAPLENGTH` bytes after BAR0) – `USBCMD`/`USBSTS` (run/stop, halted/controller‑not‑ready flags), `PAGESIZE`, `CRCR` (Command Ring Control Register — physical address + cycle state of the Command Ring), `DCBAAP` (Device Context Base Address Array Pointer), `CONFIG` (number of enabled device slots), and one `PORTSC(n)` register per root‑hub port (connect/enable/reset/speed status; the reset‑on‑write‑1 change bits are masked before any read‑modify‑write, same discipline the old UHCI driver used for its port‑status‑change bits).
- **Runtime Registers** – only Interrupter 0 is used: `ERSTSZ`/`ERSTBA` (Event Ring Segment Table size/address) and `ERDP` (Event Ring Dequeue Pointer, written after every event is consumed).
- **Doorbell Array** – one register per device slot; ringing a slot's doorbell with a target Device Context Index (DCI) tells the controller to start processing that endpoint's Transfer Ring.

**TRB rings:** the Command Ring and every endpoint's Transfer Ring are single 4 KiB pages holding 256 TRBs each. The last slot is permanently occupied by a **Link TRB** that points back to the start of the ring (255 usable slots); the Event Ring instead uses all 256 slots directly, with wraparound detected purely by the ring's Cycle bit flipping (no Link TRB needed for a single-segment Event Ring). Each ring tracks its own `cycle_state`, which every enqueued TRB's Cycle bit must match for the controller to treat it as valid.

**No real interrupts:** exactly like the AC’97 driver and the UHCI driver before it, the controller's interrupt line is never used — this kernel has no APIC/MSI‑X support and does not parse the PCI capability list at all, so there is no mechanism to receive one. Instead, `usb_poll()` is called once per PIT tick (100 Hz) from `timer_irq_handler()` in `kernel/system/timer/pit.c` (alongside `net_poll()`, see below) and drains the Event Ring by checking the Cycle bit — cheap enough that dedicating real interrupt plumbing to it was judged not worthwhile.

**Enumeration:** each root‑hub port is checked and, if a device is connected, fully enumerated (Enable Slot → Address Device → GET_DESCRIPTOR → SET_CONFIGURATION) before moving to the next port, one slot at a time — the same "no more than one device answers at once" discipline UHCI used for its default address, adapted to the fact that xHCI has no single shared "default address" but Address Device commands still must be serialized while enumeration stays synchronous.

**Two bugs found and fixed in the ring/event-handling code this release:**

1. **Link TRB Cycle‑bit bug (`xhci_ring_enqueue()`)** — the permanent Link TRB's Cycle bit was set once when the ring was initialised and never updated afterwards. On the ring's first wrap this happened to still match `cycle_state` (both start at 1), but from the *second* wrap onward the controller saw a stale Cycle bit on the Link TRB, treated it as not‑yet‑produced, and stalled at the ring boundary instead of following the link back to the start — reproducing reliably around multiples of ~255 transfers on the same ring (visible as `mount`/large-transfer stalls). Fixed by updating the Link TRB's Cycle bit synchronously with `ring->cycle_state` on every wrap, not just at ring initialisation.
2. **Event Ring race between `usb_poll()` and synchronous waits** — the Command Ring, every HID interrupt endpoint, and every Mass Storage bulk endpoint all post completions to the *same* single Event Ring. A synchronous wait for one specific completion (e.g. an MSD control or bulk transfer, or device enumeration) could have its event stolen by the periodic, timer‑tick‑driven `usb_poll()` before the waiting code observed it, producing a spurious timeout even though the controller had already responded. Fixed with an `xhci_sync_wait_depth` guard: while any synchronous wait is outstanding, `usb_poll()` does not touch the Event Ring at all; the synchronous wait path drains the ring itself and services any interleaved HID interrupt‑transfer completions inline (via the shared `xhci_service_hid_event()` helper) rather than dropping them, so keyboard/mouse input never starves while a Mass Storage transfer is in flight.

### HID Boot Protocol (keyboard/mouse)

- Looks for a boot‑protocol HID interface (keyboard or mouse) in a device's configuration descriptor during enumeration; if found, configures an interrupt IN endpoint and arms a self‑re‑arming Transfer Descriptor so `usb_poll()` picks up new reports without the device needing to be polled at the control level.
- `xhci_get_hid_device(int index)` returns the `index`‑th discovered HID device (or `NULL`). Unlike the old `uhci_get_hid_device()`, which indexed by root‑hub *port number* (UHCI had a fixed 2 ports), this indexes the array of devices *in discovery order* — xHCI controllers typically expose 4 or more ports, and nothing in the tree called the old accessor either, so this is a deliberate contract change rather than an incidental one.
- Report decoding itself is unchanged and still lives in `drivers/usb/usb_hid.c`: 8‑byte boot‑protocol keyboard reports (modifier byte + up to 6 simultaneous usage codes) and 3–4 byte boot‑protocol mouse reports (buttons + relative X/Y), translated to the same ASCII/`KEY_*` values the PS/2 driver produces and fed into `input_keyboard_event()`/`input_mouse_event()` — see [Input Dispatcher](#input-dispatcher).

### USB Mass Storage (Bulk‑Only Transport)

A device whose enumerated interface is class `0x08` (Mass Storage), subclass `0x06` (SCSI transparent command set), protocol `0x50` (Bulk‑Only Transport) gets a pair of bulk endpoints (IN + OUT) configured instead of an interrupt endpoint. Each command is a full BOT cycle over those two endpoints: a 31‑byte **CBW** (Command Block Wrapper, signature `"USBC"`) sent OUT, an optional data stage (bulk IN for reads, bulk OUT for writes), and a 13‑byte **CSW** (Command Status Wrapper, signature `"USBS"`) read IN and checked against the CBW's tag and status. The SCSI command set used is minimal: `TEST UNIT READY` and `READ CAPACITY(10)` during device init (to learn the block size and maximum LBA), and `READ(10)`/`WRITE(10)` for I/O — always exactly **one block per command**, driven one at a time by the caller; there is no request queueing or multi‑block transfer coalescing.

Public API (`drivers/usb/xhci.h`):

| Function | Description |
|----------|-------------|
| `int xhci_msd_device_count(void)` | Number of Mass Storage devices found during enumeration. |
| `int xhci_msd_get_info(int index, uint32_t *out_max_lba, uint32_t *out_block_size)` | Capacity (max LBA) and real block size (usually 512) of device `index`. Returns 0 on success, -1 for an invalid index. |
| `int xhci_msd_read_block(int index, uint32_t lba, void *buf, uint32_t block_size)` | Reads one `block_size`-byte block at `lba` via SCSI `READ(10)`. |
| `int xhci_msd_write_block(int index, uint32_t lba, const void *buf, uint32_t block_size)` | Writes one `block_size`-byte block at `lba` via SCSI `WRITE(10)`. |

There is no filesystem mounted on top of these devices by the driver itself — callers (the `usbinfo`/`usbread`/`usbwrite`/`mount`/`unmount` shell commands; see [`14_shell_commands.md`](14_shell_commands.md)) work directly in raw blocks.

### USB Hotplug

Until v0.9.0, every USB device had to already be plugged in at `xhci_init()` time — nothing re-checked the root-hub ports afterwards. `xhci_poll_hotplug()` closes that gap: called once per PIT tick from `usb_poll()` (same 100 Hz cadence as everything else that's polled in this kernel), it reads each port's `PORTSC.CSC` (Connect Status Change) bit, clears only that bit (the other RW1C bits in `PORTSC` are preserved, same discipline as `xhci_port_reset()`), and re-reads `CCS` (Current Connect Status):

- **Attach** – if the port now reports connected and has no live slot already, it runs the exact same `xhci_check_port()`/`xhci_enumerate_device()` path the boot-time scan uses.
- **Detach** – if the port reports disconnected, the matching slot (tracked by `port_id`) is torn down via `xhci_teardown_slot()` — including releasing any Mass Storage device entry that slot owned.

An `xhci_enum_busy` guard prevents `xhci_poll_hotplug()` from running concurrently with an enumeration already in progress (the boot-time scan, or another hotplug event still being serviced reentrantly through `pit_wait_ms()` inside `xhci_port_reset()`) — it simply returns immediately if one is already underway, rather than racing it.

**Limitations:** no OHCI/EHCI fallback for non‑xHCI controllers; boot‑protocol HID only (no report descriptor parsing, so multimedia keys, N‑key rollover beyond 6 keys, and non‑boot devices are unsupported); no real SuperSpeed link‑power‑management or BOS descriptor handling — devices simply run at whatever speed they report via `PORTSC`; Mass Storage is single‑LUN, single‑block‑at‑a‑time, with no filesystem support built in.

---

## Input Dispatcher

Because QEMU (and potentially real hardware) can deliver the *same* physical keystroke through both the PS/2 controller (immediately, via IRQ1) and a USB HID keyboard (polled once per timer tick, so up to ~10 ms later), `drivers/input/input.c` provides a single funnel — `input_keyboard_event()`/`input_mouse_event()` — that both drivers call into, rather than each driver talking to the shell directly.

- **Debouncing:** a duplicate of the same key arriving again within a few timer ticks is dropped, so one physical keystroke can't be processed twice (which previously caused `run`/`runbg` to execute the same command twice from a single Enter press — with the second execution starting from inside the timer IRQ and corrupting the heap).
- **Key routing:** arrow keys, Tab, Ctrl+C, Enter, and Backspace are routed to their dedicated shell handlers; anything else becomes a regular character passed to `shell_handle_char()`.

---

## Network Drivers (RTL8139 / e1000 / e1000e)

**As of v0.9.5, these drivers no longer live in the kernel.** They were ported, essentially unchanged at the hardware level, into the userspace `netd` daemon (`lufira-packages/daemons/netd/rtl8139.c`/`e1000.c`/`e1000e.c`) as part of moving the whole network stack out of the kernel binary — see [Userspace Driver Infrastructure](#userspace-driver-infrastructure) below and [`16_networking.md`](16_networking.md). This section is kept as the authoritative hardware-level reference (register layouts, boot sequences, the bugs that were found and fixed) — only the *mechanism* each driver uses to reach the hardware changed:

- Every direct kernel call the drivers used to make — `pci_find_device()`, `in8/out8`-style port I/O, `pmm_alloc_contiguous_pages()` — is now a syscall: `sys_driver_claim_device(vendor_id, device_id, &dev)` instead of PCI discovery, `sys_io_in()`/`sys_io_out()` (wrapped in the same `io_in8/16/32`/`io_out8/16/32` helper names, now in `netd/netd_hw.h`) instead of raw port I/O, and `sys_dma_alloc(num_pages, &phys)` instead of `pmm_alloc_contiguous_pages()` for the RX/TX descriptor buffers.
- Polling is no longer tied to the kernel's PIT tick. `netd` is an ordinary process now — there is no `timer_irq_handler()` hook into it — so `net_poll()` (and, under it, `rtl8139_poll()`/`e1000_poll()`/`e1000e_poll()`) runs instead from `netd`'s own `main()` loop (`daemons/netd/main.c`): wait up to 100 ms on `sys_ipc_recv()` for a client request (relayed `SYS_NET_FETCH`/`SYS_PING`/etc., see [`13_syscalls.md`](13_syscalls.md#sound-and-network-daemons-8990-9295)), handle it if one arrived, then poll the NIC — still fully polled, just driven by the daemon's own loop cadence instead of a 100 Hz interrupt.

### RTL8139 Network Driver

`daemons/netd/rtl8139.c`/`.h` drives a Realtek RTL8139 Fast Ethernet controller (PCI vendor `0x10EC`, device `0x8139`) — the NIC QEMU emulates by default (`-net nic,model=rtl8139`). This section covers the driver/hardware layer only; the Ethernet/ARP/IP/ICMP/TCP protocol stack built on top of it lives alongside it in `daemons/netd/` and is documented in [`16_networking.md`](16_networking.md).

### Discovery and register access

- Claimed via `sys_driver_claim_device(0x10EC, 0x8139, &dev)` — exact vendor/device id rather than PCI class, since RTL8139 predates the PCI class-code conventions later cards use.
- BAR0 is an **I/O-port** BAR, not MMIO — every register access goes through `io_in8`/`io_in16`/`io_in32`/`io_out8`/`io_out16`/`io_out32` (`netd/netd_hw.h`, thin wrappers over `sys_io_in()`/`sys_io_out()`) on `rtl_io_base + offset`, the same style as the PCI and AC’97 drivers always used. Key registers: `MAC0` (6-byte station address), `TSD0`/`TSAD0` (4 Transmit Status/Address Descriptors, one set per TX slot), `RBSTART` (physical base of the RX ring), `CR` (Command Register: reset/enable RX/TX/buffer-empty flags), `CAPR`/`CBR` (RX read/write cursors), `IMR`/`ISR` (interrupt mask/status — mask is always left 0, see below), `TCR`/`RCR` (transmit/receive configuration).
- `netd_pci_enable(&dev, PCI_COMMAND_IO | PCI_COMMAND_BUS_MASTER | PCI_COMMAND_INT_DISABLE)` is called during discovery (the card DMAs directly to/from system memory, and its legacy PCI INTx line is never serviced) — one read-modify-write over `SYS_PCI_CONFIG_READ`/`WRITE` replacing the kernel's old separate `pci_enable_io()`/`pci_enable_bus_master()`/`pci_disable_interrupts()` calls.

### Boot sequence

1. **Reset** – write `0x00` to `CONFIG1` to wake the device from power-down, then set the `RST` bit in `CR` and poll (up to ~1 second) until the controller clears it.
2. **Buffer allocation** – the receive path needs one **physically contiguous** buffer, because the RTL8139 has no scatter‑gather support on receive: it DMAs incoming frames directly into a single ring without any descriptor list. `sys_dma_alloc(RTL_RX_BUF_PAGES, &phys)` (3 pages — 8 KiB logical ring size plus the spec's recommended 16-byte prefetch slack and 1500 bytes of overrun room for `WRAP`-mode writes that cross the ring boundary, rounded up) gets a physically-contiguous buffer in one syscall; four separate 1‑page TX buffers (one per TX slot) are likewise each a separate `sys_dma_alloc(1, &phys)` call, since transmit doesn't need contiguity across slots.
3. **RX ring setup** – `RBSTART` is programmed with the RX buffer's physical address; `RCR` is configured to accept packets matching our MAC address and broadcasts (`APM`/`AB`), allow ring-boundary-crossing writes into the overrun slack (`WRAP`), use unlimited DMA burst size, an 8 KiB ring length, and no RX FIFO threshold (wait for a complete frame before it's handed to the driver). `CAPR` is initialised to `0 - 16` (not `0`) because the hardware always stores this register offset by −16 from the true read position — writing a bare `0` here leaves the ring permanently "non-empty" from the card's point of view and the poll loop reads uninitialised buffer contents on every pass.
4. **TX/RX enable** – `CR` is set to `RE | TE`, and the driver reads back the 6-byte station MAC from `MAC0..MAC0+5`.

### Polling and interrupts

Like xHCI and AC’97 before the v0.9.5 move, the RTL8139's interrupt line is never wired up — there is no APIC/MSI support and no PCI capability-list parsing anywhere in this project, kernel or daemon. `ISR` bits still latch in hardware regardless of `IMR` (which is left `0`), so `rtl8139_poll()` reads and acknowledges them purely in software; nothing hardware-visible depends on that acknowledgement. As described above, `rtl8139_poll()` is now called from `netd`'s own `main()` loop rather than a PIT tick. Each call drains up to 8 pending frames from the RX ring (checking the `BUFE` "buffer empty" flag in `CR` between each), parsing the 4-byte status+length header the card prepends to every frame, advancing the ring cursor, and handing the payload to `eth_receive()` for the protocol stack to process. `netd` is single-threaded with no reentrancy concern of its own (unlike the old kernel-resident version, which had to guard against being called both from a timer IRQ and synchronously), so the `cli`/`sti`-guarded re-entrancy flag the kernel driver needed is gone along with it.

### Public API (`daemons/netd/rtl8139.h`)

| Function | Description |
|----------|-------------|
| `void rtl8139_init(void)` | Claims the controller via `sys_driver_claim_device()`, resets it, allocates RX/TX buffers via `sys_dma_alloc()`, and brings up RX/TX. Safe to call even if no controller is found. |
| `int rtl8139_found(void)` | Returns 1 if a controller was found and initialised. |
| `const uint8_t *rtl8139_get_mac(void)` | Returns a pointer to the 6-byte MAC address (valid only if `rtl8139_found()`). |
| `int rtl8139_send(const void *frame, uint16_t len)` | Transmits one pre-built Ethernet frame; blocks until the hardware reports the transfer complete. Frames shorter than 60 bytes are padded up to the Ethernet minimum. |
| `void rtl8139_poll(void)` | Called once per `netd` main-loop iteration; drains pending RX frames into `eth_receive()`. |

**Limitations:** single RX ring with no automatic recovery from a desynchronised ring header (the driver simply stops parsing until the next poll, a documented simplification rather than a full reset-and-recover path); no interrupt-driven wakeups; one NIC supported at a time.

### Intel e1000 / e1000e Drivers (`daemons/netd/e1000.c`, `e1000e.c`)

A second NIC family alongside RTL8139, for the much more common Intel chipsets: `e1000.c` targets the classic Intel 8254x family (82540EM and close relatives — PCI vendor `0x8086`; QEMU's `-device e1000` emulates the 82540EM, device id `0x100E`), `e1000e.c` targets the newer PCIe-generation 8257x/8258x family (QEMU's `-device e1000e` emulates the 82574L, device id `0x10D3`) — each tries its own list of known device ids against `sys_driver_claim_device(0x8086, id, &dev)` in turn. Unlike RTL8139's I/O-port BAR0, both use an **MMIO** BAR0 (mapped via `sys_mmap_mmio()`) and the same register layout (`CTRL`/`STATUS`/`EERD`/`RCTL`/`TCTL`/`RDBAL...`/`TDBAL...`) and the same 16-byte "legacy" TX/RX descriptor format — `e1000e.c` is structurally almost identical to `e1000.c`, differing mainly in its PCI device-id list and its own `e1000e_*`-prefixed static state. Neither uses interrupts (`IMS` stays 0): `e1000_poll()`/`e1000e_poll()` check each descriptor's own `DD` (Descriptor Done) bit directly, called from `netd`'s main loop the same way `rtl8139_poll()` is.

Public API (`daemons/netd/e1000.h`/`e1000e.h`, identical shape for both): `e1000_init()`/`e1000e_init()`, `e1000_found()`/`e1000e_found()`, `e1000_get_mac()`/`e1000e_get_mac()`, `e1000_send(const void *frame, uint16_t len)`/`e1000e_send(...)`, `e1000_poll()`/`e1000e_poll()`.

### Driver Selection

`net.c` (`daemons/netd/net.c`) wraps whichever NIC is present behind one `nic_driver_t` interface, so the rest of the network stack (`eth.c` and up, see [`16_networking.md`](16_networking.md)) never has to know which of RTL8139/e1000/e1000e is actually active. `LufiraOS-Builder`'s `--net-device rtl8139|e1000|e1000e|none` picks which one a QEMU run presents (see [`05_build_system.md`](05_build_system.md)); `--driver netd` (or `--no-drivers`, see [`05_build_system.md`](05_build_system.md#driver-selection)) controls whether the `netd` daemon itself is even built into the image.

---

## PCI Bus Driver

The PCI driver enumerates all devices on the PCI bus using Configuration Mechanism #1 (ports `0xCF8`/`0xCFC`). It scans all 256 buses and up to 32 devices per bus (with multifunction detection).

### Features

- **Full bus scan** – discovers vendor ID, device ID, class code, subclass, and programming interface.
- **Header type detection** – handles multifunction devices.
- **BAR management** – reads and sizes I/O and memory BARs (both 32‑bit and 64‑bit).
- **Command register helpers** – enable I/O, memory decoding, bus mastering, and interrupt disabling.

### Public API

| Function | Description |
|----------|-------------|
| `pci_init()` | Scans the PCI bus and stores all found devices. |
| `pci_get_device_count()` | Returns number of detected devices. |
| `pci_get_device(uint32_t index)` | Returns a pointer to the `pci_device_t` structure. |
| `pci_find_device(vendor, device)` | Finds a device by vendor/device ID. |
| `pci_find_class(class, subclass)` | Finds a device by class/subclass. |
| `pci_find_class_if(class, subclass, prog_if)` | Finds a device by class/subclass/programming interface (used by xHCI, since prog-if `0x30` distinguishes it from OHCI/EHCI on the same class/subclass). |
| `pci_get_bar(dev, bar_index, &bar)` | Fills a `pci_bar_t` with address, size, and type. |
| `pci_enable_io(dev)` | Enables I/O space decoding. |
| `pci_enable_bus_master(dev)` | Enables bus mastering for DMA. |
| `pci_disable_interrupts(dev)` | Sets the Interrupt Disable bit in the command register (used by xHCI/RTL8139 since their interrupt lines are never serviced — see their respective sections). |
| `pci_enable_interrupts(dev)` | *(v0.9.5)* Clears the Interrupt Disable bit — added alongside the `cli`/`sti` fix below, for symmetry with `pci_disable_interrupts()`, though nothing in this project currently relies on a serviced PCI interrupt. |

**Fixed in v0.9.5:** `pci_config_read32()`/`pci_config_write32()` now run with interrupts disabled (`cli`/`sti`) around the `0xCF8`/`0xCFC` port pair — previously an interrupt handler touching the same ports mid-access (the two-port, write-address-then-read/write-data protocol is not atomic) could interleave with a config-space access, a suspected contributor to intermittent NVMe/xHCI hangs seen on real hardware. Not yet confirmed fixed on real hardware (QEMU never reproduced the hang in the first place), but closes a real, independently-identifiable race either way.

The PCI driver is used directly by xHCI/AHCI/NVMe (kernel-resident), and — as of v0.9.5, through the claim/permission layer described in [Userspace Driver Infrastructure](#userspace-driver-infrastructure) below rather than by calling these functions directly — by `netd` and `soundd` to locate their NIC/audio controllers.

---

## Userspace Driver Infrastructure

Added in v0.9.5 (`kernel/system/driver/driver.c`/`.h`) specifically to make moving sound and networking into userspace daemons safe. Before this, the *only* privilege check anywhere in the kernel for touching raw hardware was `uid == 0` — any root process, not just the one trusted daemon, could read/write arbitrary PCI config space, arbitrary I/O ports, or map arbitrary physical memory. That was fine while all hardware access stayed in the kernel itself, but once `netd`/`soundd` became ordinary (if root) userspace processes, "any root process" was far too broad a grant for a model where a shell someone happened to run as root shouldn't be able to reprogram the NIC underneath `netd`.

**The model:** a process must first **claim** a specific PCI device (`SYS_DRIVER_CLAIM_DEVICE`/`SYS_DRIVER_CLAIM_DEVICE_BY_CLASS`, both root-only) before any of the privileged syscalls below will do anything for it, and then only within that one device's own bounds — its own BARs, its own interrupt line. One process may hold several claims (there's no "one device per process" restriction), but each device can only ever be claimed by one process at a time; `MAX_DRIVER_CLAIMS` (8) caps how many claims can exist system-wide at once, not per process.

| Function (`driver.c`) | Description |
|------------------------|-------------|
| `driver_claim_device(pid, vendor_id, device_id, *out)` | Claims the first unclaimed device matching an exact vendor/device id (used by the NIC drivers — see [Network Drivers](#network-drivers-rtl8139--e1000--e1000e)). |
| `driver_claim_device_by_class(pid, class_code, subclass, *out)` | Claims the first unclaimed device matching a PCI class/subclass (used by the audio drivers — see [Audio Drivers](#audio-drivers-ac97--intel-hda) — since the audio class is stable across vendors/emulations while the exact device id isn't). |
| `driver_release_device(pid, bus, device, function)` | Releases one claim. |
| `driver_owns_device(pid, bus, device, function)` | Checked before every PCI config/MMIO/port-I/O/IRQ-register syscall touching that device. |
| `driver_is_registered(pid)` | True if `pid` holds *any* claim — the gate for `SYS_DMA_ALLOC`, which isn't tied to one specific device. |
| `driver_mmio_allowed(pid, phys, length)` / `driver_io_port_allowed(pid, port, width)` | True if the given physical range / I/O port range falls entirely inside one BAR of a device `pid` has claimed. |
| `driver_release_process(pid)` | Drops every claim and IRQ registration `pid` held — called from `process_exit()`/signal-driven termination, the same moment `g_wm_pid`/`g_netd_pid`/`g_soundd_pid` get cleared, so a crashed daemon doesn't permanently lock a device away from the next one that starts. |
| `driver_irq_register(pid, irq)` / `driver_irq_wait(pid, irq)` / `driver_irq_notify(irq)` | A real blocking wait on a hardware IRQ line — the same `PROCESS_BLOCKED` mechanism `mailbox_recv()` uses, not a poll loop. `driver_irq_notify()` is called from `irq_handler()` (`irq.c`) for any line with no kernel-native handler (not 0/1/12); it's a safe no-op if that line isn't actually registered. |

**The twelve syscalls built on top** (`SYS_DRIVER_CLAIM_DEVICE`/`_BY_CLASS`, `SYS_DRIVER_RELEASE_DEVICE`, `SYS_PCI_CONFIG_READ`/`WRITE`, `SYS_IO_IN`/`OUT`, `SYS_MMAP_MMIO`, `SYS_DMA_ALLOC`, `SYS_IRQ_REGISTER`/`WAIT`, `SYS_SHM_CREATE`/`ATTACH`) are documented in full — signatures, return values, error codes — in [`13_syscalls.md`](13_syscalls.md#userspace-driver-infrastructure-7788-91); this section covers only the kernel-side privilege model underneath them. Two related mechanisms introduced alongside this infrastructure are documented elsewhere: `PAGE_MMIO_NOFREE` (the PTE bit that stops `SYS_MMAP_MMIO`'s mappings from being handed back to the PMM on unmap/exit) in [`11_memory_management.md`](11_memory_management.md#page-table-hierarchy), and the shared-memory primitive `SYS_SHM_CREATE`/`ATTACH` backs, used for bulk IPC too large for the 96-byte mailbox, in [`11_memory_management.md`](11_memory_management.md#shared-memory-sys_shm_create--sys_shm_attach).

`netd` and `soundd` (`lufira-packages/daemons/`) are, as of v0.9.5, the only consumers of this infrastructure — see [Network Drivers](#network-drivers-rtl8139--e1000--e1000e) and [Audio Drivers](#audio-drivers-ac97--intel-hda) below for how each one actually uses it, and [`16_networking.md`](16_networking.md)/[`18_gui_wm.md`](18_gui_wm.md) for the daemon-registration syscalls (`SYS_NETD_REGISTER`/`SYS_SOUNDD_REGISTER`, and the pre-existing `SYS_WM_REGISTER`, now also root-only) that gate who's allowed to *be* the trusted process on the other end of `SYS_NET_FETCH`/`SYS_SOUND_*`/`SYS_WIN_*`.

---

## Audio Drivers (AC’97 / Intel HDA)

**As of v0.9.5, neither driver lives in the kernel.** Both were ported into one userspace daemon, `soundd` (`lufira-packages/daemons/soundd.c`, a single file holding both backends plus the dispatcher), as part of moving sound out of the kernel binary — see [Userspace Driver Infrastructure](#userspace-driver-infrastructure) below. The hardware-level detail below is unchanged from the kernel-resident drivers except for *how* each one reaches the hardware: `sys_driver_claim_device_by_class(0x04, 0x01, &dev)`/`sys_driver_claim_device_by_class(0x04, 0x03, &dev)` instead of `pci_find_class()` (by class/subclass rather than exact vendor/device id, since AC'97/HDA controllers vary by vendor but the class is stable), `io_in8/16/32`/`io_out8/16/32` and `mmio_read32`/`mmio_write32` (`soundd.c`'s own thin wrappers over `sys_io_in/out()`/`sys_mmap_mmio()`) instead of raw port/MMIO access, and `sys_dma_alloc()` instead of `pmm_alloc_page()` for DMA buffers.

### AC’97 Audio Driver

The AC’97 driver supports Intel ICH‑compatible audio controllers (PCI class 0x04, subclass 0x01). It provides:

- **Mixer control** – volume, sample rate, and codec information.
- **DMA‑based PCM playback** – uses a buffer descriptor list (BDL) with up to 32 pages for streaming audio.
- **Tone generation** – produces simple tones (sine/triangle wave) for beeps and testing.
- **Polling mode** – busy‑waiting for DMA completion (interrupts are disabled, same as every other driver in this project).

### Hardware Registers

- **NAM** (Native Audio Mixer) – codec registers at BAR0.
- **NABM** (Native Audio Bus Master) – DMA control registers at BAR1.

### DMA Mechanism

- One physical page for the BDL (`ac97_bdl_entry_t[32]`), from `sys_dma_alloc(1, &phys)`.
- Each entry points to a 4‑KB DMA buffer (physical address) and specifies the number of 16‑bit samples.
- Each DMA buffer is likewise `sys_dma_alloc(1, &phys)` — `soundd` has no concept of "physical memory below 4 GiB" the way the old kernel-side `pmm_alloc_page()` comment did; the syscall just hands back whatever contiguous physical pages the kernel's PMM gives it.

### Public API

| Function | Description |
|----------|-------------|
| `ac97_find_controller()`/`ac97_controller_reset()`/`ac97_codec_init()`/`ac97_alloc_dma()` | Claims the controller, resets it, initialises the codec, allocates DMA buffers — called from `soundd`'s `backend_init()`. |
| `ac97_is_available()` | Returns 1 if the driver successfully initialised. |
| `ac97_set_volume(uint8_t)` | Sets master volume (0–100). |
| `ac97_get_volume()` | Returns current volume. |
| `ac97_set_sample_rate(uint32_t)` | Sets front DAC sample rate (e.g., 48000 Hz). |
| `ac97_play_pcm_stereo(const int16_t*, uint32_t frames)` | Plays stereo 16‑bit PCM data; blocks until finished. |
| `ac97_play_tone(uint32_t frequency, uint32_t duration_ms)` | Plays a tone of given frequency and duration. |
| `ac97_stop()` | Stops any ongoing playback. |
| `ac97_read_codec(uint8_t reg)` / `ac97_write_codec()` | Direct codec register access via `io_in16`/`io_out16`. |

**Limitations:**
- Only PCM OUT channel is implemented.
- No interrupt support; playback is synchronous and blocks the caller.
- Sample rate must be supported by the codec (VRA must be enabled).

A ring-buffer producer-vs-hardware-`CIV` lookahead cap (`AC97_STREAM_MAX_LOOKAHEAD_PAGES`) bounds worst-case playback latency — previously the producer could race arbitrarily far ahead of the hardware's current-index register, adding noticeable playback delay.

### Intel HDA Audio Driver (`daemons/soundd.c`)

A second audio backend, for chipsets with no AC’97 emulation at all (common on more modern hardware). Unlike AC’97's I/O-port BARs and simple 8-byte BDL, HDA is an **MMIO** controller (mapped via `sys_mmap_mmio()`) with a separate command interface to the codec and a 16-byte BDL using 64-bit addresses. Entirely polled, like every other driver here — no controller interrupts are used, for DMA completion (`SDnSTS.BCIS`) or codec responses alike.

- **CORB/RIRB** – commands ("verbs") are sent to the codec through the **CORB** (Command Output Ring Buffer) and responses come back through the **RIRB** (Response Input Ring Buffer) — two independent, separately-polled ring buffers rather than a single synchronous register interface.
- **Codec topology discovery** – `hda_find_controller()`'s init path walks the codec's node tree using `Get Parameter`/`Get Connection List Entry` verbs, supporting a direct Pin→DAC connection or up to two hops through a Mixer/Selector; more complex topologies are a clean failure rather than a guess.
- **Sample rate** – quantized to a fixed supported set (8000/11025/16000/22050/32000/44100/48000/88200/96000/176400/192000 Hz, via the format register's BASE/MULT/DIV fields) and rounded to the nearest one — unlike AC’97's continuously adjustable VRA register.

**Fixed bug (carried over from the pre-v0.9.5 kernel driver, still relevant to the port):** `RIRBCTL`'s interrupt-enable bit (bit 0) must be set at init, or the controller never posts `RINTFL`/advances its internal RIRB read count and every verb times out (`[HDA] verb timed out`) regardless of whether an interrupt would even reach anything, since HDA is fully polled either way.

**Fixed bug (v0.9.5, continuous playback):** HDA's player previously re-armed one short buffer descriptor per `hda_play_pcm_stereo()` call, so Doom's "send one slice per game frame" calling pattern produced audible beeps rather than continuous sound. The port to `soundd` rewrote the HDA player around the same continuous-ring-buffer design AC'97 already used, fixing continuous playback over HDA.

Public API (`daemons/soundd.c`'s internal `hda_*` functions, dispatched through the same `backend_*` shape as AC'97): `hda_find_controller()`, `hda_is_available()`, `hda_set_volume()`/`hda_get_volume()`, `hda_set_sample_rate()`/`hda_get_sample_rate()`, `hda_play_pcm_stereo(const int16_t *samples, uint32_t frames)`, `hda_play_tone(uint32_t frequency, uint32_t duration_ms)`, `hda_stop()` — same shape as the AC’97 API above.

### Driver Selection

`soundd.c`'s `backend_init()` is the dispatcher both drivers sit behind: it tries `hda_find_controller()` first, then falls back to AC'97. `LufiraOS-Builder`'s `--audio ac97|hda|none` picks which controller a QEMU run presents (see [`05_build_system.md`](05_build_system.md)); `--driver soundd` (or `--no-drivers`) controls whether the `soundd` daemon itself is even built into the image.

### Kernel-Side PCM Relay (`sound_ring_write()`/`SYS_SOUND_CONSUME`)

`SYS_SOUND_PLAY_PCM` is the one piece of this subsystem that still touches the kernel directly, rather than relaying through `service_call()` like `SYS_SOUND_INFO`/`SET_VOLUME`/`STOP` do: PCM data (kilobytes per call) doesn't fit the 96-byte mailbox `service_call()` is built on. The caller writes into a kernel-resident ring buffer (`sound_ring_write()`, `syscall.c`), and `soundd` drains it with `SYS_SOUND_CONSUME` (a short, bounded-timeout read from its own main loop, so draining the PCM ring doesn't starve its control-plane mailbox — see [`13_syscalls.md`](13_syscalls.md#sound-and-network-daemons-8990-9295)) and hands whatever it got to `backend_stream_write()`.

---

## Driver Initialisation Sequence

Drivers are initialised in a specific order, driven by two hard constraints: some devices need PCI enumeration first, and AHCI/NVMe/xHCI/e1000(e)/HDA all need the PIT actually ticking (their resets are timed via `pit_wait_ms()`, not fixed busy-wait iteration counts):

1. **Serial** – `serial_init()`, deliberately the very first call in `_start()` — before the console, before the GDT — so its console-output mirror (see [Serial UART Driver](#serial-uart-driver-16550)) catches boot logging from the first line onward.
2. **Console** – `initialize_console()` uses the `BootInfo` from the bootloader.
3. **ACPI** – `acpi_init(bi->RsdpAddress)`, once LufiraFS is mounted (needed for `klog()`); see [`09_acpi.md`](09_acpi.md).
4. **HPET/RTC** – `hpet_init()` (needs ACPI's "HPET" table) and a one-shot `rtc_get_time()` read, purely informational at this point (scheduler ticks remain PIT-only).
5. **PCI** – `pci_init()` enumerates all devices.
6. **Interrupts enabled** – `sti` plus IRQ0 (timer), IRQ2 (cascade) unmasked; from this point the PIT is ticking, which every step below requires for millisecond delays during controller reset.
7. **Disk** – `disk_init()` (AHCI → NVMe → legacy ATA PIO fallback chain; see [Disk Drivers](#disk-drivers-ata-pio--ahci--nvme)), called after `sti` specifically so AHCI/NVMe's PIT-calibrated waits are available.
8. **xHCI/USB** – `xhci_init()`, before legacy PS/2 (USB HID is the more likely real input source; PS/2 is the fallback).
9. **Keyboard** – `keyboard_init()`, IRQ1 unmasked immediately after.
10. **Mouse** – `mouse_init()`, IRQ12 unmasked immediately after.
11. **Network** – *(as of v0.9.5)* no longer initialised in the kernel at all. `spawn_daemon_process("netd", "/bin/netd.elf")` starts the daemon, which itself claims a NIC (`SYS_DRIVER_CLAIM_DEVICE`, trying RTL8139/e1000/e1000e in turn), attempts DHCP, and registers via `SYS_NETD_REGISTER` — all from inside its own process, after the kernel has moved on. See [Network Drivers](#network-drivers-rtl8139--e1000--e1000e) and [`16_networking.md`](16_networking.md).
12. **Audio** – *(as of v0.9.5)* likewise no longer initialised in the kernel. `spawn_daemon_process("soundd", "/bin/soundd.elf")` starts the daemon, which claims HDA or AC'97 (`SYS_DRIVER_CLAIM_DEVICE_BY_CLASS`, HDA tried first — the same preference order the kernel's old `sound_init()` used) and registers via `SYS_SOUNDD_REGISTER`. See [Audio Drivers](#audio-drivers-ac97--intel-hda).

Interrupt handlers are registered in the IDT before enabling IRQs. See [`02_kernel_init.md`](02_kernel_init.md) for the complete, authoritative boot order.

---

## Conclusion

The LufiraOS driver subsystem provides a solid foundation for basic hardware interaction. The modular design and clear APIs make it easy to add new devices or improve existing ones. The console driver alone is powerful enough for debugging and shell interaction, while the storage and USB drivers add a full complement of local I/O to the system. **As of v0.9.5, audio and networking are no longer part of that kernel-resident set** — both moved, hardware-level logic intact, into two userspace daemons (`netd`/`soundd`) running on top of a new PCI-device-claim privilege model ([Userspace Driver Infrastructure](#userspace-driver-infrastructure)) that replaces the old "any root process can touch any hardware" assumption with one scoped to whichever process actually claimed a given device.

For more details, please refer to the source code and comments in each driver directory, and to `lufira-packages/daemons/netd/`/`daemons/soundd.c` for the drivers that used to live here.

---

**Document Version:** 3.0
**Last Updated:** October 2026
**Project:** LufiraOS
