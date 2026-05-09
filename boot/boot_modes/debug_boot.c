#include "debug_boot.h"
#include "../ui/utils.h"
#include "../system/memory.h"
#include "../system/graphics.h"
#include "../system/tables.h"
#include "../loaders/fat_loader.h"
#include "../system/exit_boot.h"
#include "../loaders/kernel_loader.h"
#include "../ui/splash.h"

VOID DebugBoot(BootInfo *bi, EFI_HANDLE ImageHandle) {
    uefi_call_wrapper(gST->ConOut->ClearScreen, 1, gST->ConOut);
    SetColor(COLOR_WHITE, COLOR_BLACK);
    Print(L"LufiraOS Debug Mode\n\n");
    
    #define LOG_OK(fmt, ...) do { \
        SetColor(COLOR_NEON_GREEN, COLOR_BLACK); Print(L"[  OK  ] "); \
        SetColor(COLOR_WHITE, COLOR_BLACK); Print(fmt, ##__VA_ARGS__); Print(L"\n"); \
    } while(0)
    #define LOG_FAIL(fmt, ...) do { \
        SetColor(COLOR_RED, COLOR_BLACK); Print(L"[FAILED] "); \
        SetColor(COLOR_WHITE, COLOR_BLACK); Print(fmt, ##__VA_ARGS__); Print(L"\n"); \
    } while(0)
    #define LOG_WARN(fmt, ...) do { \
        SetColor(COLOR_YELLOW, COLOR_BLACK); Print(L"[ WARN ] "); \
        SetColor(COLOR_WHITE, COLOR_BLACK); Print(fmt, ##__VA_ARGS__); Print(L"\n"); \
    } while(0)
    #define LOG_INFO(fmt, ...) do { \
        SetColor(COLOR_NEON_CYAN, COLOR_BLACK); Print(L"  "); \
        SetColor(COLOR_WHITE, COLOR_BLACK); Print(fmt, ##__VA_ARGS__); Print(L"\n"); \
    } while(0)
    
    LOG_INFO(L"System Information:");
    LOG_INFO(L"  Firmware: %s", gST->FirmwareVendor);
    LOG_INFO(L"  UEFI %d.%02d", gST->Hdr.Revision >> 16, gST->Hdr.Revision & 0xFFFF);
    
    EFI_GUID gEfiGlobalVariableGuid = {0x8BE4DF61, 0x93CA, 0x11D2, {0xAA, 0x0D, 0x00, 0xE0, 0x98, 0x03, 0x2B, 0x8C}};
    UINT8 SecureBoot;
    UINTN DataSize = sizeof(SecureBoot);
    EFI_STATUS sb_status = uefi_call_wrapper(gRT->GetVariable, 5, L"SecureBoot", &gEfiGlobalVariableGuid, NULL, &DataSize, &SecureBoot);
    if (!EFI_ERROR(sb_status))
        LOG_INFO(L"  Secure Boot: %s", SecureBoot ? L"Enabled" : L"Disabled");
    else
        LOG_INFO(L"  Secure Boot: Not Supported");
    
    EFI_TIME Time;
    if (!EFI_ERROR(uefi_call_wrapper(gRT->GetTime, 2, &Time, NULL)))
        LOG_INFO(L"  System Time: %02d/%02d/%04d %02d:%02d:%02d", Time.Day, Time.Month, Time.Year, Time.Hour, Time.Minute, Time.Second);
    
    ReadMemoryMap(bi);
    LOG_INFO(L"  Available Memory: %ld MB", bi->TotalMemory / (1024 * 1024));
    
    EFI_STATUS status = InitializeGraphics(bi);
    if (!EFI_ERROR(status)) {
        LOG_INFO(L"  Video: %dx%d (%s)", bi->HorizontalResolution, bi->VerticalResolution, bi->PixelFormat == 1 ? L"BGR" : L"RGB");
    } else {
        LOG_WARN(L"  Video: Not Available");
    }
    
    ScanSystemTables(bi);
    LOG_INFO(L"  RSDP: %s", bi->RsdpAddress ? L"Found" : L"Not Found");
    LOG_INFO(L"  SMBIOS: %s", bi->SmbiosAddress ? L"Found" : L"Not Found");
    
    Print(L"\n");
    
    LOG_OK(L"Loading kernel...");
    EFI_LOADED_IMAGE *LoadedImage;
    status = uefi_call_wrapper(gBS->HandleProtocol, 3, ImageHandle, &LoadedImageProtocol, (VOID**)&LoadedImage);
    if (EFI_ERROR(status)) { LOG_FAIL(L"Cannot get LoadedImage"); while(1) __asm__ volatile("hlt"); }
    
    EFI_FILE_IO_INTERFACE *FileSystem;
    status = uefi_call_wrapper(gBS->HandleProtocol, 3, LoadedImage->DeviceHandle, &FileSystemProtocol, (VOID**)&FileSystem);
    if (EFI_ERROR(status)) { LOG_FAIL(L"Cannot get FileSystem"); while(1) __asm__ volatile("hlt"); }
    
    EFI_FILE_HANDLE Root;
    uefi_call_wrapper(FileSystem->OpenVolume, 2, FileSystem, &Root);
    
    EFI_FILE_HANDLE KernelFile;
    status = uefi_call_wrapper(Root->Open, 5, Root, &KernelFile, L"kernel.bin", EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(status)) { LOG_FAIL(L"kernel.bin not found"); while(1) __asm__ volatile("hlt"); }
    
    EFI_FILE_INFO *KernelFileInfo;
    UINTN KernelFileInfoSize = sizeof(EFI_FILE_INFO) + 256;
    uefi_call_wrapper(gBS->AllocatePool, 3, EfiLoaderData, KernelFileInfoSize, (VOID**)&KernelFileInfo);
    uefi_call_wrapper(KernelFile->GetInfo, 4, KernelFile, &gEfiFileInfoGuid, &KernelFileInfoSize, KernelFileInfo);
    bi->KernelSize = KernelFileInfo->FileSize;
    uefi_call_wrapper(gBS->FreePool, 1, KernelFileInfo);
    
    UINTN Pages = (bi->KernelSize + 4095) / 4096;
    EFI_PHYSICAL_ADDRESS KernelBase = 0x100000;
    status = uefi_call_wrapper(gBS->AllocatePages, 4,
        AllocateAddress, EfiLoaderData, Pages, &KernelBase);
    
    if (EFI_ERROR(status)) {
        status = uefi_call_wrapper(gBS->AllocatePages, 4,
            AllocateAnyPages, EfiLoaderData, Pages, &KernelBase);
    }
    
    if (EFI_ERROR(status)) {
        LOG_FAIL(L"Cannot allocate kernel memory");
    }
    
    bi->KernelBase = KernelBase;
    LOG_OK(L"Kernel memory allocated at 0x%lx", KernelBase);
    
    UINTN ChunkSize = 65536, TotalLoaded = 0;
    while (TotalLoaded < bi->KernelSize) {
        UINTN ToRead = (bi->KernelSize - TotalLoaded) > ChunkSize ? ChunkSize : (bi->KernelSize - TotalLoaded);
        uefi_call_wrapper(KernelFile->Read, 3, KernelFile, &ToRead, (VOID*)(KernelBase + TotalLoaded));
        TotalLoaded += ToRead;
    }
    LOG_OK(L"Kernel loaded (%ld KB)", bi->KernelSize / 1024);
    
    LOG_INFO(L"Loading FAT image...");
    EFI_BLOCK_IO_PROTOCOL *BlockIo = NULL;
    GetBlockIO(LoadedImage, &BlockIo);
    
    if (BlockIo && BlockIo->Media) {
        LoadFATImage(BlockIo, bi, FALSE);
        if (bi->FATImageBase)
            LOG_OK(L"FAT image loaded (%ld KB)", bi->FATImageSize / 1024);
        else if (bi->FATImageSize == 0 && bi->FATImageBase == 0)
            LOG_FAIL(L"FAT image not loaded - read error or allocation failed");
        else
            LOG_FAIL(L"FAT image not loaded - unknown error");
    } else {
        if (!BlockIo)
            LOG_FAIL(L"No Block I/O Protocol available");
        else if (!BlockIo->Media)
            LOG_FAIL(L"Block I/O media not present");
        else
            LOG_FAIL(L"Block I/O device not accessible");
    }
    
    // Memory Map Dump
    EFI_INPUT_KEY Key;
    
    Print(L"\nPress any key for Memory Map dump...");
    while (uefi_call_wrapper(gST->ConIn->ReadKeyStroke, 2, gST->ConIn, &Key) != EFI_SUCCESS);
    uefi_call_wrapper(gST->ConOut->ClearScreen, 1, gST->ConOut);
    SetColor(COLOR_WHITE, COLOR_BLACK);
    Print(L"Memory Map (descriptor size: %d bytes)\n\n", bi->MemoryMapDescriptorSize);
    SetColor(COLOR_NEON_CYAN, COLOR_BLACK);
    Print(L"Type                                     Physical Start   Pages          Attributes\n");
    SetColor(COLOR_DARK_RED, COLOR_BLACK);
    Print(L"--------------------------------------- ----------------- -------------- -----------------\n");
    
    UINTN descCount = bi->MemoryMapSize / bi->MemoryMapDescriptorSize;
    EFI_MEMORY_DESCRIPTOR *MemoryMap = (EFI_MEMORY_DESCRIPTOR*)bi->MemoryMap;
    
    for (UINTN i = 0; i < descCount; i++) {
        EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR*)((UINT8*)MemoryMap + (i * bi->MemoryMapDescriptorSize));
        CONST CHAR16 *typeStr;
        switch (d->Type) {
            case EfiReservedMemoryType:   typeStr = L"Reserved"; break;
            case EfiLoaderCode:           typeStr = L"LoaderCode"; break;
            case EfiLoaderData:           typeStr = L"LoaderData"; break;
            case EfiBootServicesCode:     typeStr = L"BS Code"; break;
            case EfiBootServicesData:     typeStr = L"BS Data"; break;
            case EfiRuntimeServicesCode:  typeStr = L"RT Code"; break;
            case EfiRuntimeServicesData:  typeStr = L"RT Data"; break;
            case EfiConventionalMemory:   typeStr = L"Free"; break;
            case EfiUnusableMemory:       typeStr = L"Unusable"; break;
            case EfiACPIReclaimMemory:    typeStr = L"ACPI Reclaim"; break;
            case EfiACPIMemoryNVS:        typeStr = L"ACPI NVS"; break;
            case EfiMemoryMappedIO:       typeStr = L"MMIO"; break;
            case EfiMemoryMappedIOPortSpace: typeStr = L"MMIO Port"; break;
            case EfiPalCode:              typeStr = L"PAL Code"; break;
            default:                      typeStr = L"Unknown"; break;
        }
        SetColor(COLOR_WHITE, COLOR_BLACK);
        Print(L"%-38s %016lx %14ld %016lx\n", typeStr, d->PhysicalStart, d->NumberOfPages, d->Attribute);
    }
    
    Print(L"\nPress any key for Configuration Tables...");
    while (uefi_call_wrapper(gST->ConIn->ReadKeyStroke, 2, gST->ConIn, &Key) != EFI_SUCCESS);
    uefi_call_wrapper(gST->ConOut->ClearScreen, 1, gST->ConOut);
    SetColor(COLOR_WHITE, COLOR_BLACK);
    Print(L"Configuration Tables (%d entries)\n\n", gST->NumberOfTableEntries);
    SetColor(COLOR_NEON_CYAN, COLOR_BLACK);
    Print(L"  #  GUID                                  Address\n");
    SetColor(COLOR_DARK_RED, COLOR_BLACK);
    Print(L" --- ------------------------------------- ----------------\n");
    for (UINTN i = 0; i < gST->NumberOfTableEntries; i++) {
        CHAR16 guidStr[40];
        GuidToString(guidStr, &gST->ConfigurationTable[i].VendorGuid);
        SetColor(COLOR_WHITE, COLOR_BLACK);
        Print(L"  %2d %-37s %016lx\n", i, guidStr, gST->ConfigurationTable[i].VendorTable);
    }
    
    Print(L"\nPress any key for Graphics Output Details...");
    while (uefi_call_wrapper(gST->ConIn->ReadKeyStroke, 2, gST->ConIn, &Key) != EFI_SUCCESS);
    uefi_call_wrapper(gST->ConOut->ClearScreen, 1, gST->ConOut);
    SetColor(COLOR_WHITE, COLOR_BLACK);
    Print(L"Graphics Output Protocol\n\n");
    LOG_INFO(L"  FrameBufferBase: 0x%lx", bi->FrameBufferBase);
    LOG_INFO(L"  FrameBufferSize: %ld MB", bi->FrameBufferSize / 1024 / 1024);
    LOG_INFO(L"  Resolution: %d x %d", bi->HorizontalResolution, bi->VerticalResolution);
    LOG_INFO(L"  PixelsPerScanLine: %d", bi->PixelsPerScanLine);
    LOG_INFO(L"  PixelFormat: %s", bi->PixelFormat == 1 ? L"BGR" : L"RGB");
    
    Print(L"\nPress any key to continue...");
    while (uefi_call_wrapper(gST->ConIn->ReadKeyStroke, 2, gST->ConIn, &Key) != EFI_SUCCESS);
    uefi_call_wrapper(gST->ConOut->ClearScreen, 1, gST->ConOut);
    
    SetColor(COLOR_WHITE, COLOR_BLACK);
    Print(L"Boot parameters prepared:\n");
    LOG_INFO(L"  Kernel  : 0x%lx - 0x%lx", bi->KernelBase, bi->KernelBase + bi->KernelSize);
    LOG_INFO(L"  RSDP    : 0x%lx", bi->RsdpAddress);
    LOG_INFO(L"  SMBIOS  : 0x%lx", bi->SmbiosAddress);
    LOG_INFO(L"  Framebuf: 0x%lx (%ld MB)", bi->FrameBufferBase, bi->FrameBufferSize / (1024 * 1024));
    
    Print(L"\nPress ENTER to boot kernel, ESC to cancel...");
    while (1) {
        while (uefi_call_wrapper(gST->ConIn->ReadKeyStroke, 2, gST->ConIn, &Key) != EFI_SUCCESS);
        if (Key.UnicodeChar == L'\r' || Key.UnicodeChar == L'\n')
            break;
        else if (Key.UnicodeChar == 0x1B || Key.ScanCode == 0x17) {
            Print(L"\nBoot cancelled by user\n");
            while(1) __asm__ volatile("hlt");
        }
    }
    
    uefi_call_wrapper(gST->ConOut->ClearScreen, 1, gST->ConOut);
    LOG_OK(L"Exiting boot services...");
    ExitBootServicesWrapper(bi, ImageHandle);
    KernelEntry kStart = (KernelEntry)bi->KernelBase;
    kStart(bi);
    while(1) __asm__ volatile("hlt");
}