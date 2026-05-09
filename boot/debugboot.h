#ifndef DEBUGBOOT_H
#define DEBUGBOOT_H

#include <efi.h>
#include <efilib.h>
#include "bootinfo.h"

VOID DebugBoot(BootInfo *bi, EFI_HANDLE ImageHandle);

#endif