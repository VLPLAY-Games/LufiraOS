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
static char current_path[256] = "/"; /* Текущий путь */

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

/* Загрузить директорию по номеру блока */
static int load_dir(uint32_t block_num) {
    if (read_block(block_num, &current_dir) != 0) {
        return -1;
    }
    current_dir_block = block_num;
    return 0;
}

/* Разбор пути на компоненты */
static int parse_path(const char* path, char components[][MAX_FILENAME], int* count) {
    char temp[256];
    strcpy(temp, path);
    
    *count = 0;
    char* token = strtok(temp, "/");
    
    while (token != NULL && *count < 10) {
        strcpy(components[*count], token);
        (*count)++;
        token = strtok(NULL, "/");
    }
    
    return 0;
}

/* Получить текущий путь */
const char* fs_get_current_path(void) {
    return current_path;
}

/* Основные функции ФС */

/* Инициализация ФС */
void fs_init(void) {
    /* Если уже инициализирована, просто возвращаемся */
    if (fs_initialized) {
        return;
    }
    
    /* Сначала пробуем прочитать суперблок */
    if (read_block(0, &superblock) != 0) {
        terminal_writestring("FS: Cannot read superblock - disk may be empty\n");
        fs_initialized = 0;
        return;
    }
    
    /* Проверяем сигнатуру */
    if (strcmp(superblock.signature, "LUFIRAFS") != 0) {
        terminal_writestring("FS: No valid filesystem found\n");
        fs_initialized = 0;
        return;
    }
    
    /* Проверяем версию */
    if (superblock.version != 1) {
        terminal_writestring("FS: Unsupported filesystem version\n");
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
    
    /* Сбрасываем путь на корень */
    strcpy(current_path, "/");
    
    fs_initialized = 1;
    
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
    
    /* Сбрасываем путь */
    strcpy(current_path, "/");
    
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

/* Создать файл или директорию */
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
    
    /* Если это директория, инициализируем её содержимое */
    if (is_directory) {
        dir_block_t new_dir;
        memset(&new_dir, 0, sizeof(dir_block_t));
        new_dir.next_block = 0;
        
        /* Запись для текущей директории */
        strcpy(new_dir.entries[0].name, ".");
        new_dir.entries[0].is_used = 1;
        new_dir.entries[0].is_directory = 1;
        new_dir.entries[0].first_block = first_block;
        
        /* Запись для родительской директории */
        strcpy(new_dir.entries[1].name, "..");
        new_dir.entries[1].is_used = 1;
        new_dir.entries[1].is_directory = 1;
        new_dir.entries[1].first_block = current_dir_block;
        
        /* Записываем новую директорию на диск */
        if (write_block(first_block, &new_dir) != 0) {
            terminal_writestring("FS: Cannot write directory contents\n");
            return -1;
        }
    }
    
    /* Обновляем текущую директорию на диске */
    if (write_block(current_dir_block, &current_dir) != 0) {
        terminal_writestring("FS: Cannot update directory\n");
        return -1;
    }
    
    /* Обновляем суперблок на диске */
    if (write_block(0, &superblock) != 0) {
        terminal_writestring("FS: Cannot update superblock\n");
        return -1;
    }
    
    terminal_writestring("FS: '");
    terminal_writestring(filename);
    terminal_writestring("' created successfully\n");
    
    return 0;
}

/* Создать директорию */
int fs_mkdir(const char* dirname) {
    return fs_create(dirname, 1);
}

/* Сменить директорию */
int fs_cd(const char* path) {
    if (!fs_initialized) {
        terminal_writestring("FS: Filesystem not initialized\n");
        return -1;
    }
    
    char local_path[256];
    strcpy(local_path, path);
    
    /* Обработка специальных случаев */
    if (strcmp(local_path, ".") == 0) {
        return 0; /* Остаёмся в текущей директории */
    }
    
    if (strcmp(local_path, "..") == 0) {
        /* Переходим в родительскую директорию */
        file_entry_t* parent = find_file("..");
        if (parent == NULL || !parent->is_directory) {
            terminal_writestring("FS: Cannot go to parent directory\n");
            return -1;
        }
        
        /* Загружаем родительскую директорию */
        if (load_dir(parent->first_block) != 0) {
            terminal_writestring("FS: Cannot load parent directory\n");
            return -1;
        }
        
        /* Обновляем путь */
        if (strcmp(current_path, "/") != 0) {
            /* Удаляем последний компонент пути */
            char* last_slash = strrchr(current_path, '/');
            if (last_slash != NULL) {
                if (last_slash == current_path) {
                    /* Мы в корне? */
                    current_path[1] = '\0';
                } else {
                    *last_slash = '\0';
                }
            }
        }
        return 0;
    }
    
    if (strcmp(local_path, "/") == 0) {
        /* Переходим в корень */
        if (load_dir(superblock.root_dir) != 0) {
            terminal_writestring("FS: Cannot load root directory\n");
            return -1;
        }
        strcpy(current_path, "/");
        return 0;
    }
    
    /* Ищем директорию в текущей директории */
    file_entry_t* dir = find_file(local_path);
    if (dir == NULL) {
        terminal_writestring("FS: Directory not found: ");
        terminal_writestring(local_path);
        terminal_writestring("\n");
        return -1;
    }
    
    if (!dir->is_directory) {
        terminal_writestring("FS: Not a directory: ");
        terminal_writestring(local_path);
        terminal_writestring("\n");
        return -1;
    }
    
    /* Загружаем новую директорию */
    if (load_dir(dir->first_block) != 0) {
        terminal_writestring("FS: Cannot load directory\n");
        return -1;
    }
    
    /* Обновляем путь */
    if (strcmp(current_path, "/") != 0) {
        strcat(current_path, "/");
    }
    if (strcmp(current_path, "/") == 0 && local_path[0] != '/') {
        /* Добавляем к корню */
        strcat(current_path, local_path);
    } else if (strcmp(current_path, "/") != 0) {
        strcat(current_path, local_path);
    }
    
    return 0;
}

/* Удалить файл или директорию */
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
    
    /* Если это директория, проверяем, не пуста ли она */
    if (file->is_directory) {
        /* Загружаем директорию для проверки */
        dir_block_t dir_check;
        if (read_block(file->first_block, &dir_check) == 0) {
            int file_count = 0;
            for (int i = 2; i < 16; i++) { /* Пропускаем . и .. */
                if (dir_check.entries[i].is_used) {
                    file_count++;
                    break;
                }
            }
            
            if (file_count > 0) {
                terminal_writestring("FS: Directory not empty\n");
                return -1;
            }
        }
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
    
    terminal_writestring("FS: '");
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
    
    if (file->is_directory) {
        terminal_writestring("FS: Cannot open directory as file\n");
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
    
    terminal_writestring("Directory: ");
    terminal_writestring(current_path);
    terminal_writestring("\n");
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