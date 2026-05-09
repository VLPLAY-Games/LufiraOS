#ifndef GRAPHICS_H
#define GRAPHICS_H

#include <efi.h>
#include <efilib.h>
#include "../bootinfo.h"

EFI_STATUS InitializeGraphics(BootInfo *bi);

#endif