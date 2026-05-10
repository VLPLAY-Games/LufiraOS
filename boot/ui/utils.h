#pragma once

#include <efi.h>
#include <efilib.h>

// Цветовая палитра
#define COLOR_BLACK         0x00
#define COLOR_BLUE          0x01
#define COLOR_GREEN         0x02
#define COLOR_CYAN          0x03
#define COLOR_RED           0x04
#define COLOR_MAGENTA       0x05
#define COLOR_YELLOW        0x06
#define COLOR_WHITE         0x07
#define COLOR_LIGHTGRAY     0x08
#define COLOR_DARKGRAY      0x08
#define COLOR_LIGHTBLUE     0x09
#define COLOR_LIGHTGREEN    0x0A
#define COLOR_LIGHTCYAN     0x0B
#define COLOR_LIGHTRED      0x0C
#define COLOR_LIGHTMAGENTA  0x0D
#define COLOR_LIGHTYELLOW   0x0E
#define COLOR_BRIGHTWHITE   0x0F

#define COLOR_NEON_PINK     COLOR_LIGHTMAGENTA
#define COLOR_NEON_CYAN     COLOR_LIGHTCYAN
#define COLOR_NEON_GREEN    COLOR_LIGHTGREEN
#define COLOR_DARK_RED      COLOR_RED
#define COLOR_DIM_GRAY      COLOR_DARKGRAY

VOID SetColor(UINTN Foreground, UINTN Background);
VOID PrintColored(CONST CHAR16 *String, UINTN Foreground, UINTN Background);
VOID GetConsoleSize(UINTN *Cols, UINTN *Rows);
VOID PrintCentered(CONST CHAR16 *Str, UINTN Row, UINTN Color);
