/* fs.c - простая файловая система LufiraFS */

#include "fs.h"
#include "string.h"
#include "shell.h"
#include "disk.h"

/* Глобальные переменные */
static superblock_t superblock;
static dir_block_t current_dir;
static file_entry_t* open_files[10];
static uint32_t current_dir_block = 1; /* Блок 1 - корневая директория */
static int fs_initialized = 0; /* Флаг инициализации ФС */

/* Вспомогательные функции */

/* Чтение блока с диска */
static int read_block(uint32_t block_num, void* buffer) {
    return disk_read(block_num, (uint8_t*)buffer, 1);
}

/* Запись блока на диск */
static int write_block(uint32_t block_num, const void* buffer) {
    return disk_write(block_num, (const uint8_t*)buffer, 1);
}

/* Найти свободный блок */
static int find_free_block(void) {
    for (uint32_t i = 2; i < superblock.total_blocks; i++) {
        uint32_t byte = i / 8;
        uint32_t bit = i % 8;
        
        if (!(superblock.block_bitmap[byte] & (1 << bit))) {
            superblock.block_bitmap[byte] |= (1 << bit);
            superblock.free_blocks--;
            return i;
        }
    }
    return -1; /* Нет свободных блоков */
}

/* Освободить блок */
static void free_block(uint32_t block_num) {
    if (block_num < superblock.total_blocks) {
        uint32_t byte = block_num / 8;
        uint32_t bit = block_num % 8;
        
        superblock.block_bitmap[byte] &= ~(1 << bit);
        superblock.free_blocks++;
    }
}

/* Найти файл в текущей директории */
static file_entry_t* find_file(const char* filename) {
    for (int i = 0; i < 16; i++) {
        if (current_dir.entries[i].is_used && 
            strcmp(current_dir.entries[i].name, filename) == 0) {
            return &current_dir.entries[i];
        }
    }
    return NULL;
}

/* Найти свободную запись в каталоге */
static file_entry_t* find_free_entry(void) {
    for (int i = 0; i < 16; i++) {
        if (!current_dir.entries[i].is_used) {
            return &current_dir.entries[i];
        }
    }
    return NULL;
}

/* Основные функции ФС */

/* Инициализация ФС */
void fs_init(void) {
    /* Сначала пробуем прочитать суперблок */
    if (read_block(0, &superblock) != 0) {
        terminal_writestring("FS: Cannot read superblock - disk may be empty\n");
        fs_initialized = 0;
        return;
    }
    
    /* Проверяем сигнатуру */
    if (strcmp(superblock.signature, "LUFIRAFS") != 0) {
        terminal_writestring("FS: No valid filesystem found. Use 'format' command\n");
        fs_initialized = 0;
        return;
    }
    
    /* Читаем корневую директорию */
    if (read_block(current_dir_block, &current_dir) != 0) {
        terminal_writestring("FS: Cannot read root directory\n");
        fs_initialized = 0;
        return;
    }
    
    /* Инициализируем таблицу открытых файлов */
    for (int i = 0; i < 10; i++) {
        open_files[i] = NULL;
    }
    
    fs_initialized = 1;
    terminal_writestring("FS: Initialized successfully\n");
    
    /* Выводим информацию о ФС */
    char buf[32];
    terminal_writestring("FS: Total blocks: ");
    itoa(superblock.total_blocks, buf, 10);
    terminal_writestring(buf);
    terminal_writestring(", Free blocks: ");
    itoa(superblock.free_blocks, buf, 10);
    terminal_writestring(buf);
    terminal_writestring("\n");
}

/* Проверить, инициализирована ли ФС */
int fs_is_initialized(void) {
    return fs_initialized;
}

/* Форматирование диска */
int fs_format(void) {
    terminal_writestring("Formatting filesystem...\n");
    
    /* Инициализируем суперблок */
    strcpy(superblock.signature, "LUFIRAFS");
    superblock.version = 1;
    superblock.total_blocks = 2880; /* Для 1.44MB дискеты: 2880 секторов */
    superblock.free_blocks = 2878;  /* Минус суперблок (0) и корневая директория (1) */
    superblock.root_dir = 1;
    
    /* Очищаем битовую карту */
    memset(superblock.block_bitmap, 0, sizeof(superblock.block_bitmap));
    
    /* Помечаем блок 0 и 1 как использованные */
    superblock.block_bitmap[0] = 0x03; /* Бит 0 и 1 установлены (блоки 0 и 1) */
    
    /* Записываем суперблок на диск */
    if (write_block(0, &superblock) != 0) {
        terminal_writestring("FS: Cannot write superblock\n");
        return -1;
    }
    
    /* Инициализируем корневую директорию */
    memset(&current_dir, 0, sizeof(dir_block_t));
    current_dir.next_block = 0; /* Нет следующего блока */
    
    /* Помечаем запись для текущей директории */
    strcpy(current_dir.entries[0].name, ".");
    current_dir.entries[0].is_used = 1;
    current_dir.entries[0].is_directory = 1;
    current_dir.entries[0].first_block = current_dir_block;
    
    /* Помечаем запись для родительской директории */
    strcpy(current_dir.entries[1].name, "..");
    current_dir.entries[1].is_used = 1;
    current_dir.entries[1].is_directory = 1;
    current_dir.entries[1].first_block = current_dir_block;
    
    /* Записываем корневую директорию на диск */
    if (write_block(current_dir_block, &current_dir) != 0) {
        terminal_writestring("FS: Cannot write root directory\n");
        return -1;
    }
    
    /* Обновляем флаг инициализации */
    fs_initialized = 1;
    
    terminal_writestring("FS: Format completed successfully\n");
    
    /* Выводим информацию */
    char buf[32];
    terminal_writestring("FS: Total blocks: ");
    itoa(superblock.total_blocks, buf, 10);
    terminal_writestring(buf);
    terminal_writestring(", Free blocks: ");
    itoa(superblock.free_blocks, buf, 10);
    terminal_writestring(buf);
    terminal_writestring("\n");
    
    return 0;
}

/* Создать файл */
int fs_create(const char* filename, uint8_t is_directory) {
    /* Проверяем, инициализирована ли ФС */
    if (!fs_initialized) {
        terminal_writestring("FS: Filesystem not initialized. Use 'format' first.\n");
        return -1;
    }
    
    /* Проверяем длину имени */
    if (strlen(filename) >= MAX_FILENAME) {
        terminal_writestring("FS: Filename too long\n");
        return -1;
    }
    
    /* Проверяем, существует ли файл */
    if (find_file(filename) != NULL) {
        terminal_writestring("FS: File already exists\n");
        return -1;
    }
    
    /* Находим свободную запись */
    file_entry_t* entry = find_free_entry();
    if (entry == NULL) {
        terminal_writestring("FS: Directory full\n");
        return -1;
    }
    
    /* Находим свободный блок для данных */
    int first_block = find_free_block();
    if (first_block < 0) {
        terminal_writestring("FS: No free space\n");
        
        /* Выводим отладочную информацию */
        char buf[32];
        terminal_writestring("Debug: total_blocks=");
        itoa(superblock.total_blocks, buf, 10);
        terminal_writestring(buf);
        terminal_writestring(", free_blocks=");
        itoa(superblock.free_blocks, buf, 10);
        terminal_writestring(buf);
        terminal_writestring("\n");
        
        return -1;
    }
    
    /* Заполняем запись */
    strcpy(entry->name, filename);
    entry->size = 0;
    entry->first_block = first_block;
    entry->block_count = 1;
    entry->is_used = 1;
    entry->is_directory = is_directory;
    entry->permissions = 0xFF; /* Все права */
    
    /* Обновляем директорию на диске */
    if (write_block(current_dir_block, &current_dir) != 0) {
        terminal_writestring("FS: Cannot update directory\n");
        return -1;
    }
    
    /* Обновляем суперблок на диске */
    if (write_block(0, &superblock) != 0) {
        terminal_writestring("FS: Cannot update superblock\n");
        return -1;
    }
    
    terminal_writestring("FS: File '");
    terminal_writestring(filename);
    terminal_writestring("' created successfully\n");
    
    /* Выводим информацию о созданном файле */
    char buf[32];
    terminal_writestring("File allocated block: ");
    itoa(first_block, buf, 10);
    terminal_writestring(buf);
    terminal_writestring("\n");
    
    return 0;
}

/* Остальные функции оставить как есть, но добавить проверку fs_initialized в начале каждой */

/* Удалить файл */
int fs_delete(const char* filename) {
    if (!fs_initialized) {
        terminal_writestring("FS: Filesystem not initialized. Use 'format' first.\n");
        return -1;
    }
    
    file_entry_t* file = find_file(filename);
    
    if (file == NULL) {
        terminal_writestring("FS: File not found\n");
        return -1;
    }
    
    /* Освобождаем блоки файла */
    for (uint32_t i = 0; i < file->block_count; i++) {
        free_block(file->first_block + i);
    }
    
    /* Очищаем запись */
    memset(file, 0, sizeof(file_entry_t));
    
    /* Обновляем директорию на диске */
    if (write_block(current_dir_block, &current_dir) != 0) {
        terminal_writestring("FS: Cannot update directory\n");
        return -1;
    }
    
    /* Обновляем суперблок на диске */
    if (write_block(0, &superblock) != 0) {
        terminal_writestring("FS: Cannot update superblock\n");
        return -1;
    }
    
    terminal_writestring("FS: File '");
    terminal_writestring(filename);
    terminal_writestring("' deleted\n");
    
    return 0;
}

/* Открыть файл */
file_entry_t* fs_open(const char* filename) {
    if (!fs_initialized) {
        terminal_writestring("FS: Filesystem not initialized\n");
        return NULL;
    }
    
    file_entry_t* file = find_file(filename);
    
    if (file == NULL) {
        terminal_writestring("FS: File not found\n");
        return NULL;
    }
    
    /* Находим свободный слот для открытого файла */
    for (int i = 0; i < 10; i++) {
        if (open_files[i] == NULL) {
            open_files[i] = file;
            return file;
        }
    }
    
    terminal_writestring("FS: Too many open files\n");
    return NULL;
}

/* Закрыть файл */
void fs_close(file_entry_t* file) {
    for (int i = 0; i < 10; i++) {
        if (open_files[i] == file) {
            open_files[i] = NULL;
            break;
        }
    }
}

/* Чтение из файла */
size_t fs_read(file_entry_t* file, void* buffer, size_t size, size_t offset) {
    if (!fs_initialized || file == NULL || buffer == NULL) {
        return 0;
    }
    
    /* Проверяем границы */
    if (offset >= file->size) {
        return 0;
    }
    
    /* Корректируем размер для чтения */
    if (offset + size > file->size) {
        size = file->size - offset;
    }
    
    /* Читаем данные */
    size_t bytes_read = 0;
    uint8_t* dest = (uint8_t*)buffer;
    
    /* Вычисляем начальный блок и смещение */
    uint32_t start_block = file->first_block + (offset / BLOCK_SIZE);
    uint32_t block_offset = offset % BLOCK_SIZE;
    
    /* Читаем блоки */
    while (bytes_read < size) {
        data_block_t block;
        
        if (read_block(start_block, &block) != 0) {
            break;
        }
        
        /* Копируем данные из блока */
        size_t to_copy = BLOCK_SIZE - block_offset;
        if (to_copy > size - bytes_read) {
            to_copy = size - bytes_read;
        }
        
        memcpy(dest + bytes_read, block.data + block_offset, to_copy);
        bytes_read += to_copy;
        
        /* Переходим к следующему блоку */
        start_block++;
        block_offset = 0;
    }
    
    return bytes_read;
}

/* Запись в файл */
size_t fs_write(file_entry_t* file, const void* buffer, size_t size, size_t offset) {
    if (!fs_initialized || file == NULL || buffer == NULL) {
        return 0;
    }
    
    /* Проверяем, не превышает ли запись максимальный размер */
    if (offset + size > MAX_FILE_SIZE) {
        size = MAX_FILE_SIZE - offset;
    }
    
    /* Проверяем, нужно ли выделять дополнительные блоки */
    uint32_t required_blocks = (offset + size + BLOCK_SIZE - 1) / BLOCK_SIZE;
    
    if (required_blocks > file->block_count) {
        /* Нужно выделить дополнительные блоки */
        uint32_t blocks_needed = required_blocks - file->block_count;
        
        for (uint32_t i = 0; i < blocks_needed; i++) {
            int new_block = find_free_block();
            if (new_block < 0) {
                /* Не удалось выделить блоки */
                break;
            }
            file->block_count++;
        }
    }
    
    /* Записываем данные */
    size_t bytes_written = 0;
    const uint8_t* src = (const uint8_t*)buffer;
    
    /* Вычисляем начальный блок и смещение */
    uint32_t start_block = file->first_block + (offset / BLOCK_SIZE);
    uint32_t block_offset = offset % BLOCK_SIZE;
    
    /* Записываем блоки */
    while (bytes_written < size) {
        data_block_t block;
        
        /* Читаем существующий блок, если он есть */
        read_block(start_block, &block);
        
        /* Копируем данные в блок */
        size_t to_write = BLOCK_SIZE - block_offset;
        if (to_write > size - bytes_written) {
            to_write = size - bytes_written;
        }
        
        memcpy(block.data + block_offset, src + bytes_written, to_write);
        bytes_written += to_write;
        
        /* Записываем блок обратно */
        if (write_block(start_block, &block) != 0) {
            break;
        }
        
        /* Переходим к следующему блоку */
        start_block++;
        block_offset = 0;
    }
    
    /* Обновляем размер файла */
    if (offset + bytes_written > file->size) {
        file->size = offset + bytes_written;
    }
    
    return bytes_written;
}

/* Список файлов в текущей директории */
void fs_list(void) {
    if (!fs_initialized) {
        terminal_writestring("FS: Filesystem not initialized. Use 'format' first.\n");
        return;
    }
    
    terminal_writestring("Directory listing:\n");
    terminal_writestring("------------------\n");
    
    int file_count = 0;
    for (int i = 0; i < 16; i++) {
        if (current_dir.entries[i].is_used) {
            file_count++;
            
            terminal_writestring("  ");
            
            if (current_dir.entries[i].is_directory) {
                terminal_writestring("[DIR]  ");
            } else {
                terminal_writestring("[FILE] ");
            }
            
            terminal_writestring(current_dir.entries[i].name);
            
            /* Выравнивание */
            int spaces = 30 - strlen(current_dir.entries[i].name);
            for (int j = 0; j < spaces; j++) {
                terminal_putchar(' ');
            }
            
            /* Размер */
            char size_str[16];
            itoa(current_dir.entries[i].size, size_str, 10);
            terminal_writestring(size_str);
            terminal_writestring(" bytes\n");
        }
    }
    
    if (file_count == 0) {
        terminal_writestring("  (empty directory)\n");
    }
    
    /* Выводим свободное пространство */
    char free_str[32];
    terminal_writestring("\nFree space: ");
    itoa(fs_free_space(), free_str, 10);
    terminal_writestring(free_str);
    terminal_writestring(" bytes\n");
}

/* Получить информацию о файле */
int fs_stat(const char* filename, file_entry_t* info) {
    if (!fs_initialized) {
        terminal_writestring("FS: Filesystem not initialized\n");
        return -1;
    }
    
    file_entry_t* file = find_file(filename);
    
    if (file == NULL) {
        return -1;
    }
    
    if (info != NULL) {
        memcpy(info, file, sizeof(file_entry_t));
    }
    
    return 0;
}

/* Получить свободное пространство */
uint32_t fs_free_space(void) {
    if (!fs_initialized) {
        return 0;
    }
    return superblock.free_blocks * BLOCK_SIZE;
}