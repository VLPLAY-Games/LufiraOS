#include <efi.h>
#include <efilib.h>

// ==================== ЦВЕТОВАЯ ПАЛИТРА (КИБЕРПАНК) ====================
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

#define ACPI_10_TABLE_GUID {0xeb9d2d30, 0x2d88, 0x11d3, {0x9a, 0x16, 0x0, 0x90, 0x27, 0x3f, 0xc1, 0x4d}}

typedef struct {
    uint64_t FrameBufferBase;
    uint64_t FrameBufferSize;
    uint32_t HorizontalResolution;
    uint32_t VerticalResolution;
    uint32_t PixelsPerScanLine;
    uint32_t PixelFormat;
    uint64_t TotalMemory;
    uint64_t MemoryMapSize;
    void* MemoryMap;
    uint32_t MemoryMapDescriptorSize;
    uint64_t KernelBase;
    uint64_t KernelSize;
    uint64_t RsdpAddress;
    uint64_t SmbiosAddress;
    uint64_t FATImageBase;
    uint64_t FATImageSize;
} BootInfo;

typedef void (*KernelEntry)(BootInfo*);

typedef enum { MODE_NORMAL, MODE_DEBUG, MODE_SAFE } BootMode;

// ==================== ВСПОМОГАТЕЛЬНЫЕ ФУНКЦИИ ====================
VOID SetColor(UINTN Foreground, UINTN Background) {
    uefi_call_wrapper(gST->ConOut->SetAttribute, 2, gST->ConOut, 
                     (Foreground & 0x0F) | ((Background & 0x0F) << 4));
}

VOID PrintColored(CONST CHAR16 *String, UINTN Foreground, UINTN Background) {
    SetColor(Foreground, Background);
    Print(String);
    SetColor(COLOR_WHITE, COLOR_BLACK);  // основной текст теперь ярче
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
    // надпись "LufiraOS is loading..." убрана
    
    CHAR16 spin[] = L"|/-\\";
    UINTN spinnerCol = cols / 2;
    UINTN spinnerRow = startRow + 9;   // спиннер под логотипом
    
    for (int i = 0; i < 20; i++) {
        uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, spinnerCol, spinnerRow);
        SetColor(COLOR_NEON_CYAN, COLOR_BLACK);
        Print(L"%c", spin[i % 4]);
        uefi_call_wrapper(gBS->Stall, 1, 100000);
    }
    uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, spinnerCol, spinnerRow);
    Print(L" ");
}

// ==================== ЗАГРУЗКА FAT ОБРАЗА ====================
VOID LoadFATImage(EFI_BLOCK_IO_PROTOCOL *BlockIo, BootInfo *bi, BOOLEAN showProgress) {
    if (!BlockIo || !BlockIo->Media) {
        bi->FATImageBase = 0;
        bi->FATImageSize = 0;
        return;
    }
    UINTN BlockSize = BlockIo->Media->BlockSize;
    UINT64 MaxImageSize = 256ULL * 1024 * 1024;
    UINT64 CopySize = (BlockIo->Media->LastBlock + 1) * BlockSize;
    if (CopySize > MaxImageSize) CopySize = MaxImageSize;
    
    if (showProgress) {
        PrintColored(L"Loading FAT image... ", COLOR_NEON_CYAN, COLOR_BLACK);
    }
    
    UINTN Pages = (UINTN)((CopySize + 4095) / 4096);
    EFI_PHYSICAL_ADDRESS FATBase = 0;
    EFI_STATUS status = uefi_call_wrapper(gBS->AllocatePages, 4, AllocateAnyPages, EfiLoaderData, Pages, &FATBase);
    if (!EFI_ERROR(status)) {
        UINT8* Buffer = (UINT8*)FATBase;
        UINTN MaxBlocksPerTransfer = 1024;
        UINTN TotalBlocks = (UINTN)((CopySize + BlockSize - 1) / BlockSize);
        for (UINTN i = 0; i < TotalBlocks; i += MaxBlocksPerTransfer) {
            UINTN BlocksNow = (TotalBlocks - i) > MaxBlocksPerTransfer ? MaxBlocksPerTransfer : (TotalBlocks - i);
            status = uefi_call_wrapper(BlockIo->ReadBlocks, 5, BlockIo, BlockIo->Media->MediaId, i,
                                       BlocksNow * BlockSize, Buffer + ((UINT64)i * BlockSize));
            if (EFI_ERROR(status)) break;
            if (showProgress) {
                Print(L".");
            }
            uefi_call_wrapper(gBS->Stall, 1, 5000);
        }
        if (!EFI_ERROR(status)) {
            bi->FATImageBase = FATBase;
            bi->FATImageSize = CopySize;
            if (showProgress) PrintColored(L"OK\n", COLOR_NEON_GREEN, COLOR_BLACK);
        } else {
            bi->FATImageBase = bi->FATImageSize = 0;
            if (showProgress) PrintColored(L"FAILED\n", COLOR_RED, COLOR_BLACK);
        }
    } else {
        if (showProgress) PrintColored(L"ALLOC FAILED\n", COLOR_RED, COLOR_BLACK);
    }
}

// ==================== БЫСТРАЯ ЗАГРУЗКА (NORMAL / SAFE) ====================
VOID QuickBoot(BootInfo *bi, EFI_HANDLE ImageHandle, BootMode mode, BOOLEAN keepLogo, BOOLEAN animateSpinner) {
    UINTN cols, rows;
    GetConsoleSize(&cols, &rows);
    UINTN statusRow, spinnerRow;
    
    if (keepLogo) {
        // подгоняем строки под положение спиннера после логотипа
        UINTN logoStart = (rows - 6) / 2;
        spinnerRow = logoStart + 9;
        statusRow = logoStart + 11;
    } else {
        uefi_call_wrapper(gST->ConOut->ClearScreen, 1, gST->ConOut);
        if (mode == MODE_SAFE) {
            // Safe Mode: просто две строки слева сверху
            uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, 0, 0);
            SetColor(COLOR_DARK_RED, COLOR_BLACK);
            Print(L"[Safe mode]");
            uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, 0, 1);
            Print(L"LufiraOS Loading");
            statusRow = 3;
            spinnerRow = 4;
        } else {
            // на случай, если Safe не активирован (не должно случиться)
            uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, 0, 0);
            SetColor(COLOR_DARK_RED, COLOR_BLACK);
            Print(L"LufiraOS Loading");
            statusRow = 2;
            spinnerRow = 3;
        }
    }
    
    CHAR16 spin[] = L"|/-\\";
    UINTN spinIdx = 0;
    
    #define UPDATE_STATUS(msg) do { \
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
    
    // Макрос для фатальной ошибки – выводится в левом верхнем углу, спиннер продолжает вращаться
    #define FATAL_ERROR(msg) do { \
        uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, 0, 0); \
        SetColor(COLOR_RED, COLOR_BLACK); \
        Print(L"[FATAL] %s", msg); \
        uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, 0, 1); \
        Print(L"System halted. Press any key."); \
        while(1) { \
            spinIdx = (spinIdx + 1) % 4; \
            uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, cols / 2, spinnerRow); \
            SetColor(COLOR_NEON_CYAN, COLOR_BLACK); \
            Print(L"%c", spin[spinIdx]); \
            uefi_call_wrapper(gBS->Stall, 1, 200000); \
            EFI_INPUT_KEY _key; \
            if (uefi_call_wrapper(gST->ConIn->ReadKeyStroke, 2, gST->ConIn, &_key) == EFI_SUCCESS) break; \
        } \
        while(1) __asm__ volatile("hlt"); \
    } while(0)
    
    UPDATE_STATUS(L"Initializing...");
    uefi_call_wrapper(gBS->Stall, 1, 500000);
    
    EFI_LOADED_IMAGE *LoadedImage;
    EFI_STATUS status = uefi_call_wrapper(gBS->HandleProtocol, 3, ImageHandle, &LoadedImageProtocol, (VOID**)&LoadedImage);
    if (EFI_ERROR(status)) {
        FATAL_ERROR(L"No LoadedImage");
    }
    
    EFI_FILE_IO_INTERFACE *FileSystem;
    status = uefi_call_wrapper(gBS->HandleProtocol, 3, LoadedImage->DeviceHandle, &FileSystemProtocol, (VOID**)&FileSystem);
    if (EFI_ERROR(status)) {
        FATAL_ERROR(L"No FileSystem");
    }
    
    EFI_FILE_HANDLE Root;
    uefi_call_wrapper(FileSystem->OpenVolume, 2, FileSystem, &Root);
    
    UPDATE_STATUS(L"Locating kernel.bin...");
    EFI_FILE_HANDLE KernelFile;
    status = uefi_call_wrapper(Root->Open, 5, Root, &KernelFile, L"kernel.bin", EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(status)) {
        FATAL_ERROR(L"kernel.bin not found");
    }
    
    EFI_FILE_INFO *KernelFileInfo;
    UINTN KernelFileInfoSize = sizeof(EFI_FILE_INFO) + 256;
    uefi_call_wrapper(gBS->AllocatePool, 3, EfiLoaderData, KernelFileInfoSize, (VOID**)&KernelFileInfo);
    uefi_call_wrapper(KernelFile->GetInfo, 4, KernelFile, &gEfiFileInfoGuid, &KernelFileInfoSize, KernelFileInfo);
    bi->KernelSize = KernelFileInfo->FileSize;
    uefi_call_wrapper(gBS->FreePool, 1, KernelFileInfo);
    
    UPDATE_STATUS(L"Allocating memory for kernel...");
    UINTN Pages = (bi->KernelSize + 4095) / 4096;
    EFI_PHYSICAL_ADDRESS KernelBase = 0x100000;
    status = uefi_call_wrapper(gBS->AllocatePages, 4, AllocateAddress, EfiLoaderData, Pages, &KernelBase);
    if (EFI_ERROR(status))
        uefi_call_wrapper(gBS->AllocatePages, 4, AllocateAnyPages, EfiLoaderData, Pages, &KernelBase);
    if (EFI_ERROR(status)) {
        FATAL_ERROR(L"Cannot allocate kernel memory");
    }
    bi->KernelBase = KernelBase;
    
    UPDATE_STATUS(L"Loading kernel...");
    UINTN ChunkSize = 65536, TotalLoaded = 0;
    while (TotalLoaded < bi->KernelSize) {
        UINTN ToRead = (bi->KernelSize - TotalLoaded) > ChunkSize ? ChunkSize : (bi->KernelSize - TotalLoaded);
        uefi_call_wrapper(KernelFile->Read, 3, KernelFile, &ToRead, (VOID*)(KernelBase + TotalLoaded));
        TotalLoaded += ToRead;
        if (animateSpinner && ((TotalLoaded / ChunkSize) % 2 == 0)) {
            spinIdx = (spinIdx + 1) % 4;
            UPDATE_STATUS(L"Loading kernel...");
        }
        uefi_call_wrapper(gBS->Stall, 1, 1000);
    }
    
    UPDATE_STATUS(L"Reading memory map...");
    UINTN MemoryMapSize = 0, MapKey, DescriptorSize;
    UINT32 DescriptorVersion;
    uefi_call_wrapper(gBS->GetMemoryMap, 5, &MemoryMapSize, NULL, &MapKey, &DescriptorSize, &DescriptorVersion);
    MemoryMapSize += 2 * DescriptorSize;
    EFI_MEMORY_DESCRIPTOR *MemoryMap;
    uefi_call_wrapper(gBS->AllocatePool, 3, EfiLoaderData, MemoryMapSize, (VOID**)&MemoryMap);
    uefi_call_wrapper(gBS->GetMemoryMap, 5, &MemoryMapSize, MemoryMap, &MapKey, &DescriptorSize, &DescriptorVersion);
    
    uint64_t TotalRAM = 0;
    for (UINTN i = 0; i < (MemoryMapSize / DescriptorSize); i++) {
        EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR*)((UINT8*)MemoryMap + (i * DescriptorSize));
        if (d->Type == EfiConventionalMemory || d->Type == EfiLoaderCode || d->Type == EfiLoaderData)
            TotalRAM += d->NumberOfPages * 4096;
    }
    bi->TotalMemory = TotalRAM;
    bi->MemoryMapSize = MemoryMapSize;
    bi->MemoryMap = MemoryMap;
    bi->MemoryMapDescriptorSize = DescriptorSize;
    
    UPDATE_STATUS(L"Initializing graphics...");
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;
    EFI_GUID gopGuid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    status = uefi_call_wrapper(gBS->LocateProtocol, 3, &gopGuid, NULL, (VOID**)&gop);
    if (!EFI_ERROR(status)) {
        bi->FrameBufferBase = gop->Mode->FrameBufferBase;
        bi->FrameBufferSize = gop->Mode->FrameBufferSize;
        bi->HorizontalResolution = gop->Mode->Info->HorizontalResolution;
        bi->VerticalResolution = gop->Mode->Info->VerticalResolution;
        bi->PixelsPerScanLine = gop->Mode->Info->PixelsPerScanLine;
        bi->PixelFormat = (gop->Mode->Info->PixelFormat == PixelBlueGreenRedReserved8BitPerColor) ? 1 : 0;
    }
    
    UPDATE_STATUS(L"Scanning system tables...");
    bi->RsdpAddress = bi->SmbiosAddress = 0;
    for (UINTN i = 0; i < gST->NumberOfTableEntries; i++) {
        EFI_GUID Acpi2Guid = ACPI_20_TABLE_GUID;
        EFI_GUID Acpi1Guid = ACPI_10_TABLE_GUID;
        EFI_GUID SmbiosGuid = SMBIOS_TABLE_GUID;
        EFI_GUID Smbios3Guid = SMBIOS3_TABLE_GUID;
        if (CompareGuid(&gST->ConfigurationTable[i].VendorGuid, &Acpi2Guid) == 0 ||
            CompareGuid(&gST->ConfigurationTable[i].VendorGuid, &Acpi1Guid) == 0)
            bi->RsdpAddress = (uint64_t)gST->ConfigurationTable[i].VendorTable;
        if (CompareGuid(&gST->ConfigurationTable[i].VendorGuid, &SmbiosGuid) == 0 ||
            CompareGuid(&gST->ConfigurationTable[i].VendorGuid, &Smbios3Guid) == 0)
            bi->SmbiosAddress = (uint64_t)gST->ConfigurationTable[i].VendorTable;
    }
    
    UPDATE_STATUS(L"Loading FAT image...");
    EFI_BLOCK_IO_PROTOCOL *BlockIo = NULL;
    EFI_GUID BlockIoGuid = EFI_BLOCK_IO_PROTOCOL_GUID;
    status = uefi_call_wrapper(gBS->HandleProtocol, 3, LoadedImage->DeviceHandle, &BlockIoGuid, (VOID**)&BlockIo);
    if (EFI_ERROR(status)) {
        EFI_DEVICE_PATH_PROTOCOL *DevicePath;
        EFI_GUID DevicePathGuid = EFI_DEVICE_PATH_PROTOCOL_GUID;
        status = uefi_call_wrapper(gBS->HandleProtocol, 3, LoadedImage->DeviceHandle, &DevicePathGuid, (VOID**)&DevicePath);
        if (!EFI_ERROR(status)) {
            EFI_HANDLE blockHandle;
            status = uefi_call_wrapper(gBS->LocateDevicePath, 3, &BlockIoGuid, &DevicePath, &blockHandle);
            if (!EFI_ERROR(status))
                uefi_call_wrapper(gBS->HandleProtocol, 3, blockHandle, &BlockIoGuid, (VOID**)&BlockIo);
        }
    }
    LoadFATImage(BlockIo, bi, FALSE);
    
    UPDATE_STATUS(L"Starting kernel...");
    uefi_call_wrapper(gBS->Stall, 1, 1000000);
    
    // Очистка статуса перед передачей управления
    uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, 0, statusRow);
    SetColor(COLOR_BLACK, COLOR_BLACK);
    for (UINTN i = 0; i < cols; i++) Print(L" ");
    if (animateSpinner) {
        uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, cols / 2, spinnerRow);
        Print(L" ");
    }
    uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, 0, rows - 1);
    
    KernelEntry kStart = (KernelEntry)bi->KernelBase;
    kStart(bi);
    while(1) __asm__ volatile("hlt");
}

// ==================== DEBUG В СТИЛЕ LINUX (С ДЕТАЛЬНЫМИ ДАМПАМИ) ====================
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
    
    // === Сбор информации ===
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
    
    // Память
    UINTN MemoryMapSize = 0, MapKey, DescriptorSize;
    UINT32 DescriptorVersion;
    uefi_call_wrapper(gBS->GetMemoryMap, 5, &MemoryMapSize, NULL, &MapKey, &DescriptorSize, &DescriptorVersion);
    MemoryMapSize += 2 * DescriptorSize;
    EFI_MEMORY_DESCRIPTOR *MemoryMap;
    uefi_call_wrapper(gBS->AllocatePool, 3, EfiLoaderData, MemoryMapSize, (VOID**)&MemoryMap);
    uefi_call_wrapper(gBS->GetMemoryMap, 5, &MemoryMapSize, MemoryMap, &MapKey, &DescriptorSize, &DescriptorVersion);
    
    uint64_t TotalRAM = 0;
    for (UINTN i = 0; i < (MemoryMapSize / DescriptorSize); i++) {
        EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR*)((UINT8*)MemoryMap + (i * DescriptorSize));
        if (d->Type == EfiConventionalMemory || d->Type == EfiLoaderCode || d->Type == EfiLoaderData)
            TotalRAM += d->NumberOfPages * 4096;
    }
    LOG_INFO(L"  Available Memory: %ld MB", TotalRAM / (1024 * 1024));
    
    // GOP
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;
    EFI_GUID gopGuid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_STATUS status = uefi_call_wrapper(gBS->LocateProtocol, 3, &gopGuid, NULL, (VOID**)&gop);
    if (!EFI_ERROR(status)) {
        bi->FrameBufferBase = gop->Mode->FrameBufferBase;
        bi->FrameBufferSize = gop->Mode->FrameBufferSize;
        bi->HorizontalResolution = gop->Mode->Info->HorizontalResolution;
        bi->VerticalResolution = gop->Mode->Info->VerticalResolution;
        bi->PixelsPerScanLine = gop->Mode->Info->PixelsPerScanLine;
        bi->PixelFormat = (gop->Mode->Info->PixelFormat == PixelBlueGreenRedReserved8BitPerColor) ? 1 : 0;
        LOG_INFO(L"  Video: %dx%d (%s)", bi->HorizontalResolution, bi->VerticalResolution, bi->PixelFormat == 1 ? L"BGR" : L"RGB");
    } else {
        LOG_WARN(L"  Video: Not Available");
    }
    
    // Таблицы
    bi->RsdpAddress = bi->SmbiosAddress = 0;
    for (UINTN i = 0; i < gST->NumberOfTableEntries; i++) {
        EFI_GUID Acpi2Guid = ACPI_20_TABLE_GUID;
        EFI_GUID Acpi1Guid = ACPI_10_TABLE_GUID;
        EFI_GUID SmbiosGuid = SMBIOS_TABLE_GUID;
        EFI_GUID Smbios3Guid = SMBIOS3_TABLE_GUID;
        if (CompareGuid(&gST->ConfigurationTable[i].VendorGuid, &Acpi2Guid) == 0 ||
            CompareGuid(&gST->ConfigurationTable[i].VendorGuid, &Acpi1Guid) == 0)
            bi->RsdpAddress = (uint64_t)gST->ConfigurationTable[i].VendorTable;
        if (CompareGuid(&gST->ConfigurationTable[i].VendorGuid, &SmbiosGuid) == 0 ||
            CompareGuid(&gST->ConfigurationTable[i].VendorGuid, &Smbios3Guid) == 0)
            bi->SmbiosAddress = (uint64_t)gST->ConfigurationTable[i].VendorTable;
    }
    LOG_INFO(L"  RSDP: %s", bi->RsdpAddress ? L"Found" : L"Not Found");
    LOG_INFO(L"  SMBIOS: %s", bi->SmbiosAddress ? L"Found" : L"Not Found");
    
    Print(L"\n");
    
    // Загрузка ядра
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
    status = uefi_call_wrapper(gBS->AllocatePages, 4, AllocateAddress, EfiLoaderData, Pages, &KernelBase);
    if (EFI_ERROR(status))
        uefi_call_wrapper(gBS->AllocatePages, 4, AllocateAnyPages, EfiLoaderData, Pages, &KernelBase);
    if (EFI_ERROR(status)) { LOG_FAIL(L"Memory allocation failed"); while(1) __asm__ volatile("hlt"); }
    bi->KernelBase = KernelBase;
    LOG_OK(L"Kernel memory allocated at 0x%lx", KernelBase);
    
    UINTN ChunkSize = 65536, TotalLoaded = 0;
    while (TotalLoaded < bi->KernelSize) {
        UINTN ToRead = (bi->KernelSize - TotalLoaded) > ChunkSize ? ChunkSize : (bi->KernelSize - TotalLoaded);
        uefi_call_wrapper(KernelFile->Read, 3, KernelFile, &ToRead, (VOID*)(KernelBase + TotalLoaded));
        TotalLoaded += ToRead;
    }
    LOG_OK(L"Kernel loaded (%ld KB)", bi->KernelSize / 1024);
    
    bi->TotalMemory = TotalRAM;
    bi->MemoryMapSize = MemoryMapSize;
    bi->MemoryMap = MemoryMap;
    bi->MemoryMapDescriptorSize = DescriptorSize;
    
    // FAT
    LOG_INFO(L"Loading FAT image...");
    EFI_BLOCK_IO_PROTOCOL *BlockIo = NULL;
    EFI_GUID BlockIoGuid = EFI_BLOCK_IO_PROTOCOL_GUID;
    status = uefi_call_wrapper(gBS->HandleProtocol, 3, LoadedImage->DeviceHandle, &BlockIoGuid, (VOID**)&BlockIo);
    if (EFI_ERROR(status)) {
        EFI_DEVICE_PATH_PROTOCOL *DevicePath;
        EFI_GUID DevicePathGuid = EFI_DEVICE_PATH_PROTOCOL_GUID;
        status = uefi_call_wrapper(gBS->HandleProtocol, 3, LoadedImage->DeviceHandle, &DevicePathGuid, (VOID**)&DevicePath);
        if (!EFI_ERROR(status)) {
            EFI_HANDLE blockHandle;
            status = uefi_call_wrapper(gBS->LocateDevicePath, 3, &BlockIoGuid, &DevicePath, &blockHandle);
            if (!EFI_ERROR(status))
                uefi_call_wrapper(gBS->HandleProtocol, 3, blockHandle, &BlockIoGuid, (VOID**)&BlockIo);
        }
    }
    if (BlockIo && BlockIo->Media) {
        LoadFATImage(BlockIo, bi, FALSE);
        if (bi->FATImageBase)
            LOG_OK(L"FAT image loaded (%ld MB)", bi->FATImageSize / (1024 * 1024));
        else
            LOG_WARN(L"FAT image not loaded");
    } else {
        LOG_WARN(L"No Block I/O Protocol");
    }
    
    // === ДЕТАЛЬНЫЕ ДАМПЫ ===
    EFI_INPUT_KEY Key;
    
    // --- Карта памяти ---
    Print(L"\nPress any key for Memory Map dump...");
    while (uefi_call_wrapper(gST->ConIn->ReadKeyStroke, 2, gST->ConIn, &Key) != EFI_SUCCESS);
    uefi_call_wrapper(gST->ConOut->ClearScreen, 1, gST->ConOut);
    SetColor(COLOR_WHITE, COLOR_BLACK);
    Print(L"Memory Map (descriptor size: %d bytes)\n\n", DescriptorSize);
    SetColor(COLOR_NEON_CYAN, COLOR_BLACK);
    Print(L"Type                                     Physical Start   Pages          Attributes\n");
    SetColor(COLOR_DARK_RED, COLOR_BLACK);
    Print(L"--------------------------------------- ----------------- -------------- -----------------\n");
    UINTN descCount = MemoryMapSize / DescriptorSize;
    for (UINTN i = 0; i < descCount; i++) {
        EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR*)((UINT8*)MemoryMap + (i * DescriptorSize));
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
    
    // --- Конфигурационные таблицы ---
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
    
    // --- GOP подробно ---
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
    
    // --- Итоговая сводка ---
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
    KernelEntry kStart = (KernelEntry)bi->KernelBase;
    kStart(bi);
    while(1) __asm__ volatile("hlt");
}

// ==================== ГЛАВНАЯ ФУНКЦИЯ ====================
EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    InitializeLib(ImageHandle, SystemTable);
    gST = SystemTable;
    gBS = SystemTable->BootServices;
    gRT = SystemTable->RuntimeServices;
    
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
    
    if (mode == MODE_NORMAL) {
        BootInfo bi = {0};
        ShowSplash(MODE_NORMAL);
        QuickBoot(&bi, ImageHandle, MODE_NORMAL, TRUE, TRUE);
    } else if (mode == MODE_SAFE) {
        BootInfo bi = {0};
        QuickBoot(&bi, ImageHandle, MODE_SAFE, FALSE, TRUE);
    } else {
        ShowSplash(MODE_DEBUG);
        BootInfo bi = {0};
        DebugBoot(&bi, ImageHandle);
    }
    
    while(1) __asm__ volatile("hlt");
}