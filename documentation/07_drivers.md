# LufiraOS Driver Subsystem

This document describes the device drivers included in the LufiraOS kernel. The drivers are written in C and interact directly with hardware via memory‑mapped I/O, port I/O, and PCI configuration space. They are designed to be modular, lightweight, and suitable for a hobby operating system.

---

## Table of Contents

1. [Overview](#overview)
2. [Common Infrastructure](#common-infrastructure)
3. [Console Driver](#console-driver)
4. [Disk Driver (ATA PIO)](#disk-driver-ata-pio)
5. [Keyboard Driver (PS/2)](#keyboard-driver-ps2)
6. [Mouse Driver (PS/2)](#mouse-driver-ps2)
7. [USB (xHCI + HID + Mass Storage)](#usb-xhci--hid--mass-storage)
8. [Input Dispatcher](#input-dispatcher)
9. [RTL8139 Network Driver](#rtl8139-network-driver)
10. [PCI Bus Driver](#pci-bus-driver)
11. [AC’97 Audio Driver](#ac97-audio-driver)
12. [Driver Initialisation Sequence](#driver-initialisation-sequence)

---

## Overview

The driver subsystem provides hardware abstraction for essential peripherals:

- **Console** – graphical text output with a custom 8×8 font, 256‑color palette, cursor and scrollback.
- **Disk** – ATA PIO read/write for raw sector access (primary IDE channel).
- **Keyboard** – PS/2 keyboard with scancode translation, modifier handling, and IRQ1 interrupt support.
- **Mouse** – PS/2 mouse initialisation and packet decoding.
- **USB** – an xHCI host‑controller driver providing boot‑protocol HID keyboard/mouse input and Bulk‑Only Transport USB Mass Storage (block read/write), unified with PS/2 through a shared input dispatcher.
- **Network** – an RTL8139 Fast Ethernet driver providing raw frame TX/RX to the protocol stack in `kernel/net/` (see [`16_networking.md`](16_networking.md) for Ethernet/ARP/IP/ICMP/TCP and the `ifconfig`/`ping`/`wget` shell commands built on top).
- **PCI** – bus enumeration, configuration space access, and BAR (Base Address Register) management.
- **AC’97** – audio controller (Intel ICH‑compatible) with DMA‑based playback and tone generation.

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

## Disk Driver (ATA PIO)

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

**Limitations:** no OHCI/EHCI fallback for non‑xHCI controllers; boot‑protocol HID only (no report descriptor parsing, so multimedia keys, N‑key rollover beyond 6 keys, and non‑boot devices are unsupported); no real SuperSpeed link‑power‑management or BOS descriptor handling — devices simply run at whatever speed they report via `PORTSC`; Mass Storage is single‑LUN, single‑block‑at‑a‑time, with no filesystem support built in.

---

## Input Dispatcher

Because QEMU (and potentially real hardware) can deliver the *same* physical keystroke through both the PS/2 controller (immediately, via IRQ1) and a USB HID keyboard (polled once per timer tick, so up to ~10 ms later), `drivers/input/input.c` provides a single funnel — `input_keyboard_event()`/`input_mouse_event()` — that both drivers call into, rather than each driver talking to the shell directly.

- **Debouncing:** a duplicate of the same key arriving again within a few timer ticks is dropped, so one physical keystroke can't be processed twice (which previously caused `run`/`runbg` to execute the same command twice from a single Enter press — with the second execution starting from inside the timer IRQ and corrupting the heap).
- **Key routing:** arrow keys, Tab, Ctrl+C, Enter, and Backspace are routed to their dedicated shell handlers; anything else becomes a regular character passed to `shell_handle_char()`.

---

## RTL8139 Network Driver

`drivers/net/rtl8139.c`/`.h` drives a Realtek RTL8139 Fast Ethernet controller (PCI vendor `0x10EC`, device `0x8139`) — the NIC QEMU emulates by default (`-net nic,model=rtl8139`). This section covers the driver/hardware layer only; the Ethernet/ARP/IP/ICMP/TCP protocol stack built on top of it lives in `kernel/net/` and is documented in [`16_networking.md`](16_networking.md).

### Discovery and register access

- Found via PCI by exact vendor/device ID (`pci_find_device(0x10EC, 0x8139)`) rather than by class, since RTL8139 predates the PCI class-code conventions later cards use.
- BAR0 is an **I/O-port** BAR, not MMIO — every register access goes through `in8`/`in16`/`in32`/`out8`/`out16`/`out32` on `rtl_io_base + offset`, the same style as the PCI and AC’97 drivers. Key registers: `MAC0` (6-byte station address), `TSD0`/`TSAD0` (4 Transmit Status/Address Descriptors, one set per TX slot), `RBSTART` (physical base of the RX ring), `CR` (Command Register: reset/enable RX/TX/buffer-empty flags), `CAPR`/`CBR` (RX read/write cursors), `IMR`/`ISR` (interrupt mask/status — mask is always left 0, see below), `TCR`/`RCR` (transmit/receive configuration).
- `pci_enable_io()` and `pci_enable_bus_master()` are called during discovery (the card DMAs directly to/from system memory), and `pci_disable_interrupts()` disables the legacy PCI INTx line, since it is never serviced.

### Boot sequence

1. **Reset** – write `0x00` to `CONFIG1` to wake the device from power-down, then set the `RST` bit in `CR` and poll (up to ~1 second, via `pit_wait_ms()`) until the controller clears it.
2. **Buffer allocation** – the receive path needs one **physically contiguous** buffer, because the RTL8139 has no scatter‑gather support on receive: it DMAs incoming frames directly into a single ring without any descriptor list. The existing `pmm_alloc_page()` only ever hands out one page at a time with no contiguity guarantee across calls, so this driver's arrival motivated a new allocator, `pmm_alloc_contiguous_pages(count)` (`kernel/system/mm/pmm.c`/`.h`), which scans the PMM bitmap for a run of `count` free pages and reserves it atomically. The RX ring is allocated as 3 contiguous pages (8 KiB logical ring size plus the spec's recommended 16-byte prefetch slack and 1500 bytes of overrun room for `WRAP`-mode writes that cross the ring boundary, rounded up). Four separate 1‑page buffers (one per TX slot) are allocated with plain `pmm_alloc_page()`, since transmit doesn't need contiguity across slots.
3. **RX ring setup** – `RBSTART` is programmed with the RX buffer's physical address; `RCR` is configured to accept packets matching our MAC address and broadcasts (`APM`/`AB`), allow ring-boundary-crossing writes into the overrun slack (`WRAP`), use unlimited DMA burst size, an 8 KiB ring length, and no RX FIFO threshold (wait for a complete frame before it's handed to the driver). `CAPR` is initialised to `0 - 16` (not `0`) because the hardware always stores this register offset by −16 from the true read position — writing a bare `0` here leaves the ring permanently "non-empty" from the card's point of view and the poll loop reads uninitialised buffer contents on every tick.
4. **TX/RX enable** – `CR` is set to `RE | TE`, and the driver reads back the 6-byte station MAC from `MAC0..MAC0+5`.

### Polling and interrupts

Like xHCI and AC’97, the RTL8139's interrupt line is never wired up — there is no APIC/MSI support and no PCI capability-list parsing in this kernel. `ISR` bits still latch in hardware regardless of `IMR` (which is left `0`), so `rtl8139_poll()` reads and acknowledges them purely in software; nothing hardware-visible depends on that acknowledgement. `rtl8139_poll()` is called once per PIT tick (100 Hz) from `net_poll()` (`kernel/net/net.c`), which is itself called from `timer_irq_handler()` in `kernel/system/timer/pit.c` right alongside `usb_poll()`. Each call drains up to 8 pending frames from the RX ring (checking the `BUFE` "buffer empty" flag in `CR` between each), parsing the 4-byte status+length header the card prepends to every frame, advancing the ring cursor, and handing the payload to `eth_receive()` (`kernel/net/eth.c`) for the protocol stack to process. Because the RX cursor and `CAPR` are shared mutable state and `rtl8139_poll()`/`rtl8139_send()` can be called both from inside the timer IRQ (via `net_poll()`) and synchronously from ordinary code with interrupts enabled (protocol code that busy-waits on send/receive), a `cli`/`sti`-guarded re-entrancy flag prevents a nested timer tick from processing the ring concurrently with an already-running synchronous poll.

### Public API (`drivers/net/rtl8139.h`)

| Function | Description |
|----------|-------------|
| `void rtl8139_init(void)` | Discovers the controller via PCI, resets it, allocates RX/TX buffers, and brings up RX/TX. Safe to call even if no controller is found. |
| `int rtl8139_found(void)` | Returns 1 if a controller was found and initialised. |
| `const uint8_t *rtl8139_get_mac(void)` | Returns a pointer to the 6-byte MAC address (valid only if `rtl8139_found()`). |
| `int rtl8139_send(const void *frame, uint16_t len)` | Transmits one pre-built Ethernet frame; blocks (bounded `pit_wait_ms()` timeout) until the hardware reports the transfer complete. Frames shorter than 60 bytes are padded up to the Ethernet minimum. |
| `void rtl8139_poll(void)` | Called once per PIT tick from `net_poll()`; drains pending RX frames into `eth_receive()`. |

**Limitations:** single RX ring with no automatic recovery from a desynchronised ring header (the driver simply stops parsing until the next tick, a documented simplification rather than a full reset-and-recover path); no interrupt-driven wakeups; one NIC supported at a time.

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

The PCI driver is used by the AC’97, xHCI, and RTL8139 drivers to locate their respective controllers.

---

## AC’97 Audio Driver

The AC’97 driver supports Intel ICH‑compatible audio controllers (PCI class 0x04, subclass 0x01). It provides:

- **Mixer control** – volume, sample rate, and codec information.
- **DMA‑based PCM playback** – uses a buffer descriptor list (BDL) with up to 32 pages for streaming audio.
- **Tone generation** – produces simple tones (sine/triangle wave) for beeps and testing.
- **Polling mode** – currently uses busy‑waiting for DMA completion (interrupts are disabled).

### Hardware Registers

- **NAM** (Native Audio Mixer) – codec registers at BAR0.
- **NABM** (Native Audio Bus Master) – DMA control registers at BAR1.

### DMA Mechanism

- One physical page for the BDL (`ac97_bdl_entry_t[32]`).
- Each entry points to a 4‑KB DMA buffer (physical address) and specifies the number of 16‑bit samples.
- The driver allocates DMA pages via `pmm_alloc_page()` to ensure physical memory below 4 GiB.

### Public API

| Function | Description |
|----------|-------------|
| `ac97_init()` | Probes PCI, resets controller, initialises codec, allocates DMA buffers. |
| `ac97_is_available()` | Returns 1 if the driver successfully initialised. |
| `ac97_set_volume(uint8_t)` | Sets master volume (0–100). |
| `ac97_get_volume()` | Returns current volume. |
| `ac97_set_sample_rate(uint32_t)` | Sets front DAC sample rate (e.g., 48000 Hz). |
| `ac97_play_pcm_stereo(const int16_t*, uint32_t frames)` | Plays stereo 16‑bit PCM data; blocks until finished. |
| `ac97_play_tone(uint32_t frequency, uint32_t duration_ms)` | Plays a tone of given frequency and duration. |
| `ac97_stop()` | Stops any ongoing playback. |
| `ac97_read_codec(uint8_t reg)` / `ac97_write_codec()` | Direct codec register access. |

**Limitations:**
- Only PCM OUT channel is implemented.
- No interrupt support; playback is synchronous and blocks the caller.
- Sample rate must be supported by the codec (VRA must be enabled).

---

## Driver Initialisation Sequence

Drivers are initialised in a specific order after the kernel sets up the physical memory manager and interrupt handling:

1. **Console** – `initialize_console()` uses the `BootInfo` from the bootloader (the very first step of all, before even the GDT).
2. **Keyboard** – `keyboard_init()` (IRQ1 enabled later).
3. **Mouse** – `mouse_init()` (IRQ12 enabled later).
4. **PCI** – `pci_init()` enumerates all devices.
5. **AC’97** – `ac97_init()` depends on PCI and PMM.
6. **Interrupts enabled** – `sti` plus IRQ0 (timer), IRQ1 (keyboard), IRQ2, IRQ12 (mouse) unmasked; from this point the PIT is ticking, which the next two steps require for their millisecond delays during controller reset.
7. **xHCI/USB** – `xhci_init()`, after interrupts are enabled (needs the PIT ticking for real millisecond delays during controller/port reset).
8. **Network** – `net_init()` (which calls `rtl8139_init()` and applies the static IP configuration), for the same reason as xHCI.
9. **Disk** – optional; can be used by filesystem code later.

Interrupt handlers are registered in the IDT before enabling IRQs. See [`02_kernel_init.md`](02_kernel_init.md) for the complete, authoritative boot order.

---

## Conclusion

The LufiraOS driver subsystem provides a solid foundation for basic hardware interaction. The modular design and clear APIs make it easy to add new devices or improve existing ones. The console driver alone is powerful enough for debugging and shell interaction, while the audio, USB, and networking drivers add a full complement of I/O to the system.

For more details, please refer to the source code and comments in each driver directory.

---

**Document Version:** 2.0
**Last Updated:** September 2026
**Project:** LufiraOS
