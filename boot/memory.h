#ifndef MEMORY_H
#define MEMORY_H

#include <efi.h>
#include <efilib.h>
#include "bootinfo.h"

VOID ReadMemoryMap(BootInfo *bi);
UINT64 CalculateTotalRAM(EFI_MEMORY_DESCRIPTOR *MemoryMap, UINTN MemoryMapSize, UINTN DescriptorSize);

#endif