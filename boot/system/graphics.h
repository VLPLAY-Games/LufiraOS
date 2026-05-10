#pragma once

#include <efi.h>
#include <efilib.h>
#include "../bootinfo.h"

EFI_STATUS InitializeGraphics(BootInfo *bi);
