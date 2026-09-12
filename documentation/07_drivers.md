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
7. [USB (UHCI + HID)](#usb-uhci--hid)
8. [Input Dispatcher](#input-dispatcher)
9. [PCI Bus Driver](#pci-bus-driver)
10. [AC’97 Audio Driver](#ac97-audio-driver)
11. [Driver Initialisation Sequence](#driver-initialisation-sequence)
12. [Future Extensions](#future-extensions)

---

## Overview

The driver subsystem provides hardware abstraction for essential peripherals:

- **Console** – graphical text output with a custom 8×8 font, 256‑color palette, cursor and scrollback.
- **Disk** – ATA PIO read/write for raw sector access (primary IDE channel).
- **Keyboard** – PS/2 keyboard with scancode translation, modifier handling, and IRQ1 interrupt support.
- **Mouse** – PS/2 mouse initialisation and packet decoding.
- **USB** – a UHCI host‑controller driver plus a USB HID boot‑protocol keyboard/mouse driver, unified with PS/2 through a shared input dispatcher.
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
- **`system/mm/pmm.h`** – physical memory manager for allocating DMA‑safe pages (used by AC’97).

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
- Used by higher‑level filesystem code (e.g., FAT32) if present.

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

## USB (UHCI + HID)

LufiraOS speaks USB through a **UHCI** (Universal Host Controller Interface) driver plus a **USB HID boot-protocol** decoder — enough to support the simple keyboards and mice QEMU emulates (`-device usb-kbd -device usb-mouse`), without a general-purpose USB stack.

### UHCI Host Controller (`drivers/usb/uhci.c`)

- Found via PCI (class `0x0C`, subclass `0x03`, prog-if `0x00`); disables legacy BIOS PS/2 emulation (`USBLEGSUP`) so the controller isn't fought over.
- Global and host-controller reset, followed by building a 1024-entry Frame List that permanently links a control queue head (QH) into every slot (rather than only slot 0), so control transfers don't have to wait up to ~1 second for the frame counter to wrap back around.
- Root-hub port detection and reset (2 ports), followed by standard USB enumeration (GET_DESCRIPTOR, SET_ADDRESS, GET_DESCRIPTOR again, SET_CONFIGURATION) for any connected device.
- Looks for a boot-protocol HID interface (keyboard or mouse) in the device's configuration descriptor; if found, arms a permanent interrupt transfer (a self-re-arming Transfer Descriptor on its own queue head, ahead of the control QH in the Frame List) that delivers new HID reports without polling the device from software.
- `usb_poll()`, called once per timer tick from `timer_irq_handler()`, checks whether the armed interrupt transfer(s) completed and, if so, decodes the report and re-arms them for the next one.

### USB HID Decoder (`drivers/usb/usb_hid.c`)

- Decodes 8-byte boot-protocol keyboard reports (modifier byte + up to 6 simultaneous usage codes) and 3–4 byte boot-protocol mouse reports (buttons + relative X/Y).
- Translates HID usage codes to the same ASCII/`KEY_*` values the PS/2 driver produces, and tracks Ctrl state explicitly so `Ctrl+C` and `Ctrl`+arrow history scrolling work identically regardless of which input path delivered the keystroke.
- Feeds decoded events into the same `input_keyboard_event()`/`input_mouse_event()` entry points as PS/2 — see [Input Dispatcher](#input-dispatcher) below.

**Limitations:** UHCI only (no OHCI/EHCI/xHCI); boot-protocol HID only (no report descriptor parsing, so multimedia keys, N-key rollover beyond 6 keys, and non-boot devices are unsupported); no USB mass storage.

---

## Input Dispatcher

Because QEMU (and potentially real hardware) can deliver the *same* physical keystroke through both the PS/2 controller (immediately, via IRQ1) and a USB HID keyboard (polled once per timer tick, so up to ~10 ms later), `drivers/input/input.c` provides a single funnel — `input_keyboard_event()`/`input_mouse_event()` — that both drivers call into, rather than each driver talking to the shell directly.

- **Debouncing:** a duplicate of the same key arriving again within a few timer ticks is dropped, so one physical keystroke can't be processed twice (which previously caused `run`/`runbg` to execute the same command twice from a single Enter press — with the second execution starting from inside the timer IRQ and corrupting the heap).
- **Key routing:** arrow keys, Tab, Ctrl+C, Enter, and Backspace are routed to their dedicated shell handlers; anything else becomes a regular character passed to `shell_handle_char()`.

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
| `pci_get_bar(dev, bar_index, &bar)` | Fills a `pci_bar_t` with address, size, and type. |
| `pci_enable_io(dev)` | Enables I/O space decoding. |
| `pci_enable_bus_master(dev)` | Enables bus mastering for DMA. |

The PCI driver is used by the AC’97 audio driver to locate the audio controller.

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
6. **UHCI/USB** – `uhci_init()`, after interrupts are enabled (needs the PIT ticking for real millisecond delays during controller reset).
7. **Disk** – optional; can be used by filesystem code later.

Interrupt handlers are registered in the IDT before enabling IRQs. See [`02_kernel_init.md`](02_kernel_init.md) for the complete, authoritative boot order.

---

## Conclusion

The LufiraOS driver subsystem provides a solid foundation for basic hardware interaction. The modular design and clear APIs make it easy to add new devices or improve existing ones. The console driver alone is powerful enough for debugging and shell interaction, while the audio driver adds a touch of fun to the system.

For more details, please refer to the source code and comments in each driver directory.