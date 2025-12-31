#include <efi.h>
#include <efilib.h>

typedef struct {
    uint64_t FrameBufferBase;
    uint64_t FrameBufferSize;
    uint32_t HorizontalResolution;
    uint32_t VerticalResolution;
    uint32_t PixelsPerScanLine;
} BootInfo;

typedef void (*KernelEntry)(BootInfo*);

EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    InitializeLib(ImageHandle, SystemTable);
    uefi_call_wrapper(ST->ConOut->ClearScreen, 1, ST->ConOut);
    
    Print(L"================================================\n");
    Print(L"       LufiraOS UEFI Bootloader v1.2            \n");
    Print(L"================================================\n\n");

    // 1. Базовая информация о прошивке
    Print(L"Firmware Vendor:      %s\n", ST->FirmwareVendor);
    Print(L"UEFI Specification:   %d.%02d\n", ST->Hdr.Revision >> 16, ST->Hdr.Revision & 0xFFFF);

    // 2. Системное время (Дата и время 2025)
    EFI_TIME Time;
    if (!EFI_ERROR(uefi_call_wrapper(RT->GetTime, 2, &Time, NULL))) {
        Print(L"System Date/Time:     %02d/%02d/2025  %02d:%02d:%02d\n", 
              Time.Day, Time.Month, Time.Hour, Time.Minute, Time.Second);
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
    BootInfo bi;
    EFI_STATUS status = uefi_call_wrapper(BS->LocateProtocol, 3, &gopGuid, NULL, (VOID**)&gop);
    if (!EFI_ERROR(status)) {
        bi.FrameBufferBase = gop->Mode->FrameBufferBase;
        bi.FrameBufferSize = gop->Mode->FrameBufferSize;
        bi.HorizontalResolution = gop->Mode->Info->HorizontalResolution;
        bi.VerticalResolution = gop->Mode->Info->VerticalResolution;
        bi.PixelsPerScanLine = gop->Mode->Info->PixelsPerScanLine;
        Print(L"Video Framebuffer:    0x%lx\n", bi.FrameBufferBase);
        Print(L"Current Resolution:   %dx%d\n", bi.HorizontalResolution, bi.VerticalResolution);
        Print(L"Pixels Per Line:      %d px\n", bi.PixelsPerScanLine);
    }

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

    EFI_PHYSICAL_ADDRESS KernelBase = 0x100000;
    uefi_call_wrapper(BS->AllocatePages, 4, AllocateAddress, EfiLoaderData, 100, &KernelBase);
    UINTN KernelSize = 0x100000; 
    uefi_call_wrapper(KernelFile->Read, 3, KernelFile, &KernelSize, (VOID*)KernelBase);
    Print(L"Kernel Memory Range:  0x%lx - 0x%lx\n", KernelBase, KernelBase + KernelSize);

    // 8. Финальная стадия
    Print(L"\nAll systems ready. Handing over to LufiraOS...\n");
    uefi_call_wrapper(BS->Stall, 1, 3000000); 

    // Обновляем карту памяти перед самым выходом
    uefi_call_wrapper(BS->GetMemoryMap, 5, &MemoryMapSize, MemoryMap, &MapKey, &DescriptorSize, &DescriptorVersion);
    status = uefi_call_wrapper(BS->ExitBootServices, 2, ImageHandle, MapKey);
    if (EFI_ERROR(status)) return status;

    KernelEntry kStart = (KernelEntry)KernelBase;
    kStart(&bi);

    while(1) { __asm__ volatile("hlt"); }
}
