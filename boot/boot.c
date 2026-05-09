#include <efi.h>
#include <efilib.h>
#include "utils.h"
#include "splash.h"
#include "quickboot.h"
#include "safeboot.h"
#include "debugboot.h"

EFI_HANDLE gImageHandle;

EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    InitializeLib(ImageHandle, SystemTable);
    gST = SystemTable;
    gBS = SystemTable->BootServices;
    gRT = SystemTable->RuntimeServices;
    gImageHandle = ImageHandle;
    
    SetColor(COLOR_BLACK, COLOR_BLACK);
    uefi_call_wrapper(gST->ConOut->ClearScreen, 1, gST->ConOut);
    SetColor(COLOR_WHITE, COLOR_BLACK);
    
    uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, 0, 2);
    SetColor(COLOR_NEON_PINK, COLOR_BLACK);
    Print(L"  +=============================================================+\n");
    Print(L"  |         LufiraOS Boot Mode Selection                        |\n");
    Print(L"  +=============================================================+\n\n");
    SetColor(COLOR_WHITE, COLOR_BLACK);
    Print(L"    [N] Normal Mode - Simple boot with animation\n");
    Print(L"    [D] Debug Mode  - Detailed system information\n");
    Print(L"    [S] Safe Mode   - Minimal safe boot\n\n");
    SetColor(COLOR_NEON_CYAN, COLOR_BLACK);
    Print(L"    Press N, D or S to select...\n\n");
    
    BootMode mode = MODE_NORMAL;
    EFI_INPUT_KEY Key;
    BOOLEAN keyPressed = FALSE;
    
    UINTN countdown = 5;
    while (countdown > 0) {
        uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, 6, gST->ConOut->Mode->CursorRow);
        SetColor(COLOR_NEON_PINK, COLOR_BLACK);
        Print(L"Automatic boot in %d seconds... ", countdown);
        for (int i = 0; i < 10; i++) {
            EFI_STATUS status = uefi_call_wrapper(gST->ConIn->ReadKeyStroke, 2, gST->ConIn, &Key);
            if (!EFI_ERROR(status)) { keyPressed = TRUE; break; }
            uefi_call_wrapper(gBS->Stall, 1, 100000);
        }
        if (keyPressed) break;
        countdown--;
    }
    
    if (keyPressed) {
        if (Key.UnicodeChar == L'n' || Key.UnicodeChar == L'N') mode = MODE_NORMAL;
        else if (Key.UnicodeChar == L'd' || Key.UnicodeChar == L'D') mode = MODE_DEBUG;
        else if (Key.UnicodeChar == L's' || Key.UnicodeChar == L'S') mode = MODE_SAFE;
        else mode = MODE_NORMAL;
    }
    
    BootInfo bi = {0};
    
    if (mode == MODE_NORMAL) {
        ShowSplash(MODE_NORMAL);
        QuickBoot(&bi, ImageHandle, TRUE, TRUE);
    } else if (mode == MODE_SAFE) {
        SafeBoot(&bi, ImageHandle);
    } else {
        ShowSplash(MODE_DEBUG);
        DebugBoot(&bi, ImageHandle);
    }
    
    while(1) __asm__ volatile("hlt");
}