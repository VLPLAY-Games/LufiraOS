#pragma once

#include <efi.h>
#include <efilib.h>
#include "bootinfo.h"
#include "ui/splash.h"

VOID QuickBoot(BootInfo *bi, EFI_HANDLE ImageHandle, BOOLEAN keepLogo, BOOLEAN animateSpinner);
