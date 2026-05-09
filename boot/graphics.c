#include "graphics.h"

EFI_STATUS InitializeGraphics(BootInfo *bi) {
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;
    EFI_GUID gopGuid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_STATUS status = uefi_call_wrapper(gBS->LocateProtocol, 3, &gopGuid, NULL, (VOID**)&gop);
    
    if (!EFI_ERROR(status)) {
        bi->FrameBufferBase = gop->Mode->FrameBufferBase;
        bi->FrameBufferSize = gop->Mode->FrameBufferSize;
        bi->HorizontalResolution = gop->Mode->Info->HorizontalResolution;
        bi->VerticalResolution = gop->Mode->Info->VerticalResolution;
        bi->PixelsPerScanLine = gop->Mode->Info->PixelsPerScanLine;
        bi->PixelFormat = (gop->Mode->Info->PixelFormat == PixelBlueGreenRedReserved8BitPerColor) ? 1 : 0;
    }
    
    return status;
}