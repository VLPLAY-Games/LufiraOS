#include "memory.h"

VOID ReadMemoryMap(BootInfo *bi) {
    UINTN MemoryMapSize = 0, MapKey, DescriptorSize;
    UINT32 DescriptorVersion;
    
    uefi_call_wrapper(gBS->GetMemoryMap, 5, &MemoryMapSize, NULL, &MapKey, &DescriptorSize, &DescriptorVersion);
    MemoryMapSize += 2 * DescriptorSize;
    
    EFI_MEMORY_DESCRIPTOR *MemoryMap;
    uefi_call_wrapper(gBS->AllocatePool, 3, EfiLoaderData, MemoryMapSize, (VOID**)&MemoryMap);
    uefi_call_wrapper(gBS->GetMemoryMap, 5, &MemoryMapSize, MemoryMap, &MapKey, &DescriptorSize, &DescriptorVersion);
    
    bi->TotalMemory = CalculateTotalRAM(MemoryMap, MemoryMapSize, DescriptorSize);
    bi->MemoryMapSize = MemoryMapSize;
    bi->MemoryMap = MemoryMap;
    bi->MemoryMapDescriptorSize = DescriptorSize;
}

UINT64 CalculateTotalRAM(EFI_MEMORY_DESCRIPTOR *MemoryMap, UINTN MemoryMapSize, UINTN DescriptorSize) {
    uint64_t TotalRAM = 0;
    for (UINTN i = 0; i < (MemoryMapSize / DescriptorSize); i++) {
        EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR*)((UINT8*)MemoryMap + (i * DescriptorSize));
        if (d->Type == EfiConventionalMemory || d->Type == EfiLoaderCode || d->Type == EfiLoaderData)
            TotalRAM += d->NumberOfPages * 4096;
    }
    return TotalRAM;
}