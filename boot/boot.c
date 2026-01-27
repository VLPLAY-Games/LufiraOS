#include <efi.h>
#include <efilib.h>

typedef struct {
    uint64_t FrameBufferBase;
    uint64_t FrameBufferSize;
    uint32_t HorizontalResolution;
    uint32_t VerticalResolution;
    uint32_t PixelsPerScanLine;
    uint32_t PixelFormat;  // 0 = RGB, 1 = BGR
    uint64_t TotalMemory;   // Общая память в байтах
    uint64_t MemoryMapSize; // Размер карты памяти
    void* MemoryMap;        // Указатель на карту памяти
    uint32_t MemoryMapDescriptorSize; // Размер дескриптора
} BootInfo;

typedef void (*KernelEntry)(BootInfo*);

EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    InitializeLib(ImageHandle, SystemTable);
    uefi_call_wrapper(ST->ConOut->ClearScreen, 1, ST->ConOut);
    
    Print(L"================================================\n");
    Print(L"       LufiraOS UEFI Bootloader v0.1.2         \n");
    Print(L"================================================\n\n");

    // 1. Базовая информация о прошивке
    Print(L"Firmware Vendor:      %s\n", ST->FirmwareVendor);
    Print(L"UEFI Specification:   %d.%02d\n", ST->Hdr.Revision >> 16, ST->Hdr.Revision & 0xFFFF);
    EFI_GUID gEfiGlobalVariableGuid = {0x8BE4DF61, 0x93CA, 0x11D2, {0xAA, 0x0D, 0x00, 0xE0, 0x98, 0x03, 0x2B, 0x8C}};
    UINT8 SecureBoot;
    UINTN DataSize = sizeof(SecureBoot);
    EFI_STATUS sb_status = uefi_call_wrapper(RT->GetVariable, 5, 
        L"SecureBoot", 
        &gEfiGlobalVariableGuid,
        NULL, 
        &DataSize, 
        &SecureBoot);

    if (!EFI_ERROR(sb_status)) {
        Print(L"Secure Boot:          %s\n", SecureBoot ? L"Enabled" : L"Disabled");
    } else {
        Print(L"Secure Boot:          Unknown/Not Supported\n");
    }

    // 2. Системное время
    EFI_TIME Time;
    if (!EFI_ERROR(uefi_call_wrapper(RT->GetTime, 2, &Time, NULL))) {
        Print(L"System Date/Time:     %02d/%02d/%04d  %02d:%02d:%02d\n", 
              Time.Day, Time.Month, Time.Year, Time.Hour, Time.Minute, Time.Second);
    }

    // 3. Информация о памяти (Memory Map)
    UINTN MemoryMapSize = 0, MapKey, DescriptorSize;
    UINT32 DescriptorVersion;
    uefi_call_wrapper(BS->GetMemoryMap, 5, &MemoryMapSize, NULL, &MapKey, &DescriptorSize, &DescriptorVersion);
    MemoryMapSize += 2 * DescriptorSize;
    EFI_MEMORY_DESCRIPTOR *MemoryMap;
    uefi_call_wrapper(BS->AllocatePool, 3, EfiLoaderData, MemoryMapSize, (VOID**)&MemoryMap);
    uefi_call_wrapper(BS->GetMemoryMap, 5, &MemoryMapSize, MemoryMap, &MapKey, &DescriptorSize, &DescriptorVersion);

    uint64_t TotalRAM = 0;
    for (UINTN i = 0; i < (MemoryMapSize / DescriptorSize); i++) {
        EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR*)((UINT8*)MemoryMap + (i * DescriptorSize));
        if (d->Type == EfiConventionalMemory || d->Type == EfiLoaderCode || d->Type == EfiLoaderData)
            TotalRAM += d->NumberOfPages * 4096;
    }
    Print(L"Available RAM:        %ld MB\n", TotalRAM / 1024 / 1024);

    // 4. Поиск ACPI и SMBIOS (Configuration Tables)
    Print(L"Config Tables Count:  %d\n", ST->NumberOfTableEntries);
    for (UINTN i = 0; i < ST->NumberOfTableEntries; i++) {
        EFI_GUID Acpi2Guid = ACPI_20_TABLE_GUID;
        if (CompareGuid(&ST->ConfigurationTable[i].VendorGuid, &Acpi2Guid) == 0) {
            Print(L"ACPI 2.0 Table:       Found at 0x%lx\n", ST->ConfigurationTable[i].VendorTable);
        }
    }
    


    // 5. Данные об образе (Image Info)
    EFI_LOADED_IMAGE *LoadedImage;
    uefi_call_wrapper(BS->HandleProtocol, 3, ImageHandle, &LoadedImageProtocol, (VOID**)&LoadedImage);
    Print(L"Boot Image Base:      0x%lx\n", LoadedImage->ImageBase);
    Print(L"Boot Image Size:      %ld KB\n", LoadedImage->ImageSize / 1024);

    // 6. Графика (GOP)
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;
    EFI_GUID gopGuid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    BootInfo bi = {0};
    EFI_STATUS status = uefi_call_wrapper(BS->LocateProtocol, 3, &gopGuid, NULL, (VOID**)&gop);
    if (!EFI_ERROR(status)) {
        bi.FrameBufferBase = gop->Mode->FrameBufferBase;
        bi.FrameBufferSize = gop->Mode->FrameBufferSize;
        bi.HorizontalResolution = gop->Mode->Info->HorizontalResolution;
        bi.VerticalResolution = gop->Mode->Info->VerticalResolution;
        bi.PixelsPerScanLine = gop->Mode->Info->PixelsPerScanLine;
        
        // Определяем формат пикселей
        if (gop->Mode->Info->PixelFormat == PixelBlueGreenRedReserved8BitPerColor) {
            bi.PixelFormat = 1; // BGR
            Print(L"Pixel Format:          BGR (Blue-Green-Red)\n");
        } else if (gop->Mode->Info->PixelFormat == PixelRedGreenBlueReserved8BitPerColor) {
            bi.PixelFormat = 0; // RGB
            Print(L"Pixel Format:          RGB (Red-Green-Blue)\n");
        } else {
            bi.PixelFormat = 0;
            Print(L"Pixel Format:          RGB (default, unknown format)\n");
        }
        
        Print(L"Video Framebuffer:    0x%lx\n", bi.FrameBufferBase);
        Print(L"Current Resolution:   %dx%d\n", bi.HorizontalResolution, bi.VerticalResolution);
        Print(L"Pixels Per Line:      %d px\n", bi.PixelsPerScanLine);
    }

    // Сохраняем информацию о памяти для ядра
    bi.TotalMemory = TotalRAM;
    bi.MemoryMapSize = MemoryMapSize;
    bi.MemoryMap = MemoryMap;
    bi.MemoryMapDescriptorSize = DescriptorSize;

    // 7. Загрузка ядра
    EFI_FILE_IO_INTERFACE *FileSystem;
    uefi_call_wrapper(BS->HandleProtocol, 3, LoadedImage->DeviceHandle, &FileSystemProtocol, (VOID**)&FileSystem);
    EFI_FILE_HANDLE Root;
    uefi_call_wrapper(FileSystem->OpenVolume, 2, FileSystem, &Root);

    Print(L"\nLoading LufiraOS Kernel (kernel.bin)...\n");
    EFI_FILE_HANDLE KernelFile;
    status = uefi_call_wrapper(Root->Open, 5, Root, &KernelFile, L"kernel.bin", EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(status)) {
        Print(L"HALT: kernel.bin not found on Boot Device!\n");
        while(1);
    }

    // Получаем информацию о файле для определения размера
    EFI_FILE_INFO *FileInfo;
    UINTN FileInfoSize = sizeof(EFI_FILE_INFO) + 128;
    uefi_call_wrapper(BS->AllocatePool, 3, EfiLoaderData, FileInfoSize, (VOID**)&FileInfo);
    status = uefi_call_wrapper(KernelFile->GetInfo, 4, KernelFile, &gEfiFileInfoGuid, &FileInfoSize, FileInfo);
    
    if (!EFI_ERROR(status)) {
        UINTN KernelSize = FileInfo->FileSize;
        Print(L"Kernel File Size:     %ld KB\n", KernelSize / 1024);
    }
    
    uefi_call_wrapper(BS->FreePool, 1, FileInfo);

    EFI_PHYSICAL_ADDRESS KernelBase = 0x100000;
    uefi_call_wrapper(BS->AllocatePages, 4, AllocateAddress, EfiLoaderData, 100, &KernelBase);
    UINTN KernelSize = 0x100000; 
    uefi_call_wrapper(KernelFile->Read, 3, KernelFile, &KernelSize, (VOID*)KernelBase);
    Print(L"Kernel Memory Range:  0x%lx - 0x%lx\n", KernelBase, KernelBase + KernelSize);

    // 8. Обратный отсчет перед загрузкой
    Print(L"\nAll systems ready. Handing over to LufiraOS in 5 seconds...\n");
    Print(L"Press ENTER to skip countdown.\n\n");
    
    EFI_INPUT_KEY Key;
    UINTN Index;
    EFI_EVENT TimerEvent;
    UINT64 TimerPeriod = 10000000;
    
    uefi_call_wrapper(BS->CreateEvent, 5, EVT_TIMER, 0, NULL, NULL, &TimerEvent);
    
    for (int i = 5; i > 0; i--) {
        status = uefi_call_wrapper(ST->ConIn->ReadKeyStroke, 2, ST->ConIn, &Key);
        if (!EFI_ERROR(status)) {
            if (Key.UnicodeChar == L'\r' || Key.UnicodeChar == L'\n') {
                Print(L"Countdown skipped. Starting kernel now...\n");
                break;
            }
        }
        
        uefi_call_wrapper(BS->SetTimer, 3, TimerEvent, TimerRelative, TimerPeriod);
        EFI_EVENT WaitList[] = {TimerEvent, ST->ConIn->WaitForKey};
        status = uefi_call_wrapper(BS->WaitForEvent, 3, 2, WaitList, &Index);
        
        if (Index == 0) {
            Print(L"%d... ", i);
        } else {
            uefi_call_wrapper(ST->ConIn->ReadKeyStroke, 2, ST->ConIn, &Key);
            if (Key.UnicodeChar == L'\r' || Key.UnicodeChar == L'\n') {
                Print(L"Countdown skipped. Starting kernel now...\n");
                break;
            }
        }
    }
    
    uefi_call_wrapper(BS->CloseEvent, 1, TimerEvent);
    
    Print(L"\nStarting LufiraOS Kernel...\n");
    uefi_call_wrapper(BS->Stall, 1, 1000000);

    // Обновляем карту памяти перед самым выходом
    uefi_call_wrapper(BS->GetMemoryMap, 5, &MemoryMapSize, MemoryMap, &MapKey, &DescriptorSize, &DescriptorVersion);
    status = uefi_call_wrapper(BS->ExitBootServices, 2, ImageHandle, MapKey);
    if (EFI_ERROR(status)) return status;

    KernelEntry kStart = (KernelEntry)KernelBase;
    kStart(&bi);

    while(1) { __asm__ volatile("hlt"); }
}