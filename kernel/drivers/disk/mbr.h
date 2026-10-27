#pragma once

#include "lib/types.h"

// v1.0.0 план — переход с "суперфлоппи" на настоящую MBR-таблицу разделов
// (см. LufiraOS-Builder/lufira_builder/image.py:_write_mbr()/config.py).
// Значения ОБЯЗАНЫ совпадать с MBR_PARTITION_TYPE_ESP/MBR_PARTITION_TYPE_LUFIRAFS
// там же.
#define MBR_PARTITION_TYPE_ESP      0xEFu
#define MBR_PARTITION_TYPE_LUFIRAFS 0xDAu

// Ищет ПЕРВУЮ партицию с типом type в MBR (LBA0, через disk_read_sectors() —
// вызывать после disk_init()). out_start_lba получает её начальный LBA.
// 0 — успех, -1 — партиция не найдена, LBA0 нечитаем или нет сигнатуры 0x55AA.
int mbr_find_partition(uint8_t type, uint32_t *out_start_lba);
