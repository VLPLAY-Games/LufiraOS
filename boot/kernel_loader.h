#ifndef KERNEL_H
#define KERNEL_H

#include <efi.h>
#include <efilib.h>
#include "bootinfo.h"

EFI_STATUS LoadKernel(BootInfo *bi, EFI_HANDLE ImageHandle, BOOLEAN animateSpinner, 
                      UINTN *spinIdx, UINTN statusRow, UINTN spinnerRow, UINTN cols);

#endif