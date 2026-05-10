#include "quick_boot.h"
#include "ui/utils.h"
#include "system/memory.h"
#include "system/graphics.h"
#include "system/tables.h"
#include "loaders/fat_loader.h"
#include "system/exit_boot.h"
#include "loaders/kernel_loader.h"

VOID QuickBoot(BootInfo *bi, EFI_HANDLE ImageHandle, BOOLEAN keepLogo, BOOLEAN animateSpinner) {
    UINTN cols, rows;
    GetConsoleSize(&cols, &rows);
    UINTN statusRow, spinnerRow;
    
    if (keepLogo) {
        UINTN logoStart = (rows - 6) / 2;
        spinnerRow = logoStart + 9;
        statusRow = logoStart + 11;
    } else {
        uefi_call_wrapper(gST->ConOut->ClearScreen, 1, gST->ConOut);
        uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, 0, 0);
        SetColor(COLOR_DARK_RED, COLOR_BLACK);
        Print(L"LufiraOS Loading");
        statusRow = 2;
        spinnerRow = 3;
    }
    
    CHAR16 spin[] = L"|/-\\";
    UINTN spinIdx = 0;
    
    #define QUICK_UPDATE_STATUS(msg) do { \
        uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, 0, statusRow); \
        SetColor(COLOR_BLACK, COLOR_BLACK); \
        for (UINTN _i = 0; _i < cols; _i++) Print(L" "); \
        PrintCentered(msg, statusRow, COLOR_NEON_CYAN); \
        if (animateSpinner) { \
            uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, cols / 2, spinnerRow); \
            SetColor(COLOR_NEON_CYAN, COLOR_BLACK); \
            Print(L"%c", spin[spinIdx]); \
        } \
    } while(0)
    
    QUICK_UPDATE_STATUS(L"Initializing...");
    uefi_call_wrapper(gBS->Stall, 1, 500000);
    
    EFI_STATUS status = LoadKernel(bi, ImageHandle, animateSpinner, &spinIdx, statusRow, spinnerRow, cols);
    if (EFI_ERROR(status)) {
        // Fatal error - halt
        while(1) __asm__ volatile("hlt");
    }
    
    QUICK_UPDATE_STATUS(L"Reading memory map...");
    ReadMemoryMap(bi);
    
    QUICK_UPDATE_STATUS(L"Initializing graphics...");
    InitializeGraphics(bi);
    
    QUICK_UPDATE_STATUS(L"Scanning system tables...");
    ScanSystemTables(bi);
    
    QUICK_UPDATE_STATUS(L"Loading FAT image...");
    EFI_LOADED_IMAGE *LoadedImage;
    uefi_call_wrapper(gBS->HandleProtocol, 3, ImageHandle, &LoadedImageProtocol, (VOID**)&LoadedImage);
    
    EFI_BLOCK_IO_PROTOCOL *BlockIo = NULL;
    GetBlockIO(LoadedImage, &BlockIo);
    
    BOOLEAN fatError = FALSE;
    
    if (BlockIo && BlockIo->Media) {
        LoadFATImage(BlockIo, bi, FALSE);
        if (bi->FATImageBase == 0) {
            fatError = TRUE;
            if (bi->FATImageSize == 0) {
                uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, 0, 0);
                SetColor(COLOR_RED, COLOR_BLACK);
                Print(L"[ERROR] FAT: read error or allocation failed");
            } else {
                uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, 0, 0);
                SetColor(COLOR_RED, COLOR_BLACK);
                Print(L"[ERROR] FAT: unknown error");
            }
        }
    } else {
        fatError = TRUE;
        uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, 0, 0);
        SetColor(COLOR_RED, COLOR_BLACK);
        if (!BlockIo)
            Print(L"[ERROR] FAT: No Block I/O Protocol");
        else if (!BlockIo->Media)
            Print(L"[ERROR] FAT: No media present");
        else
            Print(L"[ERROR] FAT: Device not accessible");
    }
    
    if (fatError) {
        uefi_call_wrapper(gBS->Stall, 1, 2000000);
    }
    
    QUICK_UPDATE_STATUS(L"Starting kernel...");
    uefi_call_wrapper(gBS->Stall, 1, 1000000);
    
    QUICK_UPDATE_STATUS(L"Exiting boot services...");
    uefi_call_wrapper(gBS->Stall, 1, 500000);
    
    ExitBootServicesWrapper(bi, ImageHandle);
    
    KernelEntry kStart = (KernelEntry)bi->KernelBase;
    kStart(bi);
    while(1) __asm__ volatile("hlt");
}