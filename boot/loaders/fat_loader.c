#include "fat_loader.h"
#include "ui/utils.h"

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
        PrintColored(L"Loading FAT image... ", COLOR_NEON_CYAN, COLOR_BLACK);
    }
    
    UINTN Pages = (UINTN)((CopySize + 4095) / 4096);
    EFI_PHYSICAL_ADDRESS FATBase = 0;
    EFI_STATUS status = uefi_call_wrapper(gBS->AllocatePages, 4, AllocateAnyPages, EfiLoaderData, Pages, &FATBase);
    
    if (!EFI_ERROR(status)) {
        UINT8* Buffer = (UINT8*)FATBase;
        UINTN MaxBlocksPerTransfer = 1024;
        UINTN TotalBlocks = (UINTN)((CopySize + BlockSize - 1) / BlockSize);
        
        for (UINTN i = 0; i < TotalBlocks; i += MaxBlocksPerTransfer) {
            UINTN BlocksNow = (TotalBlocks - i) > MaxBlocksPerTransfer ? MaxBlocksPerTransfer : (TotalBlocks - i);
            status = uefi_call_wrapper(BlockIo->ReadBlocks, 5, BlockIo, BlockIo->Media->MediaId, i,
                                       BlocksNow * BlockSize, Buffer + ((UINT64)i * BlockSize));
            if (EFI_ERROR(status)) break;
            if (showProgress) {
                Print(L".");
            }
            uefi_call_wrapper(gBS->Stall, 1, 5000);
        }
        
        if (!EFI_ERROR(status)) {
            bi->FATImageBase = FATBase;
            bi->FATImageSize = CopySize;
            if (showProgress) PrintColored(L"OK\n", COLOR_NEON_GREEN, COLOR_BLACK);
        } else {
            bi->FATImageBase = bi->FATImageSize = 0;
            if (showProgress) PrintColored(L"FAILED\n", COLOR_RED, COLOR_BLACK);
        }
    } else {
        if (showProgress) PrintColored(L"ALLOC FAILED\n", COLOR_RED, COLOR_BLACK);
    }
}

EFI_STATUS GetBlockIO(EFI_LOADED_IMAGE *LoadedImage, EFI_BLOCK_IO_PROTOCOL **BlockIo) {
    EFI_GUID BlockIoGuid = EFI_BLOCK_IO_PROTOCOL_GUID;
    EFI_STATUS status = uefi_call_wrapper(gBS->HandleProtocol, 3, LoadedImage->DeviceHandle, &BlockIoGuid, (VOID**)BlockIo);
    
    if (EFI_ERROR(status)) {
        EFI_DEVICE_PATH_PROTOCOL *DevicePath;
        EFI_GUID DevicePathGuid = EFI_DEVICE_PATH_PROTOCOL_GUID;
        status = uefi_call_wrapper(gBS->HandleProtocol, 3, LoadedImage->DeviceHandle, &DevicePathGuid, (VOID**)&DevicePath);
        
        if (!EFI_ERROR(status)) {
            EFI_HANDLE blockHandle;
            status = uefi_call_wrapper(gBS->LocateDevicePath, 3, &BlockIoGuid, &DevicePath, &blockHandle);
            if (!EFI_ERROR(status))
                uefi_call_wrapper(gBS->HandleProtocol, 3, blockHandle, &BlockIoGuid, (VOID**)BlockIo);
        }
    }
    
    return status;
}