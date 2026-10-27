#include "safe_boot.h"
#include "ui/utils.h"
#include "system/memory.h"
#include "system/graphics.h"
#include "system/tables.h"
#include "system/exit_boot.h"
#include "loaders/kernel_loader.h"

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
    
    // v1.0.0 план: ядро больше не читает LufiraFS из предзагруженного в
    // RAM образа диска (см. kernel/kernel.c — lufirafs_init() сам читает
    // с диска через свой AHCI/NVMe/ATA-драйвер, найдя партицию по MBR) —
    // LoadFATImage()/GetBlockIO() (fat_loader.c) больше не нужны ни в
    // одном boot mode, поля оставлены в BootInfo нулевыми (не удалены —
    // менять layout структуры без необходимости рискованно).
    bi->FATImageBase = 0;
    bi->FATImageSize = 0;

    // Exit boot services and boot
    ExitBootServicesWrapper(bi, ImageHandle);
    
    KernelEntry kStart = (KernelEntry)bi->KernelBase;
    kStart(bi);
    while(1) __asm__ volatile("hlt");
}