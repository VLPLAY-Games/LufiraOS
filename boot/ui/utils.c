#include "utils.h"

VOID SetColor(UINTN Foreground, UINTN Background) {
    uefi_call_wrapper(gST->ConOut->SetAttribute, 2, gST->ConOut, 
                     (Foreground & 0x0F) | ((Background & 0x0F) << 4));
}

VOID PrintColored(CONST CHAR16 *String, UINTN Foreground, UINTN Background) {
    SetColor(Foreground, Background);
    Print(String);
    SetColor(COLOR_WHITE, COLOR_BLACK);
}

VOID GetConsoleSize(UINTN *Cols, UINTN *Rows) {
    UINTN Mode = gST->ConOut->Mode->Mode;
    EFI_STATUS status = uefi_call_wrapper(gST->ConOut->QueryMode, 4, gST->ConOut, Mode, Cols, Rows);
    if (EFI_ERROR(status)) {
        *Cols = 80;
        *Rows = 25;
    }
}

VOID PrintCentered(CONST CHAR16 *Str, UINTN Row, UINTN Color) {
    UINTN cols, rows;
    GetConsoleSize(&cols, &rows);
    UINTN len = StrLen(Str);
    UINTN x = (cols > len) ? (cols - len) / 2 : 0;
    uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, x, Row);
    SetColor(Color, COLOR_BLACK);
    Print(Str);
}