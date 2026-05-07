#include <efi.h>
#include <efilib.h>

// Цвета для текста (UEFI поддерживает 16 цветов)
#define COLOR_BLACK         0x00
#define COLOR_BLUE          0x01
#define COLOR_GREEN         0x02
#define COLOR_CYAN          0x03
#define COLOR_RED           0x04
#define COLOR_MAGENTA       0x05
#define COLOR_YELLOW        0x06
#define COLOR_WHITE         0x07
#define COLOR_LIGHTGRAY     0x08
#define COLOR_DARKGRAY      0x08  // В UEFI это то же самое
#define COLOR_LIGHTBLUE     0x09
#define COLOR_LIGHTGREEN    0x0A
#define COLOR_LIGHTCYAN     0x0B
#define COLOR_LIGHTRED      0x0C
#define COLOR_LIGHTMAGENTA  0x0D
#define COLOR_LIGHTYELLOW   0x0E
#define COLOR_BRIGHTWHITE   0x0F

#define ACPI_10_TABLE_GUID {0xeb9d2d30, 0x2d88, 0x11d3, {0x9a, 0x16, 0x0, 0x90, 0x27, 0x3f, 0xc1, 0x4d}}

// Структура BootInfo для передачи в ядро
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
    uint64_t RsdpAddress;     // ACPI RSDP
    uint64_t SmbiosAddress;   // SMBIOS
    uint64_t FATImageBase;    // NEW: base of FAT image
    uint64_t FATImageSize;    // NEW: size of FAT image
} BootInfo;

typedef void (*KernelEntry)(BootInfo*);

// ==================== ФУНКЦИИ ДЛЯ РАБОТЫ С ЦВЕТАМИ ====================
VOID SetColor(UINTN Foreground, UINTN Background) {
    uefi_call_wrapper(gST->ConOut->SetAttribute, 2, gST->ConOut, 
                     (Foreground & 0x0F) | ((Background & 0x0F) << 4));
}

VOID PrintColored(CONST CHAR16 *String, UINTN Foreground, UINTN Background) {
    SetColor(Foreground, Background);
    Print(String);
    SetColor(COLOR_LIGHTGRAY, COLOR_BLACK);
}

// ==================== ФУНКЦИИ ДЛЯ ОТРИСОВКИ UI ====================
VOID DrawHorizontalLine(UINTN Length, UINTN X, UINTN Y) {
    uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, X, Y);
    for (UINTN i = 0; i < Length; i++) {
        Print(L"-");
    }
}

VOID DrawBox(UINTN X, UINTN Y, UINTN Width, UINTN Height, CONST CHAR16 *Title) {
    SetColor(COLOR_BLUE, COLOR_BLACK);
    
    // Верхняя граница
    uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, X, Y);
    Print(L"+");
    for (UINTN i = 0; i < Width - 2; i++) Print(L"-");
    Print(L"+");
    
    // Заголовок
    if (Title != NULL) {
        uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, X + 2, Y);
        SetColor(COLOR_YELLOW, COLOR_BLACK);
        Print(L" %s ", Title);
        SetColor(COLOR_BLUE, COLOR_BLACK);
    }
    
    // Боковые границы
    for (UINTN i = 1; i < Height - 1; i++) {
        uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, X, Y + i);
        Print(L"|");
        uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, X + Width - 1, Y + i);
        Print(L"|");
    }
    
    // Нижняя граница
    uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, X, Y + Height - 1);
    Print(L"+");
    for (UINTN i = 0; i < Width - 2; i++) Print(L"-");
    Print(L"+");
    
    SetColor(COLOR_LIGHTGRAY, COLOR_BLACK);
}


// ==================== ФУНКЦИИ ДЛЯ ВЫВОДА ИНФОРМАЦИИ ====================
VOID PrintInfo(CONST CHAR16 *Label, CONST CHAR16 *Value, BOOLEAN Important, UINTN X, UINTN Y) {
    uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, X, Y);
    
    SetColor(COLOR_CYAN, COLOR_BLACK);
    Print(L"  ");
    Print(Label);
    
    UINTN Len = StrLen(Label);
    for (UINTN i = Len; i < 25; i++) {
        Print(L" ");
    }
    
    Print(L": ");
    
    if (Important) {
        SetColor(COLOR_YELLOW, COLOR_BLACK);
    } else {
        SetColor(COLOR_WHITE, COLOR_BLACK);
    }
    
    Print(Value);
    SetColor(COLOR_LIGHTGRAY, COLOR_BLACK);
}

// ==================== РЕЖИМ ДОПОЛНИТЕЛЬНОЙ ИНФОРМАЦИИ ====================
VOID ShowAdvancedInfo(EFI_LOADED_IMAGE *LoadedImage, EFI_FILE_HANDLE KernelFile, 
                      BootInfo *bi, EFI_MEMORY_DESCRIPTOR *MemoryMap, 
                      UINTN MemoryMapSize, UINTN DescriptorSize) {
    // Очищаем экран
    SetColor(COLOR_BLACK, COLOR_BLACK);
    uefi_call_wrapper(gST->ConOut->ClearScreen, 1, gST->ConOut);
    SetColor(COLOR_LIGHTGRAY, COLOR_BLACK);
    
    // Заголовок
    uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, 0, 1);
    SetColor(COLOR_YELLOW, COLOR_BLACK);
    Print(L"+==========================================================================+\n");
    Print(L"|                  LufiraOS Advanced System Information                |\n");
    Print(L"+==========================================================================+\n\n");
    
    SetColor(COLOR_LIGHTGRAY, COLOR_BLACK);
    
    // Информация о ядре
    DrawBox(2, 6, 76, 8, L"Kernel Information");
    
    EFI_FILE_INFO *FileInfo;
    UINTN FileInfoSize = sizeof(EFI_FILE_INFO) + 128;
    uefi_call_wrapper(gBS->AllocatePool, 3, EfiLoaderData, FileInfoSize, (VOID**)&FileInfo);
    uefi_call_wrapper(KernelFile->GetInfo, 4, KernelFile, &gEfiFileInfoGuid, &FileInfoSize, FileInfo);
    
    CHAR16 KernelSizeStr[32];
    SPrint(KernelSizeStr, sizeof(KernelSizeStr), L"%ld KB", bi->KernelSize / 1024);
    PrintInfo(L"Kernel Size", KernelSizeStr, FALSE, 4, 8);
    
    CHAR16 KernelBaseStr[32];
    SPrint(KernelBaseStr, sizeof(KernelBaseStr), L"0x%lx", bi->KernelBase);
    PrintInfo(L"Kernel Base", KernelBaseStr, FALSE, 4, 9);
    
    CHAR16 KernelEndStr[32];
    SPrint(KernelEndStr, sizeof(KernelEndStr), L"0x%lx", bi->KernelBase + bi->KernelSize);
    PrintInfo(L"Kernel End", KernelEndStr, FALSE, 4, 10);
    
    CHAR16 LoaderSizeStr[32];
    SPrint(LoaderSizeStr, sizeof(LoaderSizeStr), L"%ld KB", LoadedImage->ImageSize / 1024);
    PrintInfo(L"Bootloader Size", LoaderSizeStr, FALSE, 4, 11);
    
    uefi_call_wrapper(gBS->FreePool, 1, FileInfo);
    
    // Карта памяти
    DrawBox(2, 15, 76, 10, L"Memory Map");
    
    UINTN Line = 17;
    UINTN DescriptorCount = MemoryMapSize / DescriptorSize;
    CHAR16 DescStr[64];
    
    SPrint(DescStr, sizeof(DescStr), L"%d descriptors", DescriptorCount);
    PrintInfo(L"Total Descriptors", DescStr, FALSE, 4, Line++);
    
    SPrint(DescStr, sizeof(DescStr), L"%d bytes", DescriptorSize);
    PrintInfo(L"Descriptor Size", DescStr, FALSE, 4, Line++);
    
    // Типы памяти
    UINT64 UsableMemory = 0, ReservedMemory = 0;
    for (UINTN i = 0; i < DescriptorCount; i++) {
        EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR*)((UINT8*)MemoryMap + (i * DescriptorSize));
        if (d->Type == EfiConventionalMemory || d->Type == EfiLoaderCode || d->Type == EfiLoaderData) {
            UsableMemory += d->NumberOfPages * 4096;
        } else {
            ReservedMemory += d->NumberOfPages * 4096;
        }
    }
    
    CHAR16 UsableStr[32];
    if (UsableMemory > 1024 * 1024 * 1024) {
        SPrint(UsableStr, sizeof(UsableStr), L"%ld GB", UsableMemory / (1024 * 1024 * 1024));
    } else {
        SPrint(UsableStr, sizeof(UsableStr), L"%ld MB", UsableMemory / (1024 * 1024));
    }
    PrintInfo(L"Usable Memory", UsableStr, TRUE, 4, Line++);
    
    CHAR16 ReservedStr[32];
    if (ReservedMemory > 1024 * 1024 * 1024) {
        SPrint(ReservedStr, sizeof(ReservedStr), L"%ld GB", ReservedMemory / (1024 * 1024 * 1024));
    } else {
        SPrint(ReservedStr, sizeof(ReservedStr), L"%ld MB", ReservedMemory / (1024 * 1024));
    }
    PrintInfo(L"Reserved Memory", ReservedStr, FALSE, 4, Line++);
    
    // Системные таблицы
    DrawBox(2, 26, 76, 8, L"System Tables");
    
    Line = 28;
    PrintInfo(L"ACPI RSDP", bi->RsdpAddress ? L"Found" : L"Not Found", bi->RsdpAddress != 0, 4, Line++);
    PrintInfo(L"SMBIOS", bi->SmbiosAddress ? L"Found" : L"Not Found", bi->SmbiosAddress != 0, 4, Line++);
    
    CHAR16 TableCountStr[32];
    SPrint(TableCountStr, sizeof(TableCountStr), L"%d tables", gST->NumberOfTableEntries);
    PrintInfo(L"Config Tables", TableCountStr, FALSE, 4, Line++);
    
    // Инструкция для возврата
    uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, 4, 35);
    SetColor(COLOR_YELLOW, COLOR_BLACK);
    Print(L"\n\nPress ");
    SetColor(COLOR_CYAN, COLOR_BLACK);
    Print(L"ENTER");
    SetColor(COLOR_YELLOW, COLOR_BLACK);
    Print(L" to return to main screen...");
    
    // Ждем нажатия ENTER
    EFI_INPUT_KEY Key;
    while (1) {
        uefi_call_wrapper(gST->ConIn->ReadKeyStroke, 2, gST->ConIn, &Key);
        if (Key.UnicodeChar == L'\r' || Key.UnicodeChar == L'\n' || Key.ScanCode == 0x17) {
            break;
        }
    }
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
    
    // ==================== ЗАГОЛОВОК С ASCII-ART ====================
    uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, 0, 1);
    
    SetColor(COLOR_BLUE, COLOR_BLACK);
    Print(L"+==========================================================================+\n");
    
    SetColor(COLOR_CYAN, COLOR_BLACK);
    Print(L"|                                                                      |\n");
    
    SetColor(COLOR_YELLOW, COLOR_BLACK);
    Print(L"|    ██╗     ██╗   ██╗███████╗██╗██████╗  █████╗     ███████╗███████╗  |\n");
    Print(L"|    ██║     ██║   ██║██╔════╝██║██╔══██╗██╔══██╗    ██═══██╝██╔════╝  |\n");
    Print(L"|    ██║     ██║   ██║█████╗  ██║██████╔╝███████║    ██═══██╗███████╗  |\n");
    Print(L"|    ██║     ██║   ██║██╔══╝  ██║██╔══██╗██╔══██║    ██═══██║╚════██║  |\n");
    Print(L"|    ███████╗╚██████╔╝██║     ██║██║  ██║██║  ██║    ███████║███████║  |\n");
    Print(L"|    ╚══════╝ ╚═════╝ ╚═╝     ╚═╝╚═╝  ╚═╝╚═╝  ╚═╝    ╚══════╝╚══════╝  |\n");
    
    SetColor(COLOR_CYAN, COLOR_BLACK);
    Print(L"|                                                                      |\n");
    
    SetColor(COLOR_LIGHTGRAY, COLOR_BLACK);
    Print(L"|                         UEFI Bootloader v0.1.2                       |\n");
    Print(L"|                       Copyright © 2024 LufiraOS                       |\n");
    
    SetColor(COLOR_BLUE, COLOR_BLACK);
    Print(L"+==========================================================================+\n\n");
    
    SetColor(COLOR_LIGHTGRAY, COLOR_BLACK);
    
    // ==================== СИСТЕМНАЯ ИНФОРМАЦИЯ ====================
    DrawBox(2, 12, 76, 12, L"System Information");
    
    // 1. Информация о прошивке
    PrintInfo(L"Firmware Vendor", gST->FirmwareVendor, FALSE, 4, 14);
    
    // Версия UEFI
    CHAR16 UefiVersion[32];
    SPrint(UefiVersion, sizeof(UefiVersion), L"%d.%02d", 
           gST->Hdr.Revision >> 16, gST->Hdr.Revision & 0xFFFF);
    PrintInfo(L"UEFI Version", UefiVersion, FALSE, 4, 15);
    
    // Secure Boot
    EFI_GUID gEfiGlobalVariableGuid = {0x8BE4DF61, 0x93CA, 0x11D2, {0xAA, 0x0D, 0x00, 0xE0, 0x98, 0x03, 0x2B, 0x8C}};
    UINT8 SecureBoot;
    UINTN DataSize = sizeof(SecureBoot);
    EFI_STATUS sb_status = uefi_call_wrapper(gRT->GetVariable, 5, 
        L"SecureBoot", 
        &gEfiGlobalVariableGuid,
        NULL, 
        &DataSize, 
        &SecureBoot);
    
    if (!EFI_ERROR(sb_status)) {
        PrintInfo(L"Secure Boot", SecureBoot ? L"Enabled" : L"Disabled", SecureBoot, 4, 16);
    } else {
        PrintInfo(L"Secure Boot", L"Not Supported", FALSE, 4, 16);
    }
    
    // Системное время
    EFI_TIME Time;
    if (!EFI_ERROR(uefi_call_wrapper(gRT->GetTime, 2, &Time, NULL))) {
        CHAR16 TimeStr[64];
        SPrint(TimeStr, sizeof(TimeStr), L"%02d/%02d/%04d %02d:%02d:%02d",
               Time.Day, Time.Month, Time.Year, Time.Hour, Time.Minute, Time.Second);
        PrintInfo(L"System Time", TimeStr, FALSE, 4, 17);
    }
    
    // 2. Получаем карту памяти
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
    if (TotalRAM > 1024 * 1024 * 1024) {
        SPrint(MemoryStr, sizeof(MemoryStr), L"%ld GB", TotalRAM / (1024 * 1024 * 1024));
    } else {
        SPrint(MemoryStr, sizeof(MemoryStr), L"%ld MB", TotalRAM / (1024 * 1024));
    }
    PrintInfo(L"Available Memory", MemoryStr, TRUE, 4, 18);
    
    // 3. Графическая информация
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;
    EFI_GUID gopGuid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    BootInfo bi = {0};
    EFI_STATUS status = uefi_call_wrapper(gBS->LocateProtocol, 3, &gopGuid, NULL, (VOID**)&gop);
    
    if (!EFI_ERROR(status)) {
        CHAR16 ResolutionStr[32];
        SPrint(ResolutionStr, sizeof(ResolutionStr), L"%dx%d",
               gop->Mode->Info->HorizontalResolution,
               gop->Mode->Info->VerticalResolution);
        
        PrintInfo(L"Video Resolution", ResolutionStr, FALSE, 4, 19);
        
        bi.FrameBufferBase = gop->Mode->FrameBufferBase;
        bi.FrameBufferSize = gop->Mode->FrameBufferSize;
        bi.HorizontalResolution = gop->Mode->Info->HorizontalResolution;
        bi.VerticalResolution = gop->Mode->Info->VerticalResolution;
        bi.PixelsPerScanLine = gop->Mode->Info->PixelsPerScanLine;
        
        if (gop->Mode->Info->PixelFormat == PixelBlueGreenRedReserved8BitPerColor) {
            bi.PixelFormat = 1; // BGR
            PrintInfo(L"Pixel Format", L"BGR (Blue-Green-Red)", FALSE, 4, 20);
        } else if (gop->Mode->Info->PixelFormat == PixelRedGreenBlueReserved8BitPerColor) {
            bi.PixelFormat = 0; // RGB
            PrintInfo(L"Pixel Format", L"RGB (Red-Green-Blue)", FALSE, 4, 20);
        } else {
            bi.PixelFormat = 0;
            PrintInfo(L"Pixel Format", L"Unknown (using RGB)", FALSE, 4, 20);
        }
    } else {
        PrintInfo(L"Video Mode", L"Not Available", FALSE, 4, 19);
    }
    
    // ==================== ПОИСК СИСТЕМНЫХ ТАБЛИЦ ====================
    bi.RsdpAddress = 0;
    bi.SmbiosAddress = 0;
    // Поиск ACPI RSDP
    for (UINTN i = 0; i < gST->NumberOfTableEntries; i++) {
        EFI_GUID Acpi2Guid = ACPI_20_TABLE_GUID;
        EFI_GUID Acpi1Guid = ACPI_10_TABLE_GUID;
        EFI_GUID SmbiosGuid = SMBIOS_TABLE_GUID;
        EFI_GUID Smbios3Guid = SMBIOS3_TABLE_GUID;
        
        if (CompareGuid(&gST->ConfigurationTable[i].VendorGuid, &Acpi2Guid) == 0 ||
            CompareGuid(&gST->ConfigurationTable[i].VendorGuid, &Acpi1Guid) == 0) {
            bi.RsdpAddress = (uint64_t)gST->ConfigurationTable[i].VendorTable;
        }
        
        if (CompareGuid(&gST->ConfigurationTable[i].VendorGuid, &SmbiosGuid) == 0 ||
            CompareGuid(&gST->ConfigurationTable[i].VendorGuid, &Smbios3Guid) == 0) {
            bi.SmbiosAddress = (uint64_t)gST->ConfigurationTable[i].VendorTable;
        }
    }
    
    // ==================== ЗАГРУЗКА ЯДРА ====================
    DrawBox(2, 25, 76, 6, L"Kernel Loading");
    
    uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, 4, 27);
    PrintColored(L"[1/4] ", COLOR_GREEN, COLOR_BLACK);
    Print(L"Locating boot device... ");
    
    EFI_LOADED_IMAGE *LoadedImage;
    uefi_call_wrapper(gBS->HandleProtocol, 3, ImageHandle, &LoadedImageProtocol, (VOID**)&LoadedImage);
    
    EFI_FILE_IO_INTERFACE *FileSystem;
    uefi_call_wrapper(gBS->HandleProtocol, 3, LoadedImage->DeviceHandle, &FileSystemProtocol, (VOID**)&FileSystem);
    
    EFI_FILE_HANDLE Root;
    uefi_call_wrapper(FileSystem->OpenVolume, 2, FileSystem, &Root);
    
    PrintColored(L"OK\n", COLOR_GREEN, COLOR_BLACK);
    
    uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, 4, 28);
    PrintColored(L"[2/4] ", COLOR_GREEN, COLOR_BLACK);
    Print(L"Opening kernel file... ");
    
    EFI_FILE_HANDLE KernelFile;
    status = uefi_call_wrapper(Root->Open, 5, Root, &KernelFile, L"kernel.bin", EFI_FILE_MODE_READ, 0);
    
    if (EFI_ERROR(status)) {
        PrintColored(L"FAILED\n", COLOR_RED, COLOR_BLACK);
        uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, 4, 29);
        PrintColored(L"Error: kernel.bin not found on boot device!\n", COLOR_RED, COLOR_BLACK);
        PrintColored(L"Please ensure kernel.bin is in the root directory.\n", COLOR_YELLOW, COLOR_BLACK);
        while(1) __asm__ volatile("hlt");
    }
    
    PrintColored(L"OK\n", COLOR_GREEN, COLOR_BLACK);
    
    // Получаем размер файла ядра
    EFI_FILE_INFO *KernelFileInfo;
    UINTN KernelFileInfoSize = sizeof(EFI_FILE_INFO) + 256;
    uefi_call_wrapper(gBS->AllocatePool, 3, EfiLoaderData, KernelFileInfoSize, (VOID**)&KernelFileInfo);
    uefi_call_wrapper(KernelFile->GetInfo, 4, KernelFile, &gEfiFileInfoGuid, &KernelFileInfoSize, KernelFileInfo);
    
    bi.KernelSize = KernelFileInfo->FileSize;
    uefi_call_wrapper(gBS->FreePool, 1, KernelFileInfo);
    
    uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, 4, 29);
    PrintColored(L"[3/4] ", COLOR_GREEN, COLOR_BLACK);
    Print(L"Allocating kernel memory... ");
    
    // Выделяем память для ядра (выровненную по границе страницы)
    UINTN Pages = (bi.KernelSize + 4095) / 4096;
    EFI_PHYSICAL_ADDRESS KernelBase = 0x100000;
    status = uefi_call_wrapper(gBS->AllocatePages, 4, AllocateAddress, EfiLoaderData, Pages, &KernelBase);
    
    if (EFI_ERROR(status)) {
        // Если не удалось выделить по конкретному адресу, пробуем любой
        status = uefi_call_wrapper(gBS->AllocatePages, 4, AllocateAnyPages, EfiLoaderData, Pages, &KernelBase);
    }
    
    if (EFI_ERROR(status)) {
        PrintColored(L"FAILED\n", COLOR_RED, COLOR_BLACK);
        uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, 4, 30);
        PrintColored(L"Error: Cannot allocate memory for kernel!\n", COLOR_RED, COLOR_BLACK);
        while(1) __asm__ volatile("hlt");
    }
    
    bi.KernelBase = KernelBase;
    PrintColored(L"OK\n", COLOR_GREEN, COLOR_BLACK);
    
    uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, 4, 30);
    PrintColored(L"[4/4] ", COLOR_GREEN, COLOR_BLACK);
    Print(L"Loading kernel... ");
    
    // Имитация прогресса загрузки (с реальной загрузкой)
    UINTN ChunkSize = 65536; // 64KB за раз
    UINTN TotalLoaded = 0;
    
    // Простой индикатор загрузки с точками
    UINTN dotCount = 0;
    
    for (UINTN i = 0; TotalLoaded < bi.KernelSize; i++) {
        UINTN ToRead = (bi.KernelSize - TotalLoaded) > ChunkSize ? ChunkSize : (bi.KernelSize - TotalLoaded);
        uefi_call_wrapper(KernelFile->Read, 3, KernelFile, &ToRead, (VOID*)(KernelBase + TotalLoaded));
        TotalLoaded += ToRead;
        
        // Показываем точку каждые 64KB
        if (i % 4 == 0) {
            uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, 24 + dotCount, 30);
            SetColor(COLOR_YELLOW, COLOR_BLACK);
            Print(L".");
            dotCount++;
            SetColor(COLOR_LIGHTGRAY, COLOR_BLACK);
        }
        
        uefi_call_wrapper(gBS->Stall, 1, 5000); // 5ms задержка
    }
    
    // Очищаем строку и выводим завершение
    uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, 24, 30);
    SetColor(COLOR_BLACK, COLOR_BLACK);
    for (UINTN i = 0; i < 40; i++) {
        Print(L" ");
    }
    
    uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, 24, 30);
    SetColor(COLOR_GREEN, COLOR_BLACK);
    Print(L"  COMPLETE");
    
    // Сохраняем информацию о памяти для ядра
    bi.TotalMemory = TotalRAM;
    bi.MemoryMapSize = MemoryMapSize;
    bi.MemoryMap = MemoryMap;
    bi.MemoryMapDescriptorSize = DescriptorSize;
    
    // ==================== ПАНЕЛЬ УПРАВЛЕНИЯ ====================
    DrawBox(2, 33, 76, 5, L"Boot Menu");
    
    uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, 4, 35);
    SetColor(COLOR_YELLOW, COLOR_BLACK);
    Print(L"\nStarting LufiraOS in 5 seconds...\n");
    
    uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, 4, 36);
    Print(L"Press ");
    SetColor(COLOR_CYAN, COLOR_BLACK);
    Print(L"ENTER");
    SetColor(COLOR_YELLOW, COLOR_BLACK);
    Print(L" to boot now, ");
    SetColor(COLOR_CYAN, COLOR_BLACK);
    Print(L"I");
    SetColor(COLOR_YELLOW, COLOR_BLACK);
    Print(L" for info, ");
    SetColor(COLOR_CYAN, COLOR_BLACK);
    Print(L"ESC");
    SetColor(COLOR_YELLOW, COLOR_BLACK);
    Print(L" to cancel");
    
    // ==================== ОБРАТНЫЙ ОТСЧЕТ ====================
    EFI_INPUT_KEY Key;
    BOOLEAN SkipCountdown = FALSE;
    BOOLEAN ShowInfo = FALSE;
    
    // Отображаем начальное состояние
    uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, 4, 38);
    SetColor(COLOR_YELLOW, COLOR_BLACK);
    Print(L"\n\nStarting LufiraOS in 5 seconds... ");
    
    for (int i = 5; i > 0 && !SkipCountdown; i--) {
        // Обновляем обратный отсчет
        uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, 35, 38);
        SetColor(COLOR_CYAN, COLOR_BLACK);
        Print(L"%d sec...", i);
        
        // Ждем 1 секунду, но каждые 100 мс проверяем нажатие клавиши
        for (int j = 0; j < 10; j++) {
            // Проверяем, есть ли нажатие клавиши
            status = uefi_call_wrapper(gST->ConIn->ReadKeyStroke, 2, gST->ConIn, &Key);
            if (!EFI_ERROR(status)) {
                // Обработка нажатия
                if (Key.UnicodeChar == L'\r' || Key.UnicodeChar == L'\n') {
                    SkipCountdown = TRUE;
                    
                    // Очищаем строку с обратным отсчетом
                    uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, 4, 38);
                    SetColor(COLOR_BLACK, COLOR_BLACK);
                    for (UINTN k = 0; k < 76; k++) {
                        Print(L" ");
                    }
                    
                    uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, 4, 38);
                    PrintColored(L"Starting kernel immediately...", COLOR_GREEN, COLOR_BLACK);
                    break;
                } else if (Key.UnicodeChar == L'i' || Key.UnicodeChar == L'I') {
                    ShowInfo = TRUE;
                    break;
                } else if (Key.UnicodeChar == 0x1B || Key.ScanCode == 0x17) { // ESC
                    
                    // Очищаем строку с обратным отсчетом
                    uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, 4, 38);
                    SetColor(COLOR_BLACK, COLOR_BLACK);
                    for (UINTN k = 0; k < 76; k++) {
                        Print(L" ");
                    }
                    
                    uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, 4, 38);
                    PrintColored(L"\nBoot cancelled by user", COLOR_RED, COLOR_BLACK);
                    while(1) __asm__ volatile("hlt");
                }
            }
            // Ждем 100 мс
            uefi_call_wrapper(gBS->Stall, 1, 100000);
        }
        if (SkipCountdown || ShowInfo) {
            break;
        }
    }
    
    // Очищаем обратный отсчет перед продолжением
    uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, 4, 38);
    SetColor(COLOR_BLACK, COLOR_BLACK);
    for (UINTN k = 0; k < 76; k++) {
        Print(L" ");
    }
    
    // ==================== РЕЖИМ ДОПОЛНИТЕЛЬНОЙ ИНФОРМАЦИИ ====================
    if (ShowInfo) {
        ShowAdvancedInfo(LoadedImage, KernelFile, &bi, MemoryMap, MemoryMapSize, DescriptorSize);
        
        // После показа информации, продолжаем обратный отсчет
        uefi_call_wrapper(gST->ConOut->ClearScreen, 1, gST->ConOut);
        uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, 0, 0);
        SetColor(COLOR_YELLOW, COLOR_BLACK);
        Print(L"Resuming boot process...\n");
        uefi_call_wrapper(gBS->Stall, 1, 2000000); // 2 секунды задержки
    }

    // ==================== ЗАГРУЗКА ОБРАЗА ESP (БЫСТРОЕ КОПИРОВАНИЕ) ====================
    EFI_BLOCK_IO_PROTOCOL *BlockIo = NULL;
    EFI_GUID BlockIoGuid = EFI_BLOCK_IO_PROTOCOL_GUID;
    status = uefi_call_wrapper(gBS->HandleProtocol, 3,
        LoadedImage->DeviceHandle, &BlockIoGuid, (VOID**)&BlockIo);

    if (EFI_ERROR(status)) {
        EFI_DEVICE_PATH_PROTOCOL *DevicePath;
        EFI_GUID DevicePathGuid = EFI_DEVICE_PATH_PROTOCOL_GUID;
        status = uefi_call_wrapper(gBS->HandleProtocol, 3,
            LoadedImage->DeviceHandle, &DevicePathGuid, (VOID**)&DevicePath);
        if (!EFI_ERROR(status)) {
            EFI_HANDLE blockHandle;
            status = uefi_call_wrapper(gBS->LocateDevicePath, 3,
                &BlockIoGuid, &DevicePath, &blockHandle);
            if (!EFI_ERROR(status)) {
                status = uefi_call_wrapper(gBS->HandleProtocol, 3,
                    blockHandle, &BlockIoGuid, (VOID**)&BlockIo);
            }
        }
    }

    if (!EFI_ERROR(status) && BlockIo && BlockIo->Media) {
        UINTN BlockSize = BlockIo->Media->BlockSize;
        UINT64 MaxImageSize = 256ULL * 1024 * 1024;
        UINT64 CopySize = (BlockIo->Media->LastBlock + 1) * BlockSize;
        if (CopySize > MaxImageSize)
            CopySize = MaxImageSize;

        UINTN Pages = (UINTN)((CopySize + 4095) / 4096);
        EFI_PHYSICAL_ADDRESS FATBase = 0;
        status = uefi_call_wrapper(gBS->AllocatePages, 4,
            AllocateAnyPages, EfiLoaderData, Pages, &FATBase);

        if (!EFI_ERROR(status)) {
            UINTN MaxBlocksPerTransfer = 1024;
            UINTN TotalBlocks = (UINTN)((CopySize + BlockSize - 1) / BlockSize);
            UINT8* Buffer = (UINT8*)FATBase;

            for (UINTN i = 0; i < TotalBlocks; i += MaxBlocksPerTransfer) {
                UINTN BlocksNow = (TotalBlocks - i) > MaxBlocksPerTransfer ?
                                MaxBlocksPerTransfer : (TotalBlocks - i);
                status = uefi_call_wrapper(BlockIo->ReadBlocks, 5, BlockIo,
                    BlockIo->Media->MediaId, i,
                    BlocksNow * BlockSize,
                    Buffer + ((UINT64)i * BlockSize));
                if (EFI_ERROR(status)) {
                    bi.FATImageBase = 0;
                    bi.FATImageSize = 0;
                    break;
                }
            }

            if (!EFI_ERROR(status)) {
                bi.FATImageBase = FATBase;
                bi.FATImageSize = CopySize;
            } else {
                bi.FATImageBase = 0;
                bi.FATImageSize = 0;
                PrintColored(L"ERROR: Failed to read full FAT image\n",
                            COLOR_RED, COLOR_BLACK);
            }
        } else {
            bi.FATImageBase = 0;
            bi.FATImageSize = 0;
            PrintColored(L"ERROR: Cannot allocate memory for FAT image\n",
                        COLOR_RED, COLOR_BLACK);
        }
    } else {
        bi.FATImageBase = 0;
        bi.FATImageSize = 0;
        PrintColored(L"Warning: Block I/O not available\n", COLOR_YELLOW, COLOR_BLACK);
    }

    
    // ==================== ПЕРЕХОД К ЯДРУ ====================
    uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, 4, 40);
    SetColor(COLOR_GREEN, COLOR_BLACK);
    Print(L"\n==========================================================================\n");
    Print(L"        Switching to 64-bit mode and starting LufiraOS kernel...\n");
    Print(L"==========================================================================\n");
    
    uefi_call_wrapper(gBS->Stall, 1, 2000000); // 2 секунды задержка
    
    // Очищаем экран перед запуском ядра
    SetColor(COLOR_BLACK, COLOR_BLACK);
    uefi_call_wrapper(gST->ConOut->ClearScreen, 1, gST->ConOut);
    
    // Передаем управление ядру
    KernelEntry kStart = (KernelEntry)bi.KernelBase;
    kStart(&bi);
    
    // Если ядро вернет управление (не должно происходить)
    while(1) { __asm__ volatile("hlt"); }
}