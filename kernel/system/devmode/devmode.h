#pragma once

#include "lib/types.h"
#include "drivers/console/console.h"

// Режим разработчика: persistent-флаг (файл /system/devmode.flag на
// LufiraFS), включает/выключает подробный диагностический вывод драйверов
// (PCI/AC97/UHCI/ELF/процессы) в консоль. По умолчанию — выключен (флага
// нет на диске), события всё равно попадают в /logs/system.log — см. klog.h.

// Грубая проверка флага прямо по сырому образу диска, без lufirafs_init() —
// можно звать до heap_init() (см. devmode.c). Настоящий devmode_init() ниже
// вызывается после монтирования и является источником истины.
void devmode_probe_early(const void *fs_image, uint32_t fs_size);
void devmode_init(void);          // читать флаг с диска — вызывать сразу после lufirafs_init()
int  devmode_is_enabled(void);
int  devmode_set(int enabled);    // создаёт/удаляет флаг-файл; 0 = успех

// Печатает fmt только если включён режим разработчика.
#define DLOG(...) do { if (devmode_is_enabled()) printf(__VA_ARGS__); } while (0)
