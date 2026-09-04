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
7. [PCI Bus Driver](#pci-bus-driver)
8. [AC’97 Audio Driver](#ac97-audio-driver)
9. [Driver Initialisation Sequence](#driver-initialisation-sequence)
10. [Future Extensions](#future-extensions)

---

## Overview

The driver subsystem provides hardware abstraction for essential peripherals:

- **Console** – graphical text output with a custom 8×8 font, 256‑color palette, cursor and scrollback.
- **Disk** – ATA PIO read/write for raw sector access (primary IDE channel).
- **Keyboard** – PS/2 keyboard with scancode translation, modifier handling, and IRQ1 interrupt support.
- **Mouse** – PS/2 mouse initialisation and packet decoding.
- **PCI** – bus enumeration, configuration space access, and BAR (Base Address Register) management.
- **AC’97** – audio controller (Intel ICH‑compatible) with DMA‑based playback and tone generation.

Drivers are designed to be initialised early in the kernel boot process, after the physical memory manager (PMM) and interrupt descriptor table (IDT) are set up. All drivers are polled or interrupt‑driven; the console is used for debugging and user interaction.

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

1. **PCI** – `pci_init()` enumerates all devices.
2. **Console** – `initialize_console()` uses the `BootInfo` from the bootloader.
3. **Keyboard** – `keyboard_init()` (IRQ1 enabled later).
4. **Mouse** – `mouse_init()` (IRQ12 enabled later).
5. **AC’97** – `ac97_init()` depends on PCI and PMM.
6. **Disk** – optional; can be used by filesystem code later.

Interrupt handlers are registered in the IDT before enabling IRQs.

---

## Future Extensions

- **AHCI/SATA** – replace ATA PIO with a more modern driver.
- **USB** – HID and mass storage support.
- **Interrupt‑driven audio** – replace polling with completion interrupts.
- **Framebuffer acceleration** – improve console performance with double‑buffering.
- **Mouse cursor overlay** – integrate mouse pointer with the console/GUI.
- **Power management** – suspend/resume for audio and PCI devices.

---

## Conclusion

The LufiraOS driver subsystem provides a solid foundation for basic hardware interaction. The modular design and clear APIs make it easy to add new devices or improve existing ones. The console driver alone is powerful enough for debugging and shell interaction, while the audio driver adds a touch of fun to the system.

For more details, please refer to the source code and comments in each driver directory.