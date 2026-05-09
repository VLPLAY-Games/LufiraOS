#include "kernel_loader.h"
#include "../ui/utils.h"

EFI_STATUS LoadKernel(BootInfo *bi, EFI_HANDLE ImageHandle, BOOLEAN animateSpinner, 
                      UINTN *spinIdx, UINTN statusRow, UINTN spinnerRow, UINTN cols) {
    CHAR16 spin[] = L"|/-\\";
    
    EFI_LOADED_IMAGE *LoadedImage;
    EFI_STATUS status = uefi_call_wrapper(gBS->HandleProtocol, 3, ImageHandle, &LoadedImageProtocol, (VOID**)&LoadedImage);
    if (EFI_ERROR(status)) return status;
    
    EFI_FILE_IO_INTERFACE *FileSystem;
    status = uefi_call_wrapper(gBS->HandleProtocol, 3, LoadedImage->DeviceHandle, &FileSystemProtocol, (VOID**)&FileSystem);
    if (EFI_ERROR(status)) return status;
    
    EFI_FILE_HANDLE Root;
    uefi_call_wrapper(FileSystem->OpenVolume, 2, FileSystem, &Root);
    
    EFI_FILE_HANDLE KernelFile;
    status = uefi_call_wrapper(Root->Open, 5, Root, &KernelFile, L"kernel.bin", EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(status)) return status;
    
    EFI_FILE_INFO *KernelFileInfo;
    UINTN KernelFileInfoSize = sizeof(EFI_FILE_INFO) + 256;
    uefi_call_wrapper(gBS->AllocatePool, 3, EfiLoaderData, KernelFileInfoSize, (VOID**)&KernelFileInfo);
    uefi_call_wrapper(KernelFile->GetInfo, 4, KernelFile, &gEfiFileInfoGuid, &KernelFileInfoSize, KernelFileInfo);
    bi->KernelSize = KernelFileInfo->FileSize;
    uefi_call_wrapper(gBS->FreePool, 1, KernelFileInfo);
    
    UINTN Pages = (bi->KernelSize + 4095) / 4096;
    EFI_PHYSICAL_ADDRESS KernelBase = 0x100000;
    status = uefi_call_wrapper(gBS->AllocatePages, 4,
        AllocateAddress, EfiLoaderData, Pages, &KernelBase);
    
    if (EFI_ERROR(status)) {
        status = uefi_call_wrapper(gBS->AllocatePages, 4,
            AllocateAnyPages, EfiLoaderData, Pages, &KernelBase);
    }
    
    if (EFI_ERROR(status)) return status;
    
    bi->KernelBase = KernelBase;
    
    UINTN ChunkSize = 65536, TotalLoaded = 0;
    while (TotalLoaded < bi->KernelSize) {
        UINTN ToRead = (bi->KernelSize - TotalLoaded) > ChunkSize ? ChunkSize : (bi->KernelSize - TotalLoaded);
        uefi_call_wrapper(KernelFile->Read, 3, KernelFile, &ToRead, (VOID*)(KernelBase + TotalLoaded));
        TotalLoaded += ToRead;
        if (animateSpinner && ((TotalLoaded / ChunkSize) % 2 == 0)) {
            *spinIdx = (*spinIdx + 1) % 4;
            uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, 0, statusRow);
            SetColor(COLOR_BLACK, COLOR_BLACK);
            for (UINTN _i = 0; _i < cols; _i++) Print(L" ");
            PrintCentered(L"Loading kernel...", statusRow, COLOR_NEON_CYAN);
            uefi_call_wrapper(gST->ConOut->SetCursorPosition, 3, gST->ConOut, cols / 2, spinnerRow);
            SetColor(COLOR_NEON_CYAN, COLOR_BLACK);
            Print(L"%c", spin[*spinIdx]);
        }
        uefi_call_wrapper(gBS->Stall, 1, 1000);
    }
    
    return EFI_SUCCESS;
}