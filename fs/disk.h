/* disk.h - функции для работы с диском */

#ifndef DISK_H
#define DISK_H

#include <stdint.h>

/* Порт диска */
#define DISK_PORT 0x1F0

/* Команды диска */
#define DISK_READ_SECTORS 0x20
#define DISK_WRITE_SECTORS 0x30

/* Функции */

/* Инициализация диска */
void disk_init(void);

/* Чтение секторов */
int disk_read(uint32_t lba, uint8_t* buffer, uint32_t sector_count);

/* Запись секторов */
int disk_write(uint32_t lba, const uint8_t* buffer, uint32_t sector_count);

#endif /* DISK_H */