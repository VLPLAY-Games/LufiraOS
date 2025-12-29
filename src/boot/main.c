#include <efi.h>
#include <efilib.h>

EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
    EFI_STATUS Status;
    EFI_INPUT_KEY Key;
    
    // Инициализация библиотеки
    InitializeLib(ImageHandle, SystemTable);
    
    // Очищаем экран
    uefi_call_wrapper(ST->ConOut->ClearScreen, 1, ST->ConOut);
    
    // Выводим приветствие
    Print(L"================================\n");
    Print(L"      LufiraOS Bootloader\n");
    Print(L"================================\n\n");
    
    // Информация о системе
    Print(L"System Information:\n");
    Print(L"  UEFI Firmware: %s (Rev %d)\n", ST->FirmwareVendor, ST->FirmwareRevision);
    Print(L"  UEFI Version: %d.%02d\n", ST->Hdr.Revision >> 16, ST->Hdr.Revision & 0xFFFF);
    
    // Получаем информацию о памяти
    UINTN MemoryMapSize = 0;
    UINTN MapKey, DescriptorSize;
    UINT32 DescriptorVersion;
    
    Status = uefi_call_wrapper(ST->BootServices->GetMemoryMap, 5,
                               &MemoryMapSize,
                               NULL,
                               &MapKey,
                               &DescriptorSize,
                               &DescriptorVersion);
    
    if (Status == EFI_BUFFER_TOO_SMALL) {
        Print(L"\nMemory Information:\n");
        Print(L"  Memory Map Size: %u bytes\n", MemoryMapSize);
        Print(L"  Descriptor Size: %u bytes\n", DescriptorSize);
    }
    
    Print(L"\nBooting LufiraOS...\n");
    
    // Ждем нажатия клавиши
    Print(L"\nPress any key to continue...\n");
    uefi_call_wrapper(ST->ConIn->Reset, 2, ST->ConIn, FALSE);
    
    while (uefi_call_wrapper(ST->ConIn->ReadKeyStroke, 2, ST->ConIn, &Key) == EFI_NOT_READY);
    
    // Завершаем работу
    Print(L"\nShutting down...\n");
    
    // Даем время прочитать сообщение
    uefi_call_wrapper(ST->BootServices->Stall, 1, 2000000); // 2 секунды
    
    // Пытаемся выключиться
    Status = uefi_call_wrapper(ST->RuntimeServices->ResetSystem, 4,
                               EfiResetShutdown,
                               EFI_SUCCESS,
                               0,
                               NULL);
    
    // Если не удалось выключиться, зависаем
    while (1) {
        __asm__("hlt");
    }
    
    return EFI_SUCCESS;
}