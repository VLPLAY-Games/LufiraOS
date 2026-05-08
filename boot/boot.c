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

// Киберпанк-псевдонимы
#define COLOR_NEON_PINK     COLOR_LIGHTMAGENTA
#define COLOR_NEON_CYAN     COLOR_LIGHTCYAN
#define COLOR_NEON_GREEN    COLOR_LIGHTGREEN
#define COLOR_DARK_RED      COLOR_RED
#define COLOR_DIM_GRAY      COLOR_DARKGRAY

#define ACPI_10_TABLE_GUID {0xeb9d2d30, 0x2d88, 0x11d3, {0x9a, 0x16, 0x0, 0x90, 0x27, 0x3f, 0xc1, 0x4d}}

// ==================== СТРУКТУРА BOOTINFO ====================
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

// ==================== РЕЖИМЫ ЗАГРУЗКИ ====================
typedef enum {
    MODE_FAST,
    MODE_NORMAL,
    MODE_DEBUG,
    MODE_SAFE
} BootMode;

// ==================== ВСПОМОГАТЕЛЬНЫЕ ФУНКЦИИ ЦВЕТА ====================
VOID SetColor(UINTN Foreground, UINTN Background) {
    uefi_call_wrapper(gST->ConOut->SetAttribute, 2, gST->ConOut, 
                     (Foreground & 0x0F) | ((Background & 0x0F) << 4));
}

VOID PrintColored(CONST CHAR16 *String, UINTN Foreground, UINTN Background) {
    SetColor(Foreground, Background);
    Print(String);
    SetColor(COLOR_LIGHTGRAY, COLOR_BLACK);
}

VOID GetConsoleSize(UINTN *Cols, UINTN *Rows) {
    UINTN Mode = gST->ConOut->Mode->Mode;
    EFI_STATUS status = uefi_call_wrapper(gST->ConOut->QueryMode, 4, gST->ConOut, Mode, Cols, Rows);
    if (EFI_ERROR(status)) {
        // Если не удалось, используем стандартные 80x25
        *Cols = 80;
        *Rows = 25;
    }
}

// ==================== ФУНКЦИЯ ПЕЧАТИ ПО ЦЕНТРУ ====================
VOID PrintCentered(CONST CHAR16 *Str, UINTN Row, UINTN Color) {
    UINTN cols, rows;
    GetConsoleSize(&cols, &rows);
    UINTN len = StrLen(Str);
    UINTN x = (cols > len) ? (cols - len) / 2 : 0;
    uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, x, Row);
    SetColor(Color, COLOR_BLACK);
    Print(Str);
}

// ==================== ОТРИСОВКА UI ====================
VOID DrawBox(UINTN X, UINTN Y, UINTN Width, UINTN Height, CONST CHAR16 *Title) {
    SetColor(COLOR_DARK_RED, COLOR_BLACK);
    
    uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, X, Y);
    Print(L"+");
    for (UINTN i = 0; i < Width - 2; i++) Print(L"-");
    Print(L"+");
    
    if (Title != NULL) {
        uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, X + 2, Y);
        SetColor(COLOR_NEON_CYAN, COLOR_BLACK);
        Print(L" %s ", Title);
        SetColor(COLOR_DARK_RED, COLOR_BLACK);
    }
    
    for (UINTN i = 1; i < Height - 1; i++) {
        uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, X, Y + i);
        Print(L"|");
        uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, X + Width - 1, Y + i);
        Print(L"|");
    }
    
    uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, X, Y + Height - 1);
    Print(L"+");
    for (UINTN i = 0; i < Width - 2; i++) Print(L"-");
    Print(L"+");
    
    SetColor(COLOR_LIGHTGRAY, COLOR_BLACK);
}

VOID PrintInfo(CONST CHAR16 *Label, CONST CHAR16 *Value, BOOLEAN Important, UINTN X, UINTN Y) {
    uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, X, Y);
    
    SetColor(COLOR_NEON_CYAN, COLOR_BLACK);
    Print(L"  ");
    Print(Label);
    
    UINTN Len = StrLen(Label);
    for (UINTN i = Len; i < 25; i++) Print(L" ");
    Print(L": ");
    
    if (Important) {
        SetColor(COLOR_NEON_GREEN, COLOR_BLACK);
    } else {
        SetColor(COLOR_WHITE, COLOR_BLACK);
    }
    Print(Value);
    SetColor(COLOR_LIGHTGRAY, COLOR_BLACK);
}

// ==================== ФУНКЦИЯ ЗАСТАВКИ (БОЛЬШОЙ ЛОГОТИП) ====================
VOID ShowSplash(BootMode mode) {
    uefi_call_wrapper(gST->ConOut->ClearScreen, 1, gST->ConOut);
    
    UINTN cols, rows;
    GetConsoleSize(&cols, &rows);
    
    // Рамка из символов псевдографики
    SetColor(COLOR_DARK_RED, COLOR_BLACK);

    // Логотип (6 строк)
    CHAR16 *logo[] = {
        L"██╗     ██╗   ██╗███████╗██╗██████╗  █████╗     ███████╗███████╗",
        L"██║     ██║   ██║██╔════╝██║██╔══██╗██╔══██╗    ██═══██╝██╔════╝",
        L"██║     ██║   ██║█████╗  ██║██████╔╝███████║    ██═══██╗███████╗",
        L"██║     ██║   ██║██╔══╝  ██║██╔══██╗██╔══██║    ██═══██║╚════██║",
        L"███████╗╚██████╔╝██║     ██║██║  ██║██║  ██║    ███████║███████║",
        L"╚══════╝ ╚═════╝ ╚═╝     ╚═╝╚═╝  ╚═╝╚═╝  ╚═╝    ╚══════╝╚══════╝"
    };
    
    UINTN startRow = (rows - 6) / 2;  // центрируем по вертикали
    
    for (int i = 0; i < 6; i++) {
        PrintCentered(logo[i], startRow + i, COLOR_NEON_CYAN);
    }
    
    // Режим под логотипом
    CHAR16 modeText[32];
    switch (mode) {
        case MODE_FAST:  StrCpy(modeText, L"[ FAST MODE ]"); break;
        case MODE_NORMAL: StrCpy(modeText, L"[ NORMAL MODE ]"); break;
        case MODE_DEBUG:  StrCpy(modeText, L"[ DEBUG MODE ]"); break;
        default: modeText[0] = 0;
    }
    if (modeText[0]) {
        PrintCentered(modeText, startRow + 7, COLOR_NEON_PINK);
    }
    
    // Строка загрузки
    PrintCentered(L"LufiraOS is loading...", startRow + 9, COLOR_DIM_GRAY);
    // --- Анимация загрузки (спиннер) ---
    UINTN spinnerRow = startRow + 10;          // строка под «loading...»
    UINTN spinnerCol = cols / 2;               // центр
    CHAR16 spin[] = L"|/-\\";
    for (int i = 0; i < 20; i++) {             // 20 * 100 мс = 2 секунды
        uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, spinnerCol, spinnerRow);
        SetColor(COLOR_NEON_CYAN, COLOR_BLACK);
        Print(L"%c", spin[i % 4]);
        uefi_call_wrapper(gBS->Stall, 1, 100000);
    }
    // Стираем спиннер
    uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, spinnerCol, spinnerRow);
    Print(L" ");
}


// ==================== ФУНКЦИЯ ДЕТАЛЬНОГО ДАМПА (ТОЛЬКО DEBUG) ====================
VOID DebugDeepDump(BootInfo *bi, EFI_MEMORY_DESCRIPTOR *MemoryMap, UINTN MemoryMapSize, UINTN DescriptorSize) {
    // Очищаем экран для дампа
    SetColor(COLOR_BLACK, COLOR_BLACK);
    uefi_call_wrapper(gST->ConOut->ClearScreen, 1, gST->ConOut);
    SetColor(COLOR_NEON_PINK, COLOR_BLACK);
    Print(L"+==========================================================================+\n");
    Print(L"|                     LUFIRAOS DEEP DEBUG DUMP                           |\n");
    Print(L"+==========================================================================+\n\n");
    SetColor(COLOR_LIGHTGRAY, COLOR_BLACK);

    // --- Секция 1: Информация о ядре ---
    DrawBox(2, 4, 76, 6, L"Kernel Image");
    CHAR16 str[64];
    SPrint(str, sizeof(str), L"0x%lx", bi->KernelBase);
    PrintInfo(L"Kernel Base", str, FALSE, 4, 6);
    SPrint(str, sizeof(str), L"0x%lx", bi->KernelBase + bi->KernelSize);
    PrintInfo(L"Kernel End", str, FALSE, 4, 7);
    SPrint(str, sizeof(str), L"%ld KB", bi->KernelSize / 1024);
    PrintInfo(L"Kernel Size", str, TRUE, 4, 8);
    
    // Небольшая пауза
    uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, 4, 10);
    SetColor(COLOR_DIM_GRAY, COLOR_BLACK);
    Print(L"Press any key for next page...");
    EFI_INPUT_KEY Key;
    while (uefi_call_wrapper(gST->ConIn->ReadKeyStroke, 2, gST->ConIn, &Key) != EFI_SUCCESS);

    // --- Секция 2: Полная карта памяти ---
    uefi_call_wrapper(gST->ConOut->ClearScreen, 1, gST->ConOut);
    SetColor(COLOR_NEON_PINK, COLOR_BLACK);
    Print(L"+==========================================================================+\n");
    Print(L"|                      COMPLETE MEMORY MAP DUMP                          |\n");
    Print(L"+==========================================================================+\n\n");

    UINTN descCount = MemoryMapSize / DescriptorSize;
    SPrint(str, sizeof(str), L"%d descriptors", descCount);
    PrintInfo(L"Total Descriptors", str, FALSE, 2, 4);
    SPrint(str, sizeof(str), L"%d bytes each", DescriptorSize);
    PrintInfo(L"Descriptor Size", str, FALSE, 2, 5);
    Print(L"\n");

    SetColor(COLOR_NEON_CYAN, COLOR_BLACK);
    Print(L"  Type                                   Physical Start   Pages          Attributes\n");
    SetColor(COLOR_DARK_RED, COLOR_BLACK);
    Print(L"  -------------------------------------- ---------------- -------------- ----------------\n");

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
        Print(L"  %-38s %016lx %14ld %016lx\n",
              typeStr, d->PhysicalStart, d->NumberOfPages, d->Attribute);
    }

    uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, 4, 6 + descCount + 2);
    SetColor(COLOR_DIM_GRAY, COLOR_BLACK);
    Print(L"Press any key for next page...");
    while (uefi_call_wrapper(gST->ConIn->ReadKeyStroke, 2, gST->ConIn, &Key) != EFI_SUCCESS);

    // --- Секция 3: Все конфигурационные таблицы ---
    uefi_call_wrapper(gST->ConOut->ClearScreen, 1, gST->ConOut);
    SetColor(COLOR_NEON_PINK, COLOR_BLACK);
    Print(L"+==========================================================================+\n");
    Print(L"|                  CONFIGURATION TABLES COMPLETE LIST                    |\n");
    Print(L"+==========================================================================+\n\n");

    SPrint(str, sizeof(str), L"%d tables", gST->NumberOfTableEntries);
    PrintInfo(L"Number of Tables", str, FALSE, 2, 4);
    Print(L"\n");

    SetColor(COLOR_NEON_CYAN, COLOR_BLACK);
    Print(L"  #  GUID                                  Address\n");
    SetColor(COLOR_DARK_RED, COLOR_BLACK);
    Print(L"  -- ------------------------------------- ----------------\n");

    for (UINTN i = 0; i < gST->NumberOfTableEntries; i++) {
        CHAR16 guidStr[40];
        GuidToString(guidStr, &gST->ConfigurationTable[i].VendorGuid);
        SetColor(COLOR_WHITE, COLOR_BLACK);
        Print(L"  %2d %-37s %016lx\n", i, guidStr, gST->ConfigurationTable[i].VendorTable);
    }

    // --- Секция 4: Графический вывод ---
    Print(L"\n");
    SetColor(COLOR_NEON_PINK, COLOR_BLACK);
    Print(L"+==========================================================================+\n");
    Print(L"|                      GRAPHICS OUTPUT PROTOCOL                          |\n");
    Print(L"+==========================================================================+\n\n");
    SPrint(str, sizeof(str), L"0x%lx", bi->FrameBufferBase);
    PrintInfo(L"FrameBufferBase", str, FALSE, 2, gST->ConOut->Mode->CursorRow + 1);
    SPrint(str, sizeof(str), L"%ld MB", bi->FrameBufferSize / 1024 / 1024);
    PrintInfo(L"FrameBufferSize", str, TRUE, 2, gST->ConOut->Mode->CursorRow + 1);
    SPrint(str, sizeof(str), L"%d x %d", bi->HorizontalResolution, bi->VerticalResolution);
    PrintInfo(L"Resolution", str, FALSE, 2, gST->ConOut->Mode->CursorRow + 1);
    SPrint(str, sizeof(str), L"%d", bi->PixelsPerScanLine);
    PrintInfo(L"PixelsPerScanLine", str, FALSE, 2, gST->ConOut->Mode->CursorRow + 1);
    PrintInfo(L"PixelFormat", bi->PixelFormat == 1 ? L"BGR" : L"RGB", TRUE, 2, gST->ConOut->Mode->CursorRow + 1);

    // --- Секция 5: Прочие адреса ---
    Print(L"\n");
    SetColor(COLOR_NEON_PINK, COLOR_BLACK);
    Print(L"+==========================================================================+\n");
    Print(L"|                        SYSTEM TABLE POINTERS                           |\n");
    Print(L"+==========================================================================+\n\n");
    SPrint(str, sizeof(str), L"0x%lx", bi->RsdpAddress);
    PrintInfo(L"RSDP", bi->RsdpAddress ? str : L"Not Found", bi->RsdpAddress != 0, 2, gST->ConOut->Mode->CursorRow + 1);
    SPrint(str, sizeof(str), L"0x%lx", bi->SmbiosAddress);
    PrintInfo(L"SMBIOS", bi->SmbiosAddress ? str : L"Not Found", bi->SmbiosAddress != 0, 2, gST->ConOut->Mode->CursorRow + 1);
    if (bi->FATImageBase) {
        SPrint(str, sizeof(str), L"0x%lx (%ld MB)", bi->FATImageBase, bi->FATImageSize / 1024 / 1024);
        PrintInfo(L"FAT Image", str, TRUE, 2, gST->ConOut->Mode->CursorRow + 1);
    } else {
        PrintInfo(L"FAT Image", L"Not loaded", FALSE, 2, gST->ConOut->Mode->CursorRow + 1);
    }

    // Ожидание перед продолжением
    uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, 2, gST->ConOut->Mode->CursorRow + 2);
    SetColor(COLOR_NEON_PINK, COLOR_BLACK);
    Print(L"Press ENTER to continue boot process...");
    while (1) {
        while (uefi_call_wrapper(gST->ConIn->ReadKeyStroke, 2, gST->ConIn, &Key) != EFI_SUCCESS);
        if (Key.UnicodeChar == L'\r' || Key.UnicodeChar == L'\n')
            break;
    }
}

// ==================== ЗАГРУЗКА FAT ОБРАЗА С ПРОГРЕССОМ ====================
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
        uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, 4, 34);
        PrintColored(L"Loading FAT image... ", COLOR_NEON_CYAN, COLOR_BLACK);
    }
    
    UINTN Pages = (UINTN)((CopySize + 4095) / 4096);
    EFI_PHYSICAL_ADDRESS FATBase = 0;
    EFI_STATUS status = uefi_call_wrapper(gBS->AllocatePages, 4, AllocateAnyPages, EfiLoaderData, Pages, &FATBase);
    if (!EFI_ERROR(status)) {
        UINT8* Buffer = (UINT8*)FATBase;
        UINTN MaxBlocksPerTransfer = 1024;
        UINTN TotalBlocks = (UINTN)((CopySize + BlockSize - 1) / BlockSize);
        UINTN dotCount = 0;
        
        for (UINTN i = 0; i < TotalBlocks; i += MaxBlocksPerTransfer) {
            UINTN BlocksNow = (TotalBlocks - i) > MaxBlocksPerTransfer ? MaxBlocksPerTransfer : (TotalBlocks - i);
            status = uefi_call_wrapper(BlockIo->ReadBlocks, 5, BlockIo, BlockIo->Media->MediaId, i,
                                       BlocksNow * BlockSize, Buffer + ((UINT64)i * BlockSize));
            if (EFI_ERROR(status)) break;
            
            if (showProgress) {
                // Прогресс в виде точек
                uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, 24 + dotCount, 34);
                SetColor(COLOR_NEON_PINK, COLOR_BLACK);
                Print(L".");
                dotCount++;
                if (dotCount > 40) dotCount = 0; // циклически, если много блоков
            }
            uefi_call_wrapper(gBS->Stall, 1, 5000); // небольшая пауза, чтобы не блокировать полностью
        }
        
        if (!EFI_ERROR(status)) {
            bi->FATImageBase = FATBase;
            bi->FATImageSize = CopySize;
            if (showProgress) {
                // Удаляем точки, пишем ОК
                uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, 24, 34);
                SetColor(COLOR_BLACK, COLOR_BLACK);
                for (int k = 0; k < 40; k++) Print(L" ");
                uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, 24, 34);
                PrintColored(L"OK", COLOR_NEON_GREEN, COLOR_BLACK);
            }
        } else {
            bi->FATImageBase = bi->FATImageSize = 0;
            if (showProgress) {
                uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, 24, 34);
                PrintColored(L"FAILED", COLOR_RED, COLOR_BLACK);
            }
        }
    } else {
        bi->FATImageBase = bi->FATImageSize = 0;
        if (showProgress) {
            uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, 24, 34);
            PrintColored(L"ALLOC FAILED", COLOR_RED, COLOR_BLACK);
        }
    }
}

// ==================== РЕЖИМ ДОПОЛНИТЕЛЬНОЙ ИНФОРМАЦИИ (I) ====================
VOID ShowAdvancedInfo(EFI_LOADED_IMAGE *LoadedImage, EFI_FILE_HANDLE KernelFile, 
                      BootInfo *bi, EFI_MEMORY_DESCRIPTOR *MemoryMap, 
                      UINTN MemoryMapSize, UINTN DescriptorSize) {
    SetColor(COLOR_BLACK, COLOR_BLACK);
    uefi_call_wrapper(gST->ConOut->ClearScreen, 1, gST->ConOut);
    SetColor(COLOR_LIGHTGRAY, COLOR_BLACK);
    
    uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, 0, 1);
    SetColor(COLOR_NEON_PINK, COLOR_BLACK);
    Print(L"+==========================================================================+\n");
    Print(L"|                  LufiraOS Advanced System Information                |\n");
    Print(L"+==========================================================================+\n\n");
    SetColor(COLOR_LIGHTGRAY, COLOR_BLACK);
    
    DrawBox(2, 6, 76, 8, L"Kernel Information");
    
    EFI_FILE_INFO *FileInfo;
    UINTN FileInfoSize = sizeof(EFI_FILE_INFO) + 128;
    uefi_call_wrapper(gBS->AllocatePool, 3, EfiLoaderData, FileInfoSize, (VOID**)&FileInfo);
    uefi_call_wrapper(KernelFile->GetInfo, 4, KernelFile, &gEfiFileInfoGuid, &FileInfoSize, FileInfo);
    
    CHAR16 Str[32];
    SPrint(Str, sizeof(Str), L"%ld KB", bi->KernelSize / 1024);
    PrintInfo(L"Kernel Size", Str, FALSE, 4, 8);
    SPrint(Str, sizeof(Str), L"0x%lx", bi->KernelBase);
    PrintInfo(L"Kernel Base", Str, FALSE, 4, 9);
    SPrint(Str, sizeof(Str), L"0x%lx", bi->KernelBase + bi->KernelSize);
    PrintInfo(L"Kernel End", Str, FALSE, 4, 10);
    SPrint(Str, sizeof(Str), L"%ld KB", LoadedImage->ImageSize / 1024);
    PrintInfo(L"Bootloader Size", Str, FALSE, 4, 11);
    uefi_call_wrapper(gBS->FreePool, 1, FileInfo);
    
    DrawBox(2, 15, 76, 10, L"Memory Map");
    UINTN Line = 17;
    UINTN DescriptorCount = MemoryMapSize / DescriptorSize;
    SPrint(Str, sizeof(Str), L"%d descriptors", DescriptorCount);
    PrintInfo(L"Total Descriptors", Str, FALSE, 4, Line++);
    SPrint(Str, sizeof(Str), L"%d bytes", DescriptorSize);
    PrintInfo(L"Descriptor Size", Str, FALSE, 4, Line++);
    
    UINT64 UsableMemory = 0, ReservedMemory = 0;
    for (UINTN i = 0; i < DescriptorCount; i++) {
        EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR*)((UINT8*)MemoryMap + (i * DescriptorSize));
        if (d->Type == EfiConventionalMemory || d->Type == EfiLoaderCode || d->Type == EfiLoaderData)
            UsableMemory += d->NumberOfPages * 4096;
        else
            ReservedMemory += d->NumberOfPages * 4096;
    }
    
    if (UsableMemory > 1024 * 1024 * 1024)
        SPrint(Str, sizeof(Str), L"%ld GB", UsableMemory / (1024 * 1024 * 1024));
    else
        SPrint(Str, sizeof(Str), L"%ld MB", UsableMemory / (1024 * 1024));
    PrintInfo(L"Usable Memory", Str, TRUE, 4, Line++);
    
    if (ReservedMemory > 1024 * 1024 * 1024)
        SPrint(Str, sizeof(Str), L"%ld GB", ReservedMemory / (1024 * 1024 * 1024));
    else
        SPrint(Str, sizeof(Str), L"%ld MB", ReservedMemory / (1024 * 1024));
    PrintInfo(L"Reserved Memory", Str, FALSE, 4, Line++);
    
    DrawBox(2, 26, 76, 8, L"System Tables");
    Line = 28;
    PrintInfo(L"ACPI RSDP", bi->RsdpAddress ? L"Found" : L"Not Found", bi->RsdpAddress != 0, 4, Line++);
    PrintInfo(L"SMBIOS", bi->SmbiosAddress ? L"Found" : L"Not Found", bi->SmbiosAddress != 0, 4, Line++);
    SPrint(Str, sizeof(Str), L"%d tables", gST->NumberOfTableEntries);
    PrintInfo(L"Config Tables", Str, FALSE, 4, Line++);
    
    uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, 4, 35);
    SetColor(COLOR_NEON_PINK, COLOR_BLACK);
    Print(L"\n\nPress ");
    SetColor(COLOR_NEON_CYAN, COLOR_BLACK);
    Print(L"ENTER");
    SetColor(COLOR_NEON_PINK, COLOR_BLACK);
    Print(L" to return to main screen...");
    
    EFI_INPUT_KEY Key;
    while (1) {
        uefi_call_wrapper(gST->ConIn->ReadKeyStroke, 2, gST->ConIn, &Key);
        if (Key.UnicodeChar == L'\r' || Key.UnicodeChar == L'\n' || Key.ScanCode == 0x17)
            break;
    }
}

// ==================== БЫСТРАЯ/БЕЗОПАСНАЯ ЗАГРУЗКА ====================
VOID QuickBoot(BootInfo *bi, EFI_HANDLE ImageHandle, BootMode mode) {
    // Выводим сообщение в зависимости от режима
    uefi_call_wrapper(gST->ConOut->ClearScreen, 1, gST->ConOut);
    SetColor(COLOR_DARK_RED, COLOR_BLACK);
    
    if (mode == MODE_SAFE) {
        uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, 0, 0);
        Print(L"[Safe Mode] Loading LufiraOS...\n");
    } else {
        uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, 0, 0);
        Print(L"Loading LufiraOS...\n");
    }
    uefi_call_wrapper(gBS->Stall, 1, 500000); // 0.5 сек для эффекта
    
    // 1. Получить LoadedImage и FS
    EFI_LOADED_IMAGE *LoadedImage;
    EFI_STATUS status = uefi_call_wrapper(gBS->HandleProtocol, 3, ImageHandle, &LoadedImageProtocol, (VOID**)&LoadedImage);
    if (EFI_ERROR(status)) {
        PrintColored(L"FATAL: No LoadedImage\n", COLOR_RED, COLOR_BLACK);
        while(1) __asm__ volatile("hlt");
    }
    
    EFI_FILE_IO_INTERFACE *FileSystem;
    status = uefi_call_wrapper(gBS->HandleProtocol, 3, LoadedImage->DeviceHandle, &FileSystemProtocol, (VOID**)&FileSystem);
    if (EFI_ERROR(status)) {
        PrintColored(L"FATAL: No FileSystem\n", COLOR_RED, COLOR_BLACK);
        while(1) __asm__ volatile("hlt");
    }
    
    EFI_FILE_HANDLE Root;
    uefi_call_wrapper(FileSystem->OpenVolume, 2, FileSystem, &Root);
    
    EFI_FILE_HANDLE KernelFile;
    status = uefi_call_wrapper(Root->Open, 5, Root, &KernelFile, L"kernel.bin", EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(status)) {
        PrintColored(L"FATAL: kernel.bin not found\n", COLOR_RED, COLOR_BLACK);
        while(1) __asm__ volatile("hlt");
    }
    
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
    if (EFI_ERROR(status)) {
        PrintColored(L"FATAL: Cannot allocate kernel memory\n", COLOR_RED, COLOR_BLACK);
        while(1) __asm__ volatile("hlt");
    }
    bi->KernelBase = KernelBase;
    
    UINTN ChunkSize = 65536, TotalLoaded = 0;
    while (TotalLoaded < bi->KernelSize) {
        UINTN ToRead = (bi->KernelSize - TotalLoaded) > ChunkSize ? ChunkSize : (bi->KernelSize - TotalLoaded);
        uefi_call_wrapper(KernelFile->Read, 3, KernelFile, &ToRead, (VOID*)(KernelBase + TotalLoaded));
        TotalLoaded += ToRead;
    }
    
    // Получить MemoryMap
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
    
    // GOP
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
    
    // RSDP/SMBIOS
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
    
    // Block I/O (FAT image) - загружаем молча, без вывода
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
    LoadFATImage(BlockIo, bi, FALSE);  // без прогресса
    
    // Запуск ядра
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
    
    // Очищаем экран
    SetColor(COLOR_BLACK, COLOR_BLACK);
    uefi_call_wrapper(gST->ConOut->ClearScreen, 1, gST->ConOut);
    SetColor(COLOR_LIGHTGRAY, COLOR_BLACK);
    
    // ==================== МЕНЮ ВЫБОРА РЕЖИМА ====================
    uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, 0, 2);
    SetColor(COLOR_NEON_PINK, COLOR_BLACK);
    Print(L"  +=============================================================+\n");
    Print(L"  |         LufiraOS Boot Mode Selection                        |\n");
    Print(L"  +=============================================================+\n\n");
    SetColor(COLOR_WHITE, COLOR_BLACK);
    Print(L"    [F] Fast Mode   - Skip all, boot immediately\n");
    Print(L"    [N] Normal Mode - Show system info and 5sec countdown\n");
    Print(L"    [D] Debug Mode  - Detailed output, step-by-step\n");
    Print(L"    [S] Safe Mode   - Safe boot (minimal)\n\n");
    SetColor(COLOR_NEON_CYAN, COLOR_BLACK);
    Print(L"    Press F, N, D or S to select...\n\n");
    
    BootMode mode = MODE_NORMAL;
    EFI_INPUT_KEY Key;
    BOOLEAN keyPressed = FALSE;
    
    // Таймер на 5 секунд
    UINTN countdown = 5;
    while (countdown > 0) {
        // Выводим обратный отсчет
        uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, 6, gST->ConOut->Mode->CursorRow);
        SetColor(COLOR_NEON_PINK, COLOR_BLACK);
        Print(L"Automatic boot in %d seconds... ", countdown);
        
        // Проверяем клавишу с интервалом 100 мс
        for (int i = 0; i < 10; i++) {
            EFI_STATUS status = uefi_call_wrapper(gST->ConIn->ReadKeyStroke, 2, gST->ConIn, &Key);
            if (!EFI_ERROR(status)) {
                keyPressed = TRUE;
                break;
            }
            uefi_call_wrapper(gBS->Stall, 1, 100000);
        }
        if (keyPressed) break;
        countdown--;
    }
    
    if (keyPressed) {
        if (Key.UnicodeChar == L'f' || Key.UnicodeChar == L'F') {
            mode = MODE_FAST;
        } else if (Key.UnicodeChar == L'n' || Key.UnicodeChar == L'N') {
            mode = MODE_NORMAL;
        } else if (Key.UnicodeChar == L'd' || Key.UnicodeChar == L'D') {
            mode = MODE_DEBUG;
        } else if (Key.UnicodeChar == L's' || Key.UnicodeChar == L'S') {
            mode = MODE_SAFE;
        } else if (Key.UnicodeChar == L'\r' || Key.UnicodeChar == L'\n') {
            mode = MODE_NORMAL;
        } else {
            // любая другая клавиша — Normal
            mode = MODE_NORMAL;
        }
    } else {
        // Таймер истек — Normal
        mode = MODE_NORMAL;
    }
    
    // Быстрый/безопасный запуск
    if (mode == MODE_FAST || mode == MODE_SAFE) {
        BootInfo bi = {0};
        QuickBoot(&bi, ImageHandle, mode);
        // не возвращается
    }
    
    BOOLEAN debug = (mode == MODE_DEBUG);
    
    // Показываем заставку с большим логотипом
    ShowSplash(mode);
    
    // Очищаем экран для Normal/Debug
    SetColor(COLOR_BLACK, COLOR_BLACK);
    uefi_call_wrapper(gST->ConOut->ClearScreen, 1, gST->ConOut);
    SetColor(COLOR_LIGHTGRAY, COLOR_BLACK);
    
    // ==================== ЗАГОЛОВОК (УЖЕ НЕ НУЖЕН, НО ОСТАВИМ ДЛЯ ИНФОРМАЦИИ) ====================
    uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, 0, 1);
    
    SetColor(COLOR_DARK_RED, COLOR_BLACK);
    Print(L"+==========================================================================+\n");
    SetColor(COLOR_DIM_GRAY, COLOR_BLACK);
    Print(L"|                                                                      |\n");
    
    SetColor(COLOR_NEON_CYAN, COLOR_BLACK);
    Print(L"|    ██╗     ██╗   ██╗███████╗██╗██████╗  █████╗     ███████╗███████╗  |\n");
    Print(L"|    ██║     ██║   ██║██╔════╝██║██╔══██╗██╔══██╗    ██═══██╝██╔════╝  |\n");
    Print(L"|    ██║     ██║   ██║█████╗  ██║██████╔╝███████║    ██═══██╗███████╗  |\n");
    Print(L"|    ██║     ██║   ██║██╔══╝  ██║██╔══██╗██╔══██║    ██═══██║╚════██║  |\n");
    Print(L"|    ███████╗╚██████╔╝██║     ██║██║  ██║██║  ██║    ███████║███████║  |\n");
    Print(L"|    ╚══════╝ ╚═════╝ ╚═╝     ╚═╝╚═╝  ╚═╝╚═╝  ╚═╝    ╚══════╝╚══════╝  |\n");
    
    SetColor(COLOR_DIM_GRAY, COLOR_BLACK);
    Print(L"|                                                                      |\n");
    SetColor(COLOR_WHITE, COLOR_BLACK);
    Print(L"|                         UEFI Bootloader v0.1.2                       |\n");
    Print(L"|                       Copyright © 2024 LufiraOS                       |\n");
    SetColor(COLOR_DARK_RED, COLOR_BLACK);
    Print(L"+==========================================================================+\n\n");
    
    if (debug) {
        SetColor(COLOR_NEON_GREEN, COLOR_BLACK);
        Print(L"  *** DEBUG MODE ***\n\n");
    }
    SetColor(COLOR_LIGHTGRAY, COLOR_BLACK);
    
    // ==================== СИСТЕМНАЯ ИНФОРМАЦИЯ ====================
    DrawBox(2, 14, 76, 12, L"System Information");
    
    PrintInfo(L"Firmware Vendor", gST->FirmwareVendor, FALSE, 4, 16);
    
    CHAR16 UefiVersion[32];
    SPrint(UefiVersion, sizeof(UefiVersion), L"%d.%02d", gST->Hdr.Revision >> 16, gST->Hdr.Revision & 0xFFFF);
    PrintInfo(L"UEFI Version", UefiVersion, FALSE, 4, 17);
    
    EFI_GUID gEfiGlobalVariableGuid = {0x8BE4DF61, 0x93CA, 0x11D2, {0xAA, 0x0D, 0x00, 0xE0, 0x98, 0x03, 0x2B, 0x8C}};
    UINT8 SecureBoot;
    UINTN DataSize = sizeof(SecureBoot);
    EFI_STATUS sb_status = uefi_call_wrapper(gRT->GetVariable, 5, L"SecureBoot", &gEfiGlobalVariableGuid, NULL, &DataSize, &SecureBoot);
    if (!EFI_ERROR(sb_status))
        PrintInfo(L"Secure Boot", SecureBoot ? L"Enabled" : L"Disabled", SecureBoot, 4, 18);
    else
        PrintInfo(L"Secure Boot", L"Not Supported", FALSE, 4, 18);
    
    EFI_TIME Time;
    if (!EFI_ERROR(uefi_call_wrapper(gRT->GetTime, 2, &Time, NULL))) {
        CHAR16 TimeStr[64];
        SPrint(TimeStr, sizeof(TimeStr), L"%02d/%02d/%04d %02d:%02d:%02d",
               Time.Day, Time.Month, Time.Year, Time.Hour, Time.Minute, Time.Second);
        PrintInfo(L"System Time", TimeStr, FALSE, 4, 19);
    }
    
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
    
    CHAR16 MemoryStr[32];
    if (TotalRAM > 1024 * 1024 * 1024)
        SPrint(MemoryStr, sizeof(MemoryStr), L"%ld GB", TotalRAM / (1024 * 1024 * 1024));
    else
        SPrint(MemoryStr, sizeof(MemoryStr), L"%ld MB", TotalRAM / (1024 * 1024));
    PrintInfo(L"Available Memory", MemoryStr, TRUE, 4, 20);
    
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;
    EFI_GUID gopGuid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    BootInfo bi = {0};
    EFI_STATUS status = uefi_call_wrapper(gBS->LocateProtocol, 3, &gopGuid, NULL, (VOID**)&gop);
    
    if (!EFI_ERROR(status)) {
        CHAR16 ResolutionStr[32];
        SPrint(ResolutionStr, sizeof(ResolutionStr), L"%dx%d", gop->Mode->Info->HorizontalResolution, gop->Mode->Info->VerticalResolution);
        PrintInfo(L"Video Resolution", ResolutionStr, FALSE, 4, 21);
        
        bi.FrameBufferBase = gop->Mode->FrameBufferBase;
        bi.FrameBufferSize = gop->Mode->FrameBufferSize;
        bi.HorizontalResolution = gop->Mode->Info->HorizontalResolution;
        bi.VerticalResolution = gop->Mode->Info->VerticalResolution;
        bi.PixelsPerScanLine = gop->Mode->Info->PixelsPerScanLine;
        bi.PixelFormat = (gop->Mode->Info->PixelFormat == PixelBlueGreenRedReserved8BitPerColor) ? 1 : 0;
        PrintInfo(L"Pixel Format", bi.PixelFormat == 1 ? L"BGR" : L"RGB", FALSE, 4, 22);
    } else {
        PrintInfo(L"Video Mode", L"Not Available", FALSE, 4, 21);
    }
    
    // ==================== ПОИСК СИСТЕМНЫХ ТАБЛИЦ ====================
    bi.RsdpAddress = bi.SmbiosAddress = 0;
    for (UINTN i = 0; i < gST->NumberOfTableEntries; i++) {
        EFI_GUID Acpi2Guid = ACPI_20_TABLE_GUID;
        EFI_GUID Acpi1Guid = ACPI_10_TABLE_GUID;
        EFI_GUID SmbiosGuid = SMBIOS_TABLE_GUID;
        EFI_GUID Smbios3Guid = SMBIOS3_TABLE_GUID;
        
        if (CompareGuid(&gST->ConfigurationTable[i].VendorGuid, &Acpi2Guid) == 0 ||
            CompareGuid(&gST->ConfigurationTable[i].VendorGuid, &Acpi1Guid) == 0)
            bi.RsdpAddress = (uint64_t)gST->ConfigurationTable[i].VendorTable;
        if (CompareGuid(&gST->ConfigurationTable[i].VendorGuid, &SmbiosGuid) == 0 ||
            CompareGuid(&gST->ConfigurationTable[i].VendorGuid, &Smbios3Guid) == 0)
            bi.SmbiosAddress = (uint64_t)gST->ConfigurationTable[i].VendorTable;
    }
    
    // ==================== ЗАГРУЗКА ЯДРА ====================
    DrawBox(2, 27, 76, 6, L"Kernel Loading");
    
    uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, 4, 29);
    PrintColored(L"[1/4] ", COLOR_NEON_GREEN, COLOR_BLACK);
    Print(L"Locating boot device... ");
    
    EFI_LOADED_IMAGE *LoadedImage;
    uefi_call_wrapper(gBS->HandleProtocol, 3, ImageHandle, &LoadedImageProtocol, (VOID**)&LoadedImage);
    
    EFI_FILE_IO_INTERFACE *FileSystem;
    uefi_call_wrapper(gBS->HandleProtocol, 3, LoadedImage->DeviceHandle, &FileSystemProtocol, (VOID**)&FileSystem);
    
    EFI_FILE_HANDLE Root;
    uefi_call_wrapper(FileSystem->OpenVolume, 2, FileSystem, &Root);
    
    PrintColored(L"OK\n", COLOR_NEON_GREEN, COLOR_BLACK);
    
    uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, 4, 30);
    PrintColored(L"[2/4] ", COLOR_NEON_GREEN, COLOR_BLACK);
    Print(L"Opening kernel file... ");
    
    EFI_FILE_HANDLE KernelFile;
    status = uefi_call_wrapper(Root->Open, 5, Root, &KernelFile, L"kernel.bin", EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(status)) {
        PrintColored(L"FAILED\n", COLOR_RED, COLOR_BLACK);
        uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, 4, 31);
        PrintColored(L"Error: kernel.bin not found on boot device!\n", COLOR_RED, COLOR_BLACK);
        PrintColored(L"Please ensure kernel.bin is in the root directory.\n", COLOR_NEON_PINK, COLOR_BLACK);
        while(1) __asm__ volatile("hlt");
    }
    PrintColored(L"OK\n", COLOR_NEON_GREEN, COLOR_BLACK);
    
    EFI_FILE_INFO *KernelFileInfo;
    UINTN KernelFileInfoSize = sizeof(EFI_FILE_INFO) + 256;
    uefi_call_wrapper(gBS->AllocatePool, 3, EfiLoaderData, KernelFileInfoSize, (VOID**)&KernelFileInfo);
    uefi_call_wrapper(KernelFile->GetInfo, 4, KernelFile, &gEfiFileInfoGuid, &KernelFileInfoSize, KernelFileInfo);
    bi.KernelSize = KernelFileInfo->FileSize;
    uefi_call_wrapper(gBS->FreePool, 1, KernelFileInfo);
    
    uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, 4, 31);
    PrintColored(L"[3/4] ", COLOR_NEON_GREEN, COLOR_BLACK);
    Print(L"Allocating kernel memory... ");
    
    UINTN Pages = (bi.KernelSize + 4095) / 4096;
    EFI_PHYSICAL_ADDRESS KernelBase = 0x100000;
    status = uefi_call_wrapper(gBS->AllocatePages, 4, AllocateAddress, EfiLoaderData, Pages, &KernelBase);
    if (EFI_ERROR(status))
        uefi_call_wrapper(gBS->AllocatePages, 4, AllocateAnyPages, EfiLoaderData, Pages, &KernelBase);
    if (EFI_ERROR(status)) {
        PrintColored(L"FAILED\n", COLOR_RED, COLOR_BLACK);
        uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, 4, 32);
        PrintColored(L"Error: Cannot allocate memory for kernel!\n", COLOR_RED, COLOR_BLACK);
        while(1) __asm__ volatile("hlt");
    }
    bi.KernelBase = KernelBase;
    if (debug) {
        uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, 30, 31);
        SetColor(COLOR_NEON_CYAN, COLOR_BLACK);
        Print(L"(base=0x%lx, pages=%d)", KernelBase, Pages);
    } else {
        PrintColored(L"OK\n", COLOR_NEON_GREEN, COLOR_BLACK);
    }
    
    uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, 4, 32);
    PrintColored(L"[4/4] ", COLOR_NEON_GREEN, COLOR_BLACK);
    Print(L"Loading kernel... ");
    
    UINTN ChunkSize = 65536;
    UINTN TotalLoaded = 0;
    UINTN dotCount = 0;
    
    for (UINTN i = 0; TotalLoaded < bi.KernelSize; i++) {
        UINTN ToRead = (bi.KernelSize - TotalLoaded) > ChunkSize ? ChunkSize : (bi.KernelSize - TotalLoaded);
        uefi_call_wrapper(KernelFile->Read, 3, KernelFile, &ToRead, (VOID*)(KernelBase + TotalLoaded));
        TotalLoaded += ToRead;
        
        if (i % 4 == 0) {
            uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, 24 + dotCount, 32);
            SetColor(COLOR_NEON_PINK, COLOR_BLACK);
            Print(L".");
            dotCount++;
            SetColor(COLOR_LIGHTGRAY, COLOR_BLACK);
        }
        if (debug) {
            uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, 60, 32);
            SetColor(COLOR_DIM_GRAY, COLOR_BLACK);
            Print(L"%d%%", (TotalLoaded * 100) / bi.KernelSize);
        }
        uefi_call_wrapper(gBS->Stall, 1, 5000);
    }
    
    uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, 24, 32);
    SetColor(COLOR_BLACK, COLOR_BLACK);
    for (UINTN i = 0; i < 40; i++) Print(L" ");
    uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, 24, 32);
    SetColor(COLOR_NEON_GREEN, COLOR_BLACK);
    Print(L"  COMPLETE");
    
    bi.TotalMemory = TotalRAM;
    bi.MemoryMapSize = MemoryMapSize;
    bi.MemoryMap = MemoryMap;
    bi.MemoryMapDescriptorSize = DescriptorSize;
    
    // ==================== ЗАГРУЗКА ОБРАЗА ESP (С ПРОГРЕССОМ) ====================
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
    LoadFATImage(BlockIo, &bi, TRUE);  // показываем прогресс
    
    // ==================== В DEBUG РЕЖИМЕ - ДЕТАЛЬНЫЙ ДАМП ====================
    if (debug) {
        DebugDeepDump(&bi, MemoryMap, MemoryMapSize, DescriptorSize);
        // После дампа очищаем экран для меню загрузки
        uefi_call_wrapper(gST->ConOut->ClearScreen, 1, gST->ConOut);
        SetColor(COLOR_LIGHTGRAY, COLOR_BLACK);
    }
    
    // ==================== ПАНЕЛЬ ЗАПУСКА ====================
    if (debug) {
        // В debug-режиме ждём ENTER или ESC без таймера
        DrawBox(2, 4, 76, 5, L"Boot Control");
        uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, 4, 6);
        SetColor(COLOR_NEON_PINK, COLOR_BLACK);
        Print(L"Press ");
        SetColor(COLOR_NEON_CYAN, COLOR_BLACK);
        Print(L"ENTER");
        SetColor(COLOR_NEON_PINK, COLOR_BLACK);
        Print(L" to boot kernel, ");
        SetColor(COLOR_NEON_CYAN, COLOR_BLACK);
        Print(L"ESC");
        SetColor(COLOR_NEON_PINK, COLOR_BLACK);
        Print(L" to cancel.");
        
        while (1) {
            while (uefi_call_wrapper(gST->ConIn->ReadKeyStroke, 2, gST->ConIn, &Key) != EFI_SUCCESS);
            if (Key.UnicodeChar == L'\r' || Key.UnicodeChar == L'\n')
                break;
            else if (Key.UnicodeChar == 0x1B || Key.ScanCode == 0x17) {
                PrintColored(L"\nBoot cancelled by user", COLOR_RED, COLOR_BLACK);
                while(1) __asm__ volatile("hlt");
            }
        }
    } else {
        // Normal: таймер 5 сек
        DrawBox(2, 35, 76, 5, L"Boot Menu");
        uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, 4, 37);
        SetColor(COLOR_NEON_PINK, COLOR_BLACK);
        Print(L"Press ");
        SetColor(COLOR_NEON_CYAN, COLOR_BLACK);
        Print(L"ENTER");
        SetColor(COLOR_NEON_PINK, COLOR_BLACK);
        Print(L" to boot now, ");
        SetColor(COLOR_NEON_CYAN, COLOR_BLACK);
        Print(L"I");
        SetColor(COLOR_NEON_PINK, COLOR_BLACK);
        Print(L" for info, ");
        SetColor(COLOR_NEON_CYAN, COLOR_BLACK);
        Print(L"ESC");
        SetColor(COLOR_NEON_PINK, COLOR_BLACK);
        Print(L" to cancel");
        
        uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, 4, 39);
        SetColor(COLOR_NEON_PINK, COLOR_BLACK);
        Print(L"Starting LufiraOS in 5 seconds...");
        
        BOOLEAN SkipCountdown = FALSE, ShowInfo = FALSE;
        for (int i = 5; i > 0 && !SkipCountdown; i--) {
            uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, 35, 39);
            SetColor(COLOR_NEON_CYAN, COLOR_BLACK);
            Print(L"%d sec...", i);
            for (int j = 0; j < 10; j++) {
                status = uefi_call_wrapper(gST->ConIn->ReadKeyStroke, 2, gST->ConIn, &Key);
                if (!EFI_ERROR(status)) {
                    if (Key.UnicodeChar == L'\r' || Key.UnicodeChar == L'\n') {
                        SkipCountdown = TRUE;
                        uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, 4, 39);
                        SetColor(COLOR_BLACK, COLOR_BLACK);
                        for (UINTN k = 0; k < 76; k++) Print(L" ");
                        uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, 4, 39);
                        PrintColored(L"Starting kernel immediately...", COLOR_NEON_GREEN, COLOR_BLACK);
                        break;
                    } else if (Key.UnicodeChar == L'i' || Key.UnicodeChar == L'I') {
                        ShowInfo = TRUE;
                        break;
                    } else if (Key.UnicodeChar == 0x1B || Key.ScanCode == 0x17) {
                        uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, 4, 39);
                        SetColor(COLOR_BLACK, COLOR_BLACK);
                        for (UINTN k = 0; k < 76; k++) Print(L" ");
                        uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, 4, 39);
                        PrintColored(L"\nBoot cancelled by user", COLOR_RED, COLOR_BLACK);
                        while(1) __asm__ volatile("hlt");
                    }
                }
                uefi_call_wrapper(gBS->Stall, 1, 100000);
            }
            if (SkipCountdown || ShowInfo) break;
        }
        
        uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, 4, 39);
        SetColor(COLOR_BLACK, COLOR_BLACK);
        for (UINTN k = 0; k < 76; k++) Print(L" ");
        
        if (ShowInfo) {
            ShowAdvancedInfo(LoadedImage, KernelFile, &bi, MemoryMap, MemoryMapSize, DescriptorSize);
            uefi_call_wrapper(gST->ConOut->ClearScreen, 1, gST->ConOut);
            uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, 0, 0);
            SetColor(COLOR_NEON_PINK, COLOR_BLACK);
            Print(L"Resuming boot process...\n");
            uefi_call_wrapper(gBS->Stall, 1, 2000000);
        }
    }
    
    // ==================== ФИНАЛЬНЫЙ ЗАПУСК ЯДРА ====================
    uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, 4, 41);
    SetColor(COLOR_NEON_GREEN, COLOR_BLACK);
    Print(L"\n==========================================================================\n");
    Print(L"        Switching to 64-bit mode and starting LufiraOS kernel...\n");
    Print(L"==========================================================================\n");
    uefi_call_wrapper(gBS->Stall, 1, 2000000);
    
    SetColor(COLOR_BLACK, COLOR_BLACK);
    uefi_call_wrapper(gST->ConOut->ClearScreen, 1, gST->ConOut);
    
    KernelEntry kStart = (KernelEntry)bi.KernelBase;
    kStart(&bi);
    while(1) __asm__ volatile("hlt");
}