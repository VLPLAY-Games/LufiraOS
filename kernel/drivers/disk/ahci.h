#pragma once

#include "lib/types.h"

// AHCI SATA — v0.9, фаза 1, пункт 3: заменяет легаси ATA PIO (ata.c/disk.c,
// порты 0x1F0-0x1F7, первичная шина IDE) там, где он есть — многие реальные
// машины (в т.ч. референсный ноутбук для 1.0, Chuwi Gemibook Xpro, N100)
// вообще не имеют SATA/IDE-контроллера в legacy-режиме, только AHCI/NVMe.
// См. общий разбор в ahci.c — опрос вместо прерываний (тот же выбор, что
// уже сделан для xHCI/RTL8139/AC'97 в этом ядре), один активный порт,
// LBA48 READ/WRITE DMA EXT через bounce-буфер.

// Ищет AHCI-контроллер в PCI, инициализирует первый порт с реально
// подключённым SATA-устройством (пропуская ATAPI/пустые порты), готовит
// под него command list/FIS receive area/bounce-буфер. 0 при успехе
// (ahci_read_sectors()/ahci_write_sectors() становятся рабочими), -1 если
// контроллера или подходящего порта нет — вызывающий (disk.c) в этом
// случае остаётся на легаси ATA PIO.
int ahci_init(void);

// 1, если ahci_init() нашёл и успешно поднял порт.
int ahci_available(void);

int ahci_read_sectors(uint32_t lba, uint8_t sector_count, void *buffer);
int ahci_write_sectors(uint32_t lba, uint8_t sector_count, const void *buffer);
