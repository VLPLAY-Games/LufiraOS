#include "exit_boot.h"

VOID ExitBootServicesWrapper(BootInfo *bi, EFI_HANDLE ImageHandle) {
    EFI_STATUS status;
    UINTN MemoryMapSize = bi->MemoryMapSize;
    UINTN MapKey;
    UINTN DescriptorSize = bi->MemoryMapDescriptorSize;
    UINT32 DescriptorVersion;
    
    status = uefi_call_wrapper(gBS->GetMemoryMap, 5, &MemoryMapSize, bi->MemoryMap, &MapKey, &DescriptorSize, &DescriptorVersion);
    
    if (EFI_ERROR(status)) {
        return;
    }
    
    status = uefi_call_wrapper(gBS->ExitBootServices, 2, ImageHandle, MapKey);
    
    if (EFI_ERROR(status)) {
        MemoryMapSize = 0;
        uefi_call_wrapper(gBS->GetMemoryMap, 5, &MemoryMapSize, NULL, &MapKey, &DescriptorSize, &DescriptorVersion);
        MemoryMapSize += 2 * DescriptorSize;
        
        EFI_MEMORY_DESCRIPTOR *NewMemoryMap;
        status = uefi_call_wrapper(gBS->AllocatePool, 3, EfiLoaderData, MemoryMapSize, (VOID**)&NewMemoryMap);
        
        if (!EFI_ERROR(status)) {
            status = uefi_call_wrapper(gBS->GetMemoryMap, 5, &MemoryMapSize, NewMemoryMap, &MapKey, &DescriptorSize, &DescriptorVersion);
            
            if (!EFI_ERROR(status)) {
                bi->MemoryMap = NewMemoryMap;
                bi->MemoryMapSize = MemoryMapSize;
                bi->MemoryMapDescriptorSize = DescriptorSize;
                
                status = uefi_call_wrapper(gBS->ExitBootServices, 2, ImageHandle, MapKey);
            }
        }
    }
}