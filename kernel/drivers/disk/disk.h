#pragma once

#include "lib/types.h"

// Пробует поднять AHCI (ahci.c), затем NVMe (nvme.c); при неудаче обоих
// disk_read/write_sectors() идут через легаси ATA PIO. Вызывать после
// pci_init().
void disk_init(void);

int disk_read_sectors(uint32_t lba, uint8_t sector_count, void *buffer);
int disk_write_sectors(uint32_t lba, uint8_t sector_count, const void *buffer);
