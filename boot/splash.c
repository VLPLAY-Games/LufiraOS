#include "splash.h"
#include "utils.h"

VOID ShowSplash(BootMode mode) {
    uefi_call_wrapper(gST->ConOut->ClearScreen, 1, gST->ConOut);
    UINTN cols, rows;
    GetConsoleSize(&cols, &rows);
    
    CHAR16 *logo[] = {
        L"██╗     ██╗   ██╗███████╗██╗██████╗  █████╗     ███████╗███████╗",
        L"██║     ██║   ██║██╔════╝██║██╔══██╗██╔══██╗    ██═══██╝██╔════╝",
        L"██║     ██║   ██║█████╗  ██║██████╔╝███████║    ██═══██╗███████╗",
        L"██║     ██║   ██║██╔══╝  ██║██╔══██╗██╔══██║    ██═══██║╚════██║",
        L"███████╗╚██████╔╝██║     ██║██║  ██║██║  ██║    ███████║███████║",
        L"╚══════╝ ╚═════╝ ╚═╝     ╚═╝╚═╝  ╚═╝╚═╝  ╚═╝    ╚══════╝╚══════╝"
    };
    
    UINTN startRow = (rows - 6) / 2;
    for (int i = 0; i < 6; i++) {
        PrintCentered(logo[i], startRow + i, COLOR_NEON_CYAN);
    }
    if (mode == MODE_DEBUG) {
        PrintCentered(L"[ DEBUG MODE ]", startRow + 7, COLOR_NEON_PINK);
    }
    
    CHAR16 spin[] = L"|/-\\";
    UINTN spinnerCol = cols / 2;
    UINTN spinnerRow = startRow + 9;
    
    for (int i = 0; i < 20; i++) {
        uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, spinnerCol, spinnerRow);
        SetColor(COLOR_NEON_CYAN, COLOR_BLACK);
        Print(L"%c", spin[i % 4]);
        uefi_call_wrapper(gBS->Stall, 1, 100000);
    }
    uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, spinnerCol, spinnerRow);
    Print(L" ");
}