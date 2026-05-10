#pragma once

#include <efi.h>
#include <efilib.h>
#include "../bootinfo.h"

VOID ExitBootServicesWrapper(BootInfo *bi, EFI_HANDLE ImageHandle);
