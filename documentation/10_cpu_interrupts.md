# CPU and Interrupts Subsystem

This document describes the CPU management and interrupt handling subsystems of LufiraOS, including the Global Descriptor Table (GDT), Interrupt Descriptor Table (IDT), Task State Segment (TSS), IRQ handling, and the Programmable Interval Timer (PIT).

---

## Table of Contents

1. [Overview](#overview)
2. [Global Descriptor Table (GDT)](#global-descriptor-table-gdt)
   - [GDT Entries](#gdt-entries)
   - [Initialisation](#initialisation)
3. [Task State Segment (TSS)](#task-state-segment-tss)
   - [TSS Structure](#tss-structure)
   - [Initialisation](#initialisation-1)
4. [Interrupt Descriptor Table (IDT)](#interrupt-descriptor-table-idt)
   - [IDT Structure](#idt-structure)
   - [Exception Handlers](#exception-handlers)
   - [Initialisation](#initialisation-2)
5. [IRQ Handling (PIC)](#irq-handling-pic)
   - [PIC Remapping](#pic-remapping)
   - [IRQ Handler](#irq-handler)
6. [Programmable Interval Timer (PIT)](#programmable-interval-timer-pit)
   - [Configuration](#configuration)
   - [Timer Handler](#timer-handler)
7. [Dependencies](#dependencies)
8. [Future Extensions](#future-extensions)

---

## Overview

The CPU and interrupts subsystem provides the foundation for:

- **Protected Mode Operation** – segmentation via the GDT for privilege level separation.
- **Exception Handling** – CPU exceptions (page faults, divide by zero, general protection faults, etc.).
- **Hardware Interrupts** – IRQ handling for peripherals (timer, keyboard, mouse).
- **Multitasking Support** – the TSS provides the Ring 0 stack pointer for user-mode transitions.
- **System Timing** – the PIT generates regular timer ticks for scheduling.

**Design Philosophy:**
- **Minimal and Correct** – only essential features are implemented.
- **Well-Documented** – each structure and function is clearly described.
- **Modular** – each component can be initialised independently.

---

## Global Descriptor Table (GDT)

The GDT is required in long mode for segmentation and privilege level management. It defines memory segments with base addresses, limits, and access permissions.

### GDT Entries

The GDT contains 8 entries:

| Index | Selector | Description |
|-------|----------|-------------|
| 0 | – | Null descriptor (required by the x86_64 architecture). |
| 1 | `0x08` | Kernel code segment (ring 0, executable, 64-bit). |
| 2 | `0x10` | Kernel data segment (ring 0, writable). |
| 3-4 | `0x18` | TSS descriptor (64-bit TSS, spanning two slots). |
| 5 | `0x2B` | User data segment (ring 3, writable). |
| 6 | `0x33` | User code segment (ring 3, executable, 64-bit). |
| 7 | `0x3B` | Additional user data segment (used for SS after `sysretq`). |

**Segment Descriptor Fields:**

| Field | Description |
|-------|-------------|
| `base` | Base address of the segment (0 for flat model). |
| `limit` | Segment limit (set to maximum). |
| `access` | Access rights (present, privilege level, type). |
| `granularity` | Granularity and 64-bit mode flags. |

### Initialisation

The GDT is initialised by `gdt_init()`:

1. Clear all entries.
2. Set up each descriptor with appropriate base, limit, access, and granularity.
3. Load the GDT using the `lgdt` instruction.
4. Reload all segment registers (`ds`, `es`, `ss`, `fs`, `gs`).
5. Perform a far jump to reload `cs` using `lretq`.

---

## Task State Segment (TSS)

The TSS is used to provide the Ring 0 stack pointer when transitioning from user mode to kernel mode (e.g., during interrupts or system calls).

### TSS Structure

| Field | Description |
|-------|-------------|
| `reserved0` | Reserved (must be 0). |
| `rsp0` | Stack pointer for privilege level 0 (kernel). |
| `rsp1` | Stack pointer for privilege level 1 (unused). |
| `rsp2` | Stack pointer for privilege level 2 (unused). |
| `reserved1` | Reserved. |
| `ist1` to `ist7` | Interrupt stack tables (unused). |
| `reserved2` | Reserved. |
| `reserved3` | Reserved. |
| `iomap_base` | I/O map base address (points past the TSS to disable I/O). |

### Initialisation

The TSS is initialised by `tss_init()`:

1. Zero the entire TSS structure.
2. Set `iomap_base` to `sizeof(tss_t)` to disable I/O port access.
3. Install the TSS descriptor in the GDT using `gdt_set_tss()`.
4. Load the Task Register (TR) with the TSS selector using `ltr`.

**Updating the TSS:** During a context switch, `tss_set_rsp0()` is called with the new process's kernel stack pointer.

---

## Interrupt Descriptor Table (IDT)

The IDT defines how the CPU handles exceptions and interrupts. It contains 256 entries, each pointing to a handler function.

### IDT Structure

| Vector Range | Type | Description |
|--------------|------|-------------|
| 0–31 | Exceptions | CPU exceptions (page fault, divide by zero, GPF, etc.). |
| 32–47 | IRQs | Hardware interrupts from the PIC (timer, keyboard, mouse). |
| 48–255 | Reserved | Default stub handler (safety net). |

**Interrupt Gate Descriptor:**

| Field | Description |
|-------|-------------|
| `offset_low` | Lower 16 bits of the handler address. |
| `selector` | Segment selector (GDT_KERNEL_CODE). |
| `ist` | Interrupt stack table index (0 for no alternate stack). |
| `type_attr` | Type and attributes (present, ring 0, interrupt gate). |
| `offset_mid` | Middle 16 bits of the handler address. |
| `offset_high` | Upper 32 bits of the handler address. |

### Exception Handlers

**Supported Exceptions:**

| Vector | Exception | Error Code |
|--------|-----------|------------|
| 0 | Divide by zero | No |
| 1 | Debug | No |
| 2 | NMI | No |
| 3 | Breakpoint | No |
| 4 | Overflow | No |
| 5 | Bound range exceeded | No |
| 6 | Invalid opcode | No |
| 7 | Device not available | No |
| 8 | Double fault | Yes |
| 9 | Coprocessor segment overrun | No |
| 10 | Invalid TSS | Yes |
| 11 | Segment not present | Yes |
| 12 | Stack-segment fault | Yes |
| 13 | General protection fault | Yes |
| 14 | Page fault | Yes |
| 15 | Reserved | No |
| 16 | x87 floating-point exception | No |
| 17 | Alignment check | Yes |
| 18 | Machine check | No |
| 19 | SIMD floating-point exception | No |
| 20 | Virtualisation exception | No |
| 21 | Control protection exception | Yes |
| 22–28 | Reserved | – |
| 29 | Hypervisor injection | No |
| 30 | VMM communication | Yes |
| 31 | Security exception | No |

**Common Handler:** `isr_common_handler()` receives an `interrupt_frame_t` with all registers and the error code (if any). It prints register dumps and exception names, then halts the system.

**Page Fault Details:** For vector 14, the handler prints CR2 (the faulting address) and decodes the error code flags.

### Initialisation

The IDT is initialised by `idt_init()`:

1. Set up entries for vectors 0–31 with their respective exception handlers.
2. Set up entries for vectors 32–47 with IRQ handlers.
3. Set up entries for vectors 48–255 with a default stub.
4. Load the IDT using the `lidt` instruction.

---

## IRQ Handling (PIC)

The Programmable Interrupt Controller (PIC) manages hardware interrupts. It is remapped to avoid conflicts with CPU exceptions.

### PIC Remapping

The PIC is remapped by `pic_remap()`:

1. Send Initialisation Command Word 1 (ICW1) to both master and slave PICs.
2. Send ICW2: master vector offset → 0x20, slave vector offset → 0x28.
3. Send ICW3: cascade configuration (master has slave at IRQ2; slave is in cascade mode).
4. Send ICW4: 8086 mode.
5. Mask all interrupts (write 0xFF to both data ports).

**Resulting Mapping:**

| IRQ | Vector | Device |
|-----|--------|--------|
| 0 | 0x20 | System Timer |
| 1 | 0x21 | Keyboard |
| 2 | 0x22 | PIC Cascade |
| 3 | 0x23 | COM2/COM4 |
| 4 | 0x24 | COM1/COM3 |
| 5 | 0x25 | LPT2 |
| 6 | 0x26 | Floppy Disk |
| 7 | 0x27 | LPT1 |
| 8 | 0x28 | Real-time Clock |
| 9 | 0x29 | ACPI |
| 10 | 0x2A | Open |
| 11 | 0x2B | Open |
| 12 | 0x2C | PS/2 Mouse |
| 13 | 0x2D | FPU |
| 14 | 0x2E | Primary IDE |
| 15 | 0x2F | Secondary IDE |

### IRQ Handler

The IRQ handler (`irq_handler()`) is called from `isr_common_handler()` for vectors 32–47:

1. Determine the IRQ number (`vector - 32`).
2. Call the appropriate device driver:
   - IRQ 0 → `timer_irq_handler()`
   - IRQ 1 → `keyboard_irq_handler()`
   - IRQ 12 → `mouse_irq_handler()`
3. Send End-Of-Interrupt (EOI) to the PIC(s).

**Enabling IRQs:**
`irq_enable()` clears the mask bit for a specific IRQ on the PIC.

**Disabling IRQs:**
`irq_disable()` sets the mask bit for a specific IRQ on the PIC.

---

## Programmable Interval Timer (PIT)

The PIT provides regular timer interrupts (IRQ0) for task scheduling and timekeeping.

### Configuration

| Parameter | Value |
|-----------|-------|
| **Base Frequency** | 1193180 Hz |
| **Desired Frequency** | 100 Hz |
| **Divisor** | 1193180 / 100 = 11931 |
| **Channel** | Channel 0 |
| **Mode** | Square wave (mode 3) |

**Initialisation:**
1. Set the command byte: channel 0, access low/high, square wave mode, binary mode.
2. Write the low byte of the divisor.
3. Write the high byte of the divisor.

### Timer Handler

The timer handler (`timer_irq_handler()`) is called on every PIT interrupt (100 times per second):

1. Increment the global tick counter (`pit_ticks`).
2. Wake up any sleeping processes (`wakeup_tick`).
3. Update the cursor blink state (`update_cursor()`).

**Scheduling:** The timer handler does not call `schedule()` directly; the scheduler is called from `kernel.c` after interrupts are re-enabled.

---

## Dependencies

| Component | Depends On | Purpose |
|-----------|------------|---------|
| GDT | Console | Logging |
| TSS | GDT | TSS descriptor installation |
| IDT | Console, GDT | Exception/IRQ handlers |
| IRQ | IDT | Hardware interrupt handling |
| PIT | IRQ, Process Manager, Console | Timer interrupts |

---

## Conclusion

The CPU and interrupts subsystem provides the essential foundation for protected mode operation, exception handling, hardware interrupts, and task scheduling. It is designed to be minimal, correct, and well-documented, forming the core of the LufiraOS kernel.

For more details, refer to the source code in `system/cpu/` and `system/timer/`.

---

**Document Version:** 1.0  
**Last Updated:** September 2026  
**Project:** LufiraOS