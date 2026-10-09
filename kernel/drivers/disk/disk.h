#pragma once

#include "lib/types.h"

// Пробует поднять AHCI (ahci.c) — на успехе disk_read_sectors()/
// disk_write_sectors() ниже прозрачно идут через него; иначе остаются на
// легаси ATA PIO (как раньше, без вызова disk_init() вообще). Вызывать
// после pci_init() — см. kernel.c.
void disk_init(void);

int disk_read_sectors(uint32_t lba, uint8_t sector_count, void *buffer);
int disk_write_sectors(uint32_t lba, uint8_t sector_count, const void *buffer);
