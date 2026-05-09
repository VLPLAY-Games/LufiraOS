#include "safeboot.h"
#include "utils.h"
#include "memory.h"
#include "graphics.h"
#include "tables.h"
#include "fatloader.h"
#include "exitbs.h"
#include "kernel.h"

VOID SafeBoot(BootInfo *bi, EFI_HANDLE ImageHandle) {
    uefi_call_wrapper(gST->ConOut->ClearScreen, 1, gST->ConOut);
    
    // Safe mode header
    uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, 0, 0);
    SetColor(COLOR_DARK_RED, COLOR_BLACK);
    Print(L"[Safe mode]");
    uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, 0, 1);
    Print(L"LufiraOS Loading");
    
    // Load kernel
    EFI_STATUS status = LoadKernel(bi, ImageHandle, FALSE, NULL, 0, 0, 0);
    if (EFI_ERROR(status)) {
        uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, 0, 3);
        SetColor(COLOR_RED, COLOR_BLACK);
        Print(L"[FATAL] Cannot load kernel");
        while(1) __asm__ volatile("hlt");
    }
    
    // Memory map
    ReadMemoryMap(bi);
    
    // Graphics
    InitializeGraphics(bi);
    
    // System tables
    ScanSystemTables(bi);
    
    // FAT image (optional in safe mode, just try)
    EFI_LOADED_IMAGE *LoadedImage;
    uefi_call_wrapper(gBS->HandleProtocol, 3, ImageHandle, &LoadedImageProtocol, (VOID**)&LoadedImage);
    
    EFI_BLOCK_IO_PROTOCOL *BlockIo = NULL;
    GetBlockIO(LoadedImage, &BlockIo);
    
    if (BlockIo && BlockIo->Media) {
        LoadFATImage(BlockIo, bi, FALSE);
    } else {
        bi->FATImageBase = 0;
        bi->FATImageSize = 0;
    }
    
    // Exit boot services and boot
    ExitBootServicesWrapper(bi, ImageHandle);
    
    KernelEntry kStart = (KernelEntry)bi->KernelBase;
    kStart(bi);
    while(1) __asm__ volatile("hlt");
}