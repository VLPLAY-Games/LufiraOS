
# Logging Macros (log.h)

This document describes the logging system used throughout the LufiraOS kernel. The logging macros provide a consistent, colour-coded interface for outputting status messages, progress indicators, and debug information.

---

## Table of Contents

1. [Overview](#overview)
2. [Colour Definitions](#colour-definitions)
3. [Status Macros](#status-macros)
4. [Progress Macros](#progress-macros)
5. [Decorative Macros](#decorative-macros)
6. [Implementation Notes](#implementation-notes)
7. [Dependencies](#dependencies)
8. [Future Extensions](#future-extensions)

---

## Overview

The logging system is defined in `log.h` and provides a set of macros that expand to formatted `printf` calls with automatic colour management. The macros are designed to be:

- **Simple to use** – one macro per log type.
- **Visually consistent** – colour codes are predefined for each log level.
- **Non-intrusive** – macros can be used anywhere in the kernel code.
- **Informative** – progress macros show pending status and overwrite lines on completion.

**Design Philosophy:**
- Every log message has a clear visual indicator of its severity or status.
- Progress operations show a pending state that is replaced with a final status when complete.
- Section headers and separators make log output easy to navigate.

---

## Colour Definitions

The logging system uses a subset of the 16-colour VGA palette defined in `colors.h`:

| Colour Alias | Palette Index | Visual Appearance | Usage |
|--------------|---------------|-------------------|-------|
| `LOG_COLOR_OK` | `COLOR_LIGHT_GREEN` | Bright green | Successful operations |
| `LOG_COLOR_FAIL` | `COLOR_LIGHT_RED` | Bright red | Errors and failures |
| `LOG_COLOR_WARN` | `COLOR_YELLOW` | Yellow | Warnings and non-critical issues |
| `LOG_COLOR_PENDING` | `COLOR_LIGHT_CYAN` | Bright cyan | In-progress operations |
| `LOG_COLOR_INFO` | `COLOR_WHITE` | White | General information |
| `LOG_COLOR_HEADER` | `COLOR_LIGHT_MAGENTA` | Bright magenta | Section titles and separators |
| `LOG_COLOR_DIM` | `COLOR_DARK_GRAY` | Dark gray | Less important information |
| `LOG_COLOR_LABEL` | `COLOR_LIGHT_CYAN` | Bright cyan | Labels in status lines |
| `STATUS_READY` | `COLOR_LIGHT_GREEN` | Bright green | Positive status indicators |
| `STATUS_NOT_READY` | `COLOR_LIGHT_RED` | Bright red | Negative status indicators |

All macros automatically reset the text colour to `COLOR_WHITE` after printing.

---

## Status Macros

### `LOG_OK(fmt, ...)`

Prints a success message with a green `[  OK  ]` prefix.

- **Used for:** Successful operations, completed tasks.
- **Colour:** Green prefix, white message.

### `LOG_FAIL(fmt, ...)`

Prints an error message with a red `[FAILED]` prefix.

- **Used for:** Critical errors, failed operations.
- **Colour:** Red prefix, white message.

### `LOG_WARN(fmt, ...)`

Prints a warning message with a yellow `[ WARN ]` prefix.

- **Used for:** Non-critical issues, fallback conditions.
- **Colour:** Yellow prefix, white message.

### `LOG_INFO_LINE(fmt, ...)`

Prints an informational message with a dim `[ INFO ]` prefix.

- **Used for:** Less important details, debug information.
- **Colour:** Dim prefix, dim message.

---

## Progress Macros

### `LOG_PENDING(fmt, ...)`

Prints a pending status message with a cyan `[  ..  ]` prefix. **Does not print a newline.**

- **Used for:** Long-running operations.
- **Colour:** Cyan prefix, white message.
- **Important:** This macro does not end the line, allowing the next macro to overwrite it.

### `LOG_DONE_OK(fmt, ...)`

Replaces the current line with a green `[  OK  ]` prefix. Overwrites the previous `LOG_PENDING` output.

- **Used for:** Successful completion of a pending operation.
- **Colour:** Green prefix, white message.
- **Important:** Must be used after `LOG_PENDING` to overwrite the same line.

### `LOG_DONE_FAIL(fmt, ...)`

Replaces the current line with a red `[FAILED]` prefix. Overwrites the previous `LOG_PENDING` output.

- **Used for:** Failed completion of a pending operation.
- **Colour:** Red prefix, white message.

### `LOG_DONE_WARN(fmt, ...)`

Replaces the current line with a yellow `[ WARN ]` prefix. Overwrites the previous `LOG_PENDING` output.

- **Used for:** Warning-level completion of a pending operation.
- **Colour:** Yellow prefix, white message.

**Progress Macro Flow:**
1. Call `LOG_PENDING()` with the operation description.
2. Perform the operation.
3. Call `LOG_DONE_OK()`, `LOG_DONE_FAIL()`, or `LOG_DONE_WARN()` with the final message.

The progress macros use a carriage return to overwrite the line, creating a smooth, in-place update effect.

---

## Decorative Macros

### `LOG_SECTION(title)`

Prints a section header in magenta.

- **Used for:** Grouping related operations, making logs more readable.
- **Colour:** Magenta text.

### `LOG_SEPARATOR`

Prints a separator line in magenta.

- **Used for:** Visual separation between sections.
- **Colour:** Magenta text.

### `LOG_STATUS_LINE(label, status, fmt, ...)`

Prints a status line with a coloured label and value.

**Parameters:**
- `label` – The component name (printed in cyan).
- `status` – A boolean value (1 for ready/OK, 0 for not ready/failed).
- `fmt, ...` – The status message with optional formatting.

- **Used for:** Showing component status in a summary section.
- **Colour:** Label in cyan, value in green or red depending on status.

---

## Implementation Notes

### How the Progress Macros Work

The progress macros achieve their in-place update effect through the following mechanism:

1. `LOG_PENDING()` prints a string without a newline.
2. Subsequent `LOG_DONE_*()` macros begin with a carriage return, which moves the cursor back to the start of the line.
3. The new message overwrites the previous one.
4. The macro then prints a newline to end the line.

This creates a smooth, in-place update effect that makes the logs look more polished and informative.

### Colour Management

All macros automatically handle colour management by:
1. Saving the current colour state (implicitly through the console driver).
2. Setting the desired foreground colour using the console driver's colour management system.
3. Printing the formatted message.
4. Restoring the default colour after the message.

The colour management functions are defined in the console driver and use the framebuffer's colour palette.

### Format String Usage

All logging macros accept `printf`-style format strings and variable arguments. This allows for convenient formatting of numeric values, addresses, and other data types.

---

## Dependencies

| Dependency | Purpose |
|------------|---------|
| `drivers/console/console.h` | Console output functions (printf, colour management) |
| `lib/colors.h` | Colour constants and definitions |
| `lib/stdarg.h` | Variable argument handling for formatted output |

The logging macros expand to `printf` calls, so the console driver must be initialised before any logging occurs. This is why console initialisation is the very first step in the kernel.

---

## Conclusion

The logging system provides a simple yet powerful way to output consistent, colour-coded messages throughout the kernel. The progress macros in particular make the boot process feel polished and professional. By standardising log output, the system makes debugging and development easier while also providing a pleasant user experience during boot.

For more details, refer to the source code in `log.h` and the console driver in `console.c`.

---

**Document Version:** 1.0  
**Last Updated:** September 2026  
**Project:** LufiraOS