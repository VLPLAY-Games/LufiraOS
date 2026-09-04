# System Libraries

This document describes the system libraries that provide fundamental types, utilities, and helper functions used throughout the LufiraOS kernel. These libraries are located in the `kernel/lib/` directory and serve as the foundation for all other kernel components.

---

## Table of Contents

1. [Overview](#overview)
2. [Type Definitions (types.h)](#type-definitions-typesh)
3. [Standard Definitions (stddef.h)](#standard-definitions-stddefh)
4. [Variable Arguments (stdarg.h)](#variable-arguments-stdargh)
5. [Colour Definitions (colors.h)](#colour-definitions-colorsh)
6. [String Utilities (string.h / string.c)](#string-utilities-stringh--stringc)
7. [CPU Utilities (cpu.h / cpu.c)](#cpu-utilities-cpuh--cpuc)
8. [Dependencies](#dependencies)
9. [Future Extensions](#future-extensions)

---

## Overview

The system libraries provide a minimal but essential set of utilities for the kernel. They are designed to be:

- **Lightweight** – no external dependencies, suitable for freestanding environments.
- **Portable** – architecture-agnostic where possible, with x86_64-specific code isolated.
- **Self-contained** – each header can be included independently.
- **Type-safe** – using fixed-width integer types from `<stdint.h>`.

**Key Libraries:**
- **Type Definitions** – basic types, booleans, and offsets.
- **Standard Definitions** – NULL, offsetof, and size_t.
- **Variable Arguments** – va_list macros for variadic functions.
- **Colour Definitions** – 16-colour palette and RGB constants.
- **String Utilities** – token parsing, integer conversion, space skipping.
- **CPU Utilities** – interrupt flag checking.

---

## Type Definitions (types.h)

`types.h` is the primary header for fundamental data types. It includes the standard `<stdint.h>` and `<stddef.h>` headers from the compiler and adds additional type definitions.

### Included Types

| Type | Description |
|------|-------------|
| `off_t` | Signed 64-bit integer for file offsets. |
| `bool` | Boolean type (if not defined by compiler). |
| `true` | Boolean true value. |
| `false` | Boolean false value. |
| `NULL` | Null pointer constant. |
| `offsetof` | Compile-time offset of a structure member. |

### Purpose

- **Consistency** – ensures all kernel code uses the same type definitions.
- **Portability** – abstracts away compiler-specific differences.
- **Completeness** – provides missing definitions for freestanding environments.

---

## Standard Definitions (stddef.h)

`stddef.h` provides standard definitions that may be missing in freestanding environments. It includes `types.h` for `size_t` and defines:

| Definition | Description |
|------------|-------------|
| `NULL` | Null pointer constant (0). |
| `offsetof(type, member)` | Offset of a member within a structure. |

### Usage

The `offsetof` macro is used extensively in kernel data structures to calculate field offsets, particularly in VFS and process management.

---

## Variable Arguments (stdarg.h)

`stdarg.h` provides the macros for handling variable-length argument lists, used heavily by `printf`-style functions in the console driver.

### Supported Macros

| Macro | Description |
|-------|-------------|
| `va_list` | Type for the argument list state. |
| `va_start(v, l)` | Initialises `v` before the first argument. |
| `va_arg(v, t)` | Retrieves the next argument of type `t`. |
| `va_end(v)` | Cleans up the argument list. |
| `va_copy(d, s)` | Copies the argument list state. |

### Architecture Support

- **x86_64** – uses compiler built-ins (`__builtin_va_list`, `__builtin_va_start`, etc.).
- **Other Architectures** – not supported (compilation will fail).

### Dependencies

- The implementation relies on GCC built-in functions for variadic argument handling.
- No external libraries are required.

---

## Colour Definitions (colors.h)

`colors.h` defines the complete 16-colour VGA palette, RGB equivalents, and semantic aliases used throughout the kernel.

### 16-Colour Palette

The palette defines 16 standard VGA colours as an enumeration:

| Index | Constant | Description |
|-------|----------|-------------|
| 0 | `COLOR_BLACK` | Black |
| 1 | `COLOR_BLUE` | Blue |
| 2 | `COLOR_GREEN` | Green |
| 3 | `COLOR_CYAN` | Cyan |
| 4 | `COLOR_RED` | Red |
| 5 | `COLOR_MAGENTA` | Magenta |
| 6 | `COLOR_BROWN` | Brown |
| 7 | `COLOR_LIGHT_GRAY` | Light Gray |
| 8 | `COLOR_DARK_GRAY` | Dark Gray |
| 9 | `COLOR_LIGHT_BLUE` | Light Blue |
| 10 | `COLOR_LIGHT_GREEN` | Light Green |
| 11 | `COLOR_LIGHT_CYAN` | Light Cyan |
| 12 | `COLOR_LIGHT_RED` | Light Red |
| 13 | `COLOR_LIGHT_MAGENTA` | Light Magenta |
| 14 | `COLOR_YELLOW` | Yellow |
| 15 | `COLOR_WHITE` | White |
| – | `COLOR_RGB` | Special marker for direct RGB mode |

### RGB Equivalents

Each colour has a corresponding 24-bit RGB value for framebuffer output:

| Constant | RGB Value | Description |
|----------|-----------|-------------|
| `RGB_BLACK` | `0x000000` | Black |
| `RGB_BLUE` | `0x0000AA` | Blue |
| `RGB_GREEN` | `0x00AA00` | Green |
| `RGB_CYAN` | `0x00AAAA` | Cyan |
| `RGB_RED` | `0xAA0000` | Red |
| `RGB_MAGENTA` | `0xAA00AA` | Magenta |
| `RGB_BROWN` | `0xAA5500` | Brown |
| `RGB_LIGHT_GRAY` | `0xAAAAAA` | Light Gray |
| `RGB_DARK_GRAY` | `0x555555` | Dark Gray |
| `RGB_LIGHT_BLUE` | `0x5555FF` | Light Blue |
| `RGB_LIGHT_GREEN` | `0x55FF55` | Light Green |
| `RGB_LIGHT_CYAN` | `0x55FFFF` | Light Cyan |
| `RGB_LIGHT_RED` | `0xFF5555` | Light Red |
| `RGB_LIGHT_MAGENTA` | `0xFF55FF` | Light Magenta |
| `RGB_YELLOW` | `0xFFFF55` | Yellow |
| `RGB_WHITE` | `0xFFFFFF` | White |

### Semantic Logging Aliases

| Alias | Palette Index | Purpose |
|-------|---------------|---------|
| `LOG_COLOR_OK` | `COLOR_LIGHT_GREEN` | Success messages |
| `LOG_COLOR_FAIL` | `COLOR_LIGHT_RED` | Error messages |
| `LOG_COLOR_WARN` | `COLOR_YELLOW` | Warning messages |
| `LOG_COLOR_PENDING` | `COLOR_LIGHT_CYAN` | In-progress operations |
| `LOG_COLOR_INFO` | `COLOR_WHITE` | General information |
| `LOG_COLOR_HEADER` | `COLOR_LIGHT_MAGENTA` | Section headers |
| `LOG_COLOR_DIM` | `COLOR_DARK_GRAY` | Dim/less important text |
| `LOG_COLOR_LABEL` | `COLOR_LIGHT_CYAN` | Labels and keys |
| `STATUS_READY` | `COLOR_LIGHT_GREEN` | Ready status |
| `STATUS_NOT_READY` | `COLOR_LIGHT_RED` | Not ready status |

### Usage

The colour constants are used throughout the kernel:
- Console driver for setting text colours.
- Logging macros for consistent status output.
- Shell commands for user-configurable colours.

---

## String Utilities (string.h / string.c)

The string utilities provide simple, zero-allocation string parsing and conversion functions, primarily used by the shell and command parsers.

### Functions

| Function | Description |
|----------|-------------|
| `skip_spaces(s)` | Returns a pointer to the first non-space character. |
| `token_length(s)` | Returns the length of the next token (space-delimited). |
| `token_equals(s, word)` | Checks if `s` starts with `word` followed by a delimiter. |
| `atoi(str)` | Converts a decimal string to an integer. |
| `hex_to_int(hex)` | Converts a hexadecimal string to an integer. |

### `skip_spaces`

- **Purpose:** Skip leading spaces and tabs in a string.
- **Returns:** Pointer to the first non-space character.
- **Use Case:** Parsing user input, skipping indentation.

### `token_length`

- **Purpose:** Determine the length of the next token.
- **Returns:** Number of characters until a space or tab.
- **Use Case:** Extracting command names and arguments.

### `token_equals`

- **Purpose:** Compare a string with a token (case-sensitive).
- **Returns:** Non-zero if the token matches `word`.
- **Use Case:** Command matching in the shell.

### `atoi`

- **Purpose:** Convert a decimal string to an integer.
- **Returns:** Integer value (no overflow handling).
- **Use Case:** Parsing numeric arguments (e.g., `kill 123`).

### `hex_to_int`

- **Purpose:** Convert a hexadecimal string to an integer.
- **Returns:** Integer value (no overflow handling).
- **Use Case:** Parsing colour values (e.g., `color FF0000`).

### Implementation Notes

- All functions are safe (they do not modify the input string).
- No dynamic memory allocation is performed.
- `atoi` and `hex_to_int` do not handle overflow and are intended for small values.

---

## CPU Utilities (cpu.h / cpu.c)

The CPU utilities provide low-level CPU state inspection.

### Functions

| Function | Description |
|----------|-------------|
| `interrupts_enabled()` | Returns 1 if interrupts are enabled, 0 otherwise. |

### `interrupts_enabled`

- **Purpose:** Read the interrupt flag (IF) from RFLAGS.
- **Implementation:** Uses `pushfq`/`pop` to read RFLAGS and checks bit 9.
- **Use Case:** Displaying interrupt state in `status` command, debugging.

### Architecture Support

- **x86_64** – uses inline assembly (`pushfq`, `pop`) to read RFLAGS.
- **Other Architectures** – not supported (compilation will fail).

---

## Dependencies

| Library | Depends On | Purpose |
|---------|------------|---------|
| `types.h` | `<stdint.h>`, `<stddef.h>` | Standard integer types |
| `stddef.h` | `types.h` | `size_t` for offsetof |
| `stdarg.h` | Compiler built-ins | Variadic argument handling |
| `colors.h` | None | Self-contained |
| `string.h` | None | Self-contained |
| `string.c` | None | Self-contained |
| `cpu.h` | `types.h` | `bool` type for return value |
| `cpu.c` | `cpu.h` | Function implementation |


---

## Conclusion

The system libraries provide the essential building blocks for the LufiraOS kernel. They abstract away architecture-specific details, define consistent types and colours, and offer safe string-parsing utilities. Their simplicity makes them easy to maintain and extend as the kernel evolves.

For more details, refer to the source code in the `kernel/lib/` directory.

---

**Document Version:** 1.0  
**Last Updated:** September 2026  
**Project:** LufiraOS