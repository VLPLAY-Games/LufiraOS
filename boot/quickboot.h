#ifndef QUICKBOOT_H
#define QUICKBOOT_H

#include <efi.h>
#include <efilib.h>
#include "bootinfo.h"
#include "splash.h"

VOID QuickBoot(BootInfo *bi, EFI_HANDLE ImageHandle, BOOLEAN keepLogo, BOOLEAN animateSpinner);

#endif