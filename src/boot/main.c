#include <efi.h>
#include <efilib.h>

// Внешнее объявление функции ядра
extern UINT64 kernel_main(void);

// Макрос для выравнивания
#define ALIGN_UP(addr, align) (((addr) + (align) - 1) & ~((align) - 1))
#define ALIGN_DOWN(addr, align) ((addr) & ~((align) - 1))

// Функция для получения информации о памяти
EFI_STATUS GetMemoryInfo(VOID) {
    EFI_STATUS Status;
    UINTN MemoryMapSize = 0;
    UINTN MapKey, DescriptorSize;
    UINT32 DescriptorVersion;
    EFI_MEMORY_DESCRIPTOR *MemoryMap = NULL;
    UINTN MemoryMapSizeNeeded;
    
    // Первый вызов для получения размера
    Status = uefi_call_wrapper(ST->BootServices->GetMemoryMap, 5,
                               &MemoryMapSize,
                               NULL,
                               &MapKey,
                               &DescriptorSize,
                               &DescriptorVersion);
    
    if (Status != EFI_BUFFER_TOO_SMALL) {
        Print(L"  Failed to get memory map size: %r\n", Status);
        return Status;
    }
    
    // Выделяем память с запасом
    MemoryMapSizeNeeded = MemoryMapSize + (DescriptorSize * 2);
    Status = uefi_call_wrapper(ST->BootServices->AllocatePool, 3,
                               EfiLoaderData,
                               MemoryMapSizeNeeded,
                               (VOID**)&MemoryMap);
    
    if (EFI_ERROR(Status)) {
        Print(L"  Failed to allocate memory for map\n");
        return Status;
    }
    
    // Получаем карту памяти
    Status = uefi_call_wrapper(ST->BootServices->GetMemoryMap, 5,
                               &MemoryMapSize,
                               MemoryMap,
                               &MapKey,
                               &DescriptorSize,
                               &DescriptorVersion);
    
    if (EFI_ERROR(Status)) {
        Print(L"  Failed to get memory map: %r\n", Status);
        FreePool(MemoryMap);
        return Status;
    }
    
    // Анализируем карту памяти
    UINTN NumEntries = MemoryMapSize / DescriptorSize;
    EFI_MEMORY_DESCRIPTOR *Desc = MemoryMap;
    UINT64 TotalMemory = 0;
    UINT64 AvailableMemory = 0;
    UINT64 ReservedMemory = 0;
    UINT64 ACPIMemory = 0;
    UINT64 ACPINVSMemory = 0;
    
    for (UINTN i = 0; i < NumEntries; i++) {
        UINT64 RegionSize = Desc->NumberOfPages * EFI_PAGE_SIZE;
        
        switch (Desc->Type) {
            case EfiReservedMemoryType:
                ReservedMemory += RegionSize;
                break;
            case EfiLoaderCode:
            case EfiLoaderData:
            case EfiBootServicesCode:
            case EfiBootServicesData:
            case EfiConventionalMemory:
                AvailableMemory += RegionSize;
                TotalMemory += RegionSize;
                break;
            case EfiRuntimeServicesCode:
            case EfiRuntimeServicesData:
                TotalMemory += RegionSize;
                break;
            case EfiACPIReclaimMemory:
                ACPIMemory += RegionSize;
                TotalMemory += RegionSize;
                break;
            case EfiACPIMemoryNVS:
                ACPINVSMemory += RegionSize;
                TotalMemory += RegionSize;
                break;
            case EfiMemoryMappedIO:
            case EfiMemoryMappedIOPortSpace:
            case EfiPalCode:
                TotalMemory += RegionSize;
                break;
            default:
                TotalMemory += RegionSize;
                break;
        }
        
        // Переходим к следующему дескриптору
        Desc = (EFI_MEMORY_DESCRIPTOR*)((UINT8*)Desc + DescriptorSize);
    }
    
    // Выводим информацию о памяти
    Print(L"  Total Memory: %llu MB\n", TotalMemory / (1024 * 1024));
    Print(L"  Available Memory: %llu MB\n", AvailableMemory / (1024 * 1024));
    Print(L"  Reserved Memory: %llu MB\n", ReservedMemory / (1024 * 1024));
    Print(L"  ACPI Memory: %llu KB\n", ACPIMemory / 1024);
    Print(L"  ACPI NVS Memory: %llu KB\n", ACPINVSMemory / 1024);
    Print(L"  Memory Regions: %d\n", NumEntries);
    
    // Освобождаем память
    FreePool(MemoryMap);
    
    return EFI_SUCCESS;
}

// Функция для получения информации о прошивке
VOID GetFirmwareInfo(VOID) {
    // Информация о прошивке
    Print(L"  Firmware Vendor: %s\n", ST->FirmwareVendor);
    Print(L"  Firmware Revision: %d.%d\n", 
          ST->FirmwareRevision >> 16, 
          ST->FirmwareRevision & 0xFFFF);
    
    // Версия UEFI
    Print(L"  UEFI Version: %d.%d\n", 
          ST->Hdr.Revision >> 16, 
          ST->Hdr.Revision & 0xFFFF);
}

// Функция для получения времени работы системы
VOID GetSystemTimeInfo(VOID) {
    EFI_TIME Time;
    EFI_STATUS Status;
    
    // Получаем текущее время
    Status = uefi_call_wrapper(ST->RuntimeServices->GetTime, 2, &Time, NULL);
    
    if (!EFI_ERROR(Status)) {
        Print(L"  System Time: %d-%02d-%02d %02d:%02d:%02d\n",
              Time.Year, Time.Month, Time.Day,
              Time.Hour, Time.Minute, Time.Second);
    }
}

// Функция для получения информации о графике
EFI_STATUS GetGraphicsInfo(VOID) {
    EFI_STATUS Status;
    EFI_GUID GraphicsOutputProtocolGuid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *GOP = NULL;
    
    // Пытаемся получить GOP
    Status = uefi_call_wrapper(ST->BootServices->LocateProtocol, 3,
                               &GraphicsOutputProtocolGuid,
                               NULL,
                               (VOID**)&GOP);
    
    if (!EFI_ERROR(Status) && GOP) {
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *ModeInfo;
        UINTN SizeOfInfo;
        
        // Получаем информацию о текущем режиме
        Status = uefi_call_wrapper(GOP->QueryMode, 4,
                                   GOP,
                                   GOP->Mode->Mode,
                                   &SizeOfInfo,
                                   &ModeInfo);
        
        if (!EFI_ERROR(Status)) {
            Print(L"  Graphics Mode: %dx%d\n", 
                  ModeInfo->HorizontalResolution,
                  ModeInfo->VerticalResolution);
            Print(L"  Pixel Format: ");
            
            switch (ModeInfo->PixelFormat) {
                case PixelRedGreenBlueReserved8BitPerColor:
                    Print(L"RGB (32-bit)\n");
                    break;
                case PixelBlueGreenRedReserved8BitPerColor:
                    Print(L"BGR (32-bit)\n");
                    break;
                case PixelBitMask:
                    Print(L"Bit Mask\n");
                    break;
                case PixelBltOnly:
                    Print(L"BLT Only\n");
                    break;
                default:
                    Print(L"Unknown\n");
                    break;
            }
            
            FreePool(ModeInfo);
        }
    } else {
        Print(L"  No graphics protocol found\n");
    }
    
    return Status;
}

// Функция для получения информации о диске
EFI_STATUS GetDiskInfo(VOID) {
    EFI_STATUS Status;
    UINTN HandleCount = 0;
    EFI_HANDLE *HandleBuffer = NULL;
    EFI_GUID BlockIoProtocolGuid = EFI_BLOCK_IO_PROTOCOL_GUID;
    
    // Получаем все handles
    Status = uefi_call_wrapper(ST->BootServices->LocateHandleBuffer, 5,
                               AllHandles,
                               NULL,
                               NULL,
                               &HandleCount,
                               &HandleBuffer);
    
    if (EFI_ERROR(Status)) {
        return Status;
    }
    
    UINTN DiskCount = 0;
    
    // Ищем устройства с протоколом Block I/O
    for (UINTN i = 0; i < HandleCount; i++) {
        EFI_BLOCK_IO_PROTOCOL *BlockIo = NULL;
        
        Status = uefi_call_wrapper(ST->BootServices->HandleProtocol, 3,
                                   HandleBuffer[i],
                                   &BlockIoProtocolGuid,
                                   (VOID**)&BlockIo);
        
        if (!EFI_ERROR(Status) && BlockIo && BlockIo->Media) {
            DiskCount++;
            if (DiskCount == 1) { // Показываем только первый диск
                UINT64 DiskSize = (UINT64)BlockIo->Media->LastBlock * 
                                 BlockIo->Media->BlockSize;
                
                Print(L"  Boot Disk Size: %llu MB\n", 
                      DiskSize / (1024 * 1024));
                Print(L"  Disk Block Size: %u bytes\n", 
                      BlockIo->Media->BlockSize);
                Print(L"  Disk Media Present: %s\n", 
                      BlockIo->Media->MediaPresent ? L"Yes" : L"No");
                Print(L"  Disk Read-Only: %s\n", 
                      BlockIo->Media->ReadOnly ? L"Yes" : L"No");
            }
        }
    }
    
    Print(L"  Storage Devices Found: %d\n", DiskCount);
    
    if (HandleBuffer) {
        FreePool(HandleBuffer);
    }
    
    return EFI_SUCCESS;
}

// Функция для отображения детальной карты памяти
VOID ShowDetailedMemoryMap(VOID) {
    EFI_STATUS Status;
    UINTN MemoryMapSize = 0;
    UINTN MapKey, DescriptorSize;
    UINT32 DescriptorVersion;
    EFI_MEMORY_DESCRIPTOR *MemoryMap = NULL;
    UINTN MemoryMapSizeNeeded;
    
    // Получаем размер карты
    Status = uefi_call_wrapper(ST->BootServices->GetMemoryMap, 5,
                               &MemoryMapSize,
                               NULL,
                               &MapKey,
                               &DescriptorSize,
                               &DescriptorVersion);
    
    if (Status != EFI_BUFFER_TOO_SMALL) {
        Print(L"  Cannot get memory map size\n");
        return;
    }
    
    // Выделяем память
    MemoryMapSizeNeeded = MemoryMapSize + DescriptorSize * 2;
    Status = uefi_call_wrapper(ST->BootServices->AllocatePool, 3,
                               EfiLoaderData,
                               MemoryMapSizeNeeded,
                               (VOID**)&MemoryMap);
    
    if (EFI_ERROR(Status)) {
        Print(L"  Failed to allocate memory\n");
        return;
    }
    
    // Получаем карту
    Status = uefi_call_wrapper(ST->BootServices->GetMemoryMap, 5,
                               &MemoryMapSize,
                               MemoryMap,
                               &MapKey,
                               &DescriptorSize,
                               &DescriptorVersion);
    
    if (EFI_ERROR(Status)) {
        Print(L"  Failed to get memory map\n");
        FreePool(MemoryMap);
        return;
    }
    
    // Выводим заголовок
    Print(L"\n  Type                Start Address        End Address          Size       Attributes\n");
    Print(L"  -----------------------------------------------------------------------------------\n");
    
    // Проходим по всем дескрипторам
    UINTN NumEntries = MemoryMapSize / DescriptorSize;
    EFI_MEMORY_DESCRIPTOR *Desc = MemoryMap;
    
    for (UINTN i = 0; i < NumEntries; i++) {
        UINT64 StartAddr = Desc->PhysicalStart;
        UINT64 EndAddr = StartAddr + (Desc->NumberOfPages * EFI_PAGE_SIZE) - 1;
        UINT64 Size = Desc->NumberOfPages * EFI_PAGE_SIZE;
        
        // Тип памяти
        const CHAR16 *TypeStr;
        switch (Desc->Type) {
            case EfiReservedMemoryType: TypeStr = L"Reserved"; break;
            case EfiLoaderCode: TypeStr = L"Loader Code"; break;
            case EfiLoaderData: TypeStr = L"Loader Data"; break;
            case EfiBootServicesCode: TypeStr = L"Boot Code"; break;
            case EfiBootServicesData: TypeStr = L"Boot Data"; break;
            case EfiRuntimeServicesCode: TypeStr = L"Runtime Code"; break;
            case EfiRuntimeServicesData: TypeStr = L"Runtime Data"; break;
            case EfiConventionalMemory: TypeStr = L"Available"; break;
            case EfiUnusableMemory: TypeStr = L"Unusable"; break;
            case EfiACPIReclaimMemory: TypeStr = L"ACPI Reclaim"; break;
            case EfiACPIMemoryNVS: TypeStr = L"ACPI NVS"; break;
            case EfiMemoryMappedIO: TypeStr = L"MMIO"; break;
            case EfiMemoryMappedIOPortSpace: TypeStr = L"I/O Port"; break;
            case EfiPalCode: TypeStr = L"PAL Code"; break;
            default: TypeStr = L"Unknown"; break;
        }
        
        // Выводим информацию
        Print(L"  %-18s 0x%016llx 0x%016llx %6llu KB  ",
              TypeStr, StartAddr, EndAddr, Size / 1024);
        
        // Атрибуты
        if (Desc->Attribute & EFI_MEMORY_UC) Print(L"UC ");
        if (Desc->Attribute & EFI_MEMORY_WC) Print(L"WC ");
        if (Desc->Attribute & EFI_MEMORY_WT) Print(L"WT ");
        if (Desc->Attribute & EFI_MEMORY_WB) Print(L"WB ");
        if (Desc->Attribute & EFI_MEMORY_UCE) Print(L"UCE ");
        if (Desc->Attribute & EFI_MEMORY_WP) Print(L"WP ");
        if (Desc->Attribute & EFI_MEMORY_RP) Print(L"RP ");
        if (Desc->Attribute & EFI_MEMORY_XP) Print(L"XP ");
        if (Desc->Attribute & EFI_MEMORY_RUNTIME) Print(L"RT ");
        
        Print(L"\n");
        
        // Переходим к следующему дескриптору
        Desc = (EFI_MEMORY_DESCRIPTOR*)((UINT8*)Desc + DescriptorSize);
    }
    
    Print(L"\n  Total entries: %d\n", NumEntries);
    
    // Освобождаем память
    FreePool(MemoryMap);
}

// Главная функция загрузчика
EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
    EFI_STATUS Status;
    EFI_INPUT_KEY Key;
    
    // Инициализация библиотеки
    InitializeLib(ImageHandle, SystemTable);
    
    // Очищаем экран
    uefi_call_wrapper(ST->ConOut->ClearScreen, 1, ST->ConOut);
    
    // Выводим заголовок
    Print(L"========================================\n");
    Print(L"         LufiraOS Bootloader\n");
    Print(L"            Version 1.0\n");
    Print(L"========================================\n\n");
    
    // Раздел 1: Информация о системе
    Print(L"[1] System Information:\n");
    Print(L"  Boot Mode: ");
    
    // Определяем режим загрузки
    if (sizeof(VOID*) == 8) {
        Print(L"UEFI 64-bit\n");
    } else {
        Print(L"UEFI 32-bit\n");
    }
    
    GetFirmwareInfo();
    GetSystemTimeInfo();
    
    // Раздел 2: Информация о памяти
    Print(L"\n[2] Memory Information:\n");
    Status = GetMemoryInfo();
    if (EFI_ERROR(Status)) {
        Print(L"  Failed to retrieve memory information\n");
    }
    
    // Раздел 3: Информация о графике
    Print(L"\n[3] Graphics Information:\n");
    Status = GetGraphicsInfo();
    if (EFI_ERROR(Status)) {
        Print(L"  No graphics protocol found\n");
    }
    
    // Раздел 4: Информация о хранилище
    Print(L"\n[4] Storage Information:\n");
    Status = GetDiskInfo();
    if (EFI_ERROR(Status)) {
        Print(L"  Failed to retrieve disk information\n");
    }
    
    // Раздел 5: Информация о консоли
    Print(L"\n[5] Console Information:\n");
    Print(L"  Console Control Protocol: %s\n",
          ST->ConsoleInHandle ? L"Available" : L"Not Available");
    
    // Запрашиваем у пользователя подтверждение
    Print(L"\n========================================\n");
    Print(L"Booting sequence ready.\n\n");
    
    // Опции загрузки
    Print(L"Options:\n");
    Print(L"  1. Press any key to boot immediately\n");
    Print(L"  2. Wait 5 seconds for auto-boot\n");
    Print(L"  3. Press 'M' to show memory map details\n");
    Print(L"  4. Press 'R' to reboot\n");
    
    // Таймер авто-загрузки
    INTN Timeout = 5;
    while (Timeout > 0) {
        Print(L"\rAuto-boot in %d seconds... ", Timeout);
        
        // Проверяем нажатие клавиши
        Status = uefi_call_wrapper(ST->ConIn->ReadKeyStroke, 2, ST->ConIn, &Key);
        if (!EFI_ERROR(Status)) {
            if (Key.UnicodeChar == 'm' || Key.UnicodeChar == 'M') {
                // Показываем детальную карту памяти
                Print(L"\n\n[Memory Map Details]\n");
                ShowDetailedMemoryMap();
                Print(L"\nPress any key to continue booting...\n");
                uefi_call_wrapper(ST->ConIn->Reset, 2, ST->ConIn, FALSE);
                while (uefi_call_wrapper(ST->ConIn->ReadKeyStroke, 2, ST->ConIn, &Key) == EFI_NOT_READY);
            } else if (Key.UnicodeChar == 'r' || Key.UnicodeChar == 'R') {
                // Перезагрузка системы
                Print(L"\n\nRebooting system...\n");
                uefi_call_wrapper(ST->RuntimeServices->ResetSystem, 4, 
                                  EfiResetCold, EFI_SUCCESS, 0, NULL);
            }
            break;
        }
        
        // Задержка 1 секунда
        uefi_call_wrapper(ST->BootServices->Stall, 1, 1000000);
        Timeout--;
    }
    
    Print(L"\n\nStarting kernel...\n");
    
    // Выходим из Boot Services
    UINTN MemoryMapSize = 0;
    UINTN MapKey, DescriptorSize;
    UINT32 DescriptorVersion;
    
    // Получаем карту памяти для ExitBootServices
    Status = uefi_call_wrapper(ST->BootServices->GetMemoryMap, 5,
                               &MemoryMapSize,
                               NULL,
                               &MapKey,
                               &DescriptorSize,
                               &DescriptorVersion);
    
    if (Status == EFI_BUFFER_TOO_SMALL) {
        // Выделяем память для карты
        EFI_MEMORY_DESCRIPTOR *MemoryMap;
        UINTN MemoryMapSizeNeeded = MemoryMapSize + DescriptorSize;
        
        Status = uefi_call_wrapper(ST->BootServices->AllocatePool, 3,
                                   EfiLoaderData,
                                   MemoryMapSizeNeeded,
                                   (VOID**)&MemoryMap);
        
        if (!EFI_ERROR(Status)) {
            Status = uefi_call_wrapper(ST->BootServices->GetMemoryMap, 5,
                                       &MemoryMapSize,
                                       MemoryMap,
                                       &MapKey,
                                       &DescriptorSize,
                                       &DescriptorVersion);
            
            if (!EFI_ERROR(Status)) {
                // Выходим из Boot Services
                Status = uefi_call_wrapper(ST->BootServices->ExitBootServices, 2,
                                           ImageHandle, MapKey);
            }
            
            FreePool(MemoryMap);
        }
    }
    
    if (EFI_ERROR(Status)) {
        Print(L"Failed to exit boot services: %r\n", Status);
        return Status;
    }
    
    // Запускаем ядро
    UINT64 kernel_result = kernel_main();
    
    // Если ядро вернуло управление
    Print(L"\nKernel returned: 0x%llx\n", kernel_result);
    Print(L"System halted.\n");
    
    while (1) {
        __asm__("hlt");
    }
    
    return EFI_SUCCESS;
}