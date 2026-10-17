#pragma once

#include "lib/types.h"

// AHCI SATA — заменяет легаси ATA PIO (ata.c/disk.c, порты 0x1F0-0x1F7) там,
// где его нет: многие машины содержат только AHCI/NVMe, без SATA/IDE в
// legacy-режиме. Детали в ahci.c: опрос вместо прерываний, один активный
// порт, LBA48 READ/WRITE DMA EXT через bounce-буфер.

// Ищет AHCI-контроллер в PCI, инициализирует первый порт с подключённым
// SATA-устройством (пропуская ATAPI/пустые порты), готовит command list/
// FIS receive area/bounce-буфер. 0 при успехе, -1 если контроллера или
// подходящего порта нет — тогда disk.c остаётся на легаси ATA PIO.
int ahci_init(void);

// 1, если ahci_init() нашёл и успешно поднял порт.
int ahci_available(void);

int ahci_read_sectors(uint32_t lba, uint8_t sector_count, void *buffer);
int ahci_write_sectors(uint32_t lba, uint8_t sector_count, const void *buffer);
