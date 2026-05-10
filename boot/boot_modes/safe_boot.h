#pragma once

#include <efi.h>
#include <efilib.h>
#include "../bootinfo.h"

VOID SafeBoot(BootInfo *bi, EFI_HANDLE ImageHandle);
