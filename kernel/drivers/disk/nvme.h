#pragma once

#include "lib/types.h"

// NVMe — второй, после AHCI, способ добраться до диска на машинах без
// legacy SATA/IDE/AHCI: многие NVMe-накопители не поддерживают режим
// совместимости с AHCI. Опрос вместо прерываний здесь особенно естественен:
// контроллер сигналит о завершении, просто переключая phase-бит записи
// completion queue в обычной памяти — это штатный механизм, не замена
// прерыванию; парсинг PCI capability list под MSI-X не нужен даже
// формально. Один контроллер, один namespace (NSID=1), один admin queue
// pair (QID 0) + один I/O queue pair (QID 1), глубина каждой — 2 записи
// (не больше одной команды "в полёте" одновременно). PRP — физически
// непрерывный bounce-буфер на 128 КБ, плюс одна заранее заполненная
// PRP-list страница под страницы 2..32 (буфер неподвижен — список не
// обновляется на каждый I/O).
//
// LBA namespace'а должен быть ровно 512 байт без metadata — иначе
// контроллер остаётся недоступен, disk.c откатывается на следующий
// драйвер в цепочке.

int nvme_init(void);
int nvme_available(void);

int nvme_read_sectors(uint32_t lba, uint8_t sector_count, void *buffer);
int nvme_write_sectors(uint32_t lba, uint8_t sector_count, const void *buffer);
