#pragma once

#include <efi.h>
#include <efilib.h>
#include "../bootinfo.h"

VOID LoadFATImage(EFI_BLOCK_IO_PROTOCOL *BlockIo, BootInfo *bi, BOOLEAN showProgress);
EFI_STATUS GetBlockIO(EFI_LOADED_IMAGE *LoadedImage, EFI_BLOCK_IO_PROTOCOL **BlockIo);
