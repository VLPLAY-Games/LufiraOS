/* disk.c - функции для работы с диском */

#include "disk.h"
#include "shell.h"

/* Чтение из порта */
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/* Запись в порт */
static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

/* Ожидание готовности диска */
static void disk_wait(void) {
    while (inb(DISK_PORT + 7) & 0x80) {
        /* busy */
    }
}

/* Инициализация диска */
void disk_init(void) {
    terminal_writestring("Disk: Initializing...\n");
    
    /* Проверяем наличие диска */
    disk_wait();
    outb(DISK_PORT + 6, 0xE0); /* Выбираем мастер диск */
    
    /* Сброс диска */
    outb(DISK_PORT + 7, 0x08);
    disk_wait();
    
    terminal_writestring("Disk: Ready\n");
}

/* Чтение секторов */
int disk_read(uint32_t lba, uint8_t* buffer, uint32_t sector_count) {
    disk_wait();
    
    /* Устанавливаем количество секторов */
    outb(DISK_PORT + 2, (uint8_t)sector_count);
    
    /* Устанавливаем LBA */
    outb(DISK_PORT + 3, (uint8_t)lba);
    outb(DISK_PORT + 4, (uint8_t)(lba >> 8));
    outb(DISK_PORT + 5, (uint8_t)(lba >> 16));
    outb(DISK_PORT + 6, 0xE0 | ((lba >> 24) & 0x0F));
    
    /* Отправляем команду чтения */
    outb(DISK_PORT + 7, DISK_READ_SECTORS);
    
    /* Читаем данные */
    for (uint32_t i = 0; i < sector_count; i++) {
        disk_wait();
        
        /* Читаем 256 слов (512 байт) */
        for (int j = 0; j < 256; j++) {
            uint16_t data = inb(DISK_PORT);
            data |= (inb(DISK_PORT) << 8);
            
            /* Сохраняем в буфер */
            buffer[(i * 512) + (j * 2)] = data & 0xFF;
            buffer[(i * 512) + (j * 2) + 1] = (data >> 8) & 0xFF;
        }
    }
    
    return 0;
}

/* Запись секторов */
int disk_write(uint32_t lba, const uint8_t* buffer, uint32_t sector_count) {
    disk_wait();
    
    /* Устанавливаем количество секторов */
    outb(DISK_PORT + 2, (uint8_t)sector_count);
    
    /* Устанавливаем LBA */
    outb(DISK_PORT + 3, (uint8_t)lba);
    outb(DISK_PORT + 4, (uint8_t)(lba >> 8));
    outb(DISK_PORT + 5, (uint8_t)(lba >> 16));
    outb(DISK_PORT + 6, 0xE0 | ((lba >> 24) & 0x0F));
    
    /* Отправляем команду записи */
    outb(DISK_PORT + 7, DISK_WRITE_SECTORS);
    
    /* Записываем данные */
    for (uint32_t i = 0; i < sector_count; i++) {
        disk_wait();
        
        /* Записываем 256 слов (512 байт) */
        for (int j = 0; j < 256; j++) {
            uint16_t data = buffer[(i * 512) + (j * 2)] | 
                           (buffer[(i * 512) + (j * 2) + 1] << 8);
            
            outb(DISK_PORT, data & 0xFF);
            outb(DISK_PORT, (data >> 8) & 0xFF);
        }
    }
    
    /* Ожидаем завершения записи */
    disk_wait();
    
    return 0;
}