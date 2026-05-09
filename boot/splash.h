#ifndef SPLASH_H
#define SPLASH_H

#include <efi.h>
#include <efilib.h>

typedef enum { MODE_NORMAL, MODE_DEBUG, MODE_SAFE } BootMode;

VOID ShowSplash(BootMode mode);

#endif