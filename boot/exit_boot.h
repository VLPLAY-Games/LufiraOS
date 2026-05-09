#ifndef EXITBS_H
#define EXITBS_H

#include <efi.h>
#include <efilib.h>
#include "bootinfo.h"

VOID ExitBootServicesWrapper(BootInfo *bi, EFI_HANDLE ImageHandle);

#endif