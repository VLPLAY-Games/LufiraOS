/* fs.h - простая файловая система LufiraFS */

#ifndef FS_H
#define FS_H

#include <stdint.h>
#include <stddef.h>

/* Размеры и ограничения */
#define MAX_FILES 64
#define MAX_FILENAME 32
#define BLOCK_SIZE 512
#define MAX_BLOCKS_PER_FILE 8
#define MAX_FILE_SIZE (BLOCK_SIZE * MAX_BLOCKS_PER_FILE)
#define BITMAP_SIZE 360 /* Для 2880 блоков: 2880/8 = 360 байт */

/* Структура записи в каталоге */
typedef struct {
    char name[MAX_FILENAME];
    uint32_t size;           /* Размер файла в байтах */
    uint32_t first_block;    /* Первый блок данных */
    uint32_t block_count;    /* Количество блоков */
    uint8_t is_used;         /* 1 = используется, 0 = свободно */
    uint8_t is_directory;    /* 1 = директория, 0 = файл */
    uint8_t permissions;     /* Права доступа (пока не используется) */
    uint32_t created_time;   /* Время создания (пока не используется) */
} file_entry_t;

/* Структура суперблока */
typedef struct {
    char signature[8];       /* "LUFIRAFS" */
    uint32_t version;        /* Версия ФС */
    uint32_t total_blocks;   /* Всего блоков */
    uint32_t free_blocks;    /* Свободных блоков */
    uint32_t root_dir;       /* Номер блока корневой директории */
    uint8_t  block_bitmap[BITMAP_SIZE]; /* Битовая карта блоков */
} superblock_t;

/* Структура блока данных */
typedef struct {
    uint8_t data[BLOCK_SIZE];
} data_block_t;

/* Структура блока каталога */
typedef struct {
    file_entry_t entries[16]; /* 16 записей в одном блоке каталога */
    uint32_t next_block;      /* Следующий блок каталога (для больших каталогов) */
} dir_block_t;

/* Функции файловой системы */

/* Инициализация ФС */
void fs_init(void);

/* Проверить, инициализирована ли ФС */
int fs_is_initialized(void);

/* Форматирование диска */
int fs_format(void);

/* Создать файл или директорию */
int fs_create(const char* filename, uint8_t is_directory);

/* Создать директорию */
int fs_mkdir(const char* dirname);

/* Сменить директорию */
int fs_cd(const char* path);

/* Получить текущий путь */
const char* fs_get_current_path(void);

/* Удалить файл */
int fs_delete(const char* filename);

/* Открыть файл */
file_entry_t* fs_open(const char* filename);

/* Закрыть файл */
void fs_close(file_entry_t* file);

/* Чтение из файла */
size_t fs_read(file_entry_t* file, void* buffer, size_t size, size_t offset);

/* Запись в файл */
size_t fs_write(file_entry_t* file, const void* buffer, size_t size, size_t offset);

/* Список файлов в текущей директории */
void fs_list(void);

/* Получить информацию о файле */
int fs_stat(const char* filename, file_entry_t* info);

/* Получить свободное пространство */
uint32_t fs_free_space(void);

#endif /* FS_H */