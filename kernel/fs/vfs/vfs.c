#include "vfs.h"
#include "system/mm/heap.h"
#include "drivers/console/console.h"
#include "lib/stddef.h"

// ========== ВНУТРЕННИЕ СТРУКТУРЫ ==========

// Таблица открытых файлов (глобальная)
static file_t *file_table[MAX_FILES_SYSTEM] = {0};

// Файловые дескрипторы для процесса
typedef struct fd_table {
    file_t *files[MAX_FD_PER_PROCESS];
    int count;
} fd_table_t;

static fd_table_t *current_fd_table = NULL;

// Корневой inode VFS
static inode_t *root_inode = NULL;

// ========== ВСПОМОГАТЕЛЬНЫЕ ФУНКЦИИ ==========

static void *memset(void *s, int c, size_t n) {
    unsigned char *p = (unsigned char *)s;
    while (n--) *p++ = (unsigned char)c;
    return s;
}

static void *memcpy(void *dest, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;
    while (n--) *d++ = *s++;
    return dest;
}

static int strcmp(const char *s1, const char *s2) {
    while (*s1 && *s1 == *s2) { s1++; s2++; }
    return *(unsigned char *)s1 - *(unsigned char *)s2;
}

static int strlen(const char *s) {
    int len = 0;
    while (*s++) len++;
    return len;
}

// Выделение нового fd
static int alloc_fd(void) {
    for (int i = 0; i < MAX_FD_PER_PROCESS; i++) {
        if (current_fd_table->files[i] == NULL) {
            return i;
        }
    }
    return -1;
}

// Выделение file структуры
static file_t* alloc_file(void) {
    for (int i = 0; i < MAX_FILES_SYSTEM; i++) {
        if (file_table[i] == NULL) {
            file_table[i] = (file_t*)kmalloc(sizeof(file_t));
            if (file_table[i]) {
                memset(file_table[i], 0, sizeof(file_t));
                return file_table[i];
            }
        }
    }
    return NULL;
}

static void free_file(file_t *f) {
    for (int i = 0; i < MAX_FILES_SYSTEM; i++) {
        if (file_table[i] == f) {
            kfree(f);
            file_table[i] = NULL;
            return;
        }
    }
}

// Создание inode
inode_t* vfs_create_inode(uint32_t ino, file_type_t type, 
                           inode_ops_t *ops, void *private_data) {
    inode_t *inode = (inode_t*)kmalloc(sizeof(inode_t));
    if (!inode) return NULL;
    
    memset(inode, 0, sizeof(inode_t));
    inode->ino = ino;
    inode->type = type;
    inode->ref_count = 1;
    inode->private_data = private_data;
    inode->ops = ops;
    
    return inode;
}

// ========== ОПЕРАЦИИ С КОНСОЛЬЮ ==========

static int console_read(file_t *f, void *buf, size_t count) {
    (void)f;
    (void)buf;
    (void)count;
    // Чтение с консоли пока не реализовано
    return 0;
}

static int console_write(file_t *f, const void *buf, size_t count) {
    (void)f;
    if (!buf || count == 0) return 0;
    
    const char *str = (const char *)buf;
    int written = 0;
    
    for (size_t i = 0; i < count; i++) {
        if (str[i] == '\0') break;
        put_char(str[i]);
        written++;
    }
    
    return written;
}

static int console_seek(file_t *f, off_t offset, int whence) {
    (void)f;
    (void)offset;
    (void)whence;
    return -1;  // Консоль нельзя seek'ать
}

static int console_close(file_t *f) {
    (void)f;
    return 0;
}

static file_ops_t console_fops = {
    .read = console_read,
    .write = console_write,
    .seek = console_seek,
    .close = console_close,
};

// ========== ИНИЦИАЛИЗАЦИЯ ==========

void vfs_init(void) {
    // Очищаем таблицы
    for (int i = 0; i < MAX_FILES_SYSTEM; i++) {
        file_table[i] = NULL;
    }
    
    // Создаём таблицу fd для init процесса
    current_fd_table = (fd_table_t*)kmalloc(sizeof(fd_table_t));
    if (current_fd_table) {
        memset(current_fd_table, 0, sizeof(fd_table_t));
    }
    
    // Создаём корневой inode (заглушка)
    root_inode = vfs_create_inode(0, FT_DIRECTORY, NULL, NULL);
    
    // Регистрируем /dev/console и /dev/null
    vfs_register_dev("/dev/console", &console_fops, FT_CHARDEV);
    
    // Создаём предопределённые fd: 0, 1, 2 (stdin, stdout, stderr)
    // Все указывают на консоль
    file_t *stdout_file = alloc_file();
    if (stdout_file) {
        stdout_file->fd = 1;
        stdout_file->inode = vfs_create_inode(100, FT_CHARDEV, NULL, NULL);
        stdout_file->flags = O_WRONLY;
        stdout_file->ops = &console_fops;
    }
    
    file_t *stdin_file = alloc_file();
    if (stdin_file) {
        stdin_file->fd = 0;
        stdin_file->inode = vfs_create_inode(101, FT_CHARDEV, NULL, NULL);
        stdin_file->flags = O_RDONLY;
        stdin_file->ops = &console_fops;
    }
    
    file_t *stderr_file = alloc_file();
    if (stderr_file) {
        stderr_file->fd = 2;
        stderr_file->inode = vfs_create_inode(102, FT_CHARDEV, NULL, NULL);
        stderr_file->flags = O_WRONLY;
        stderr_file->ops = &console_fops;
    }
    
    // Устанавливаем предопределённые fd
    if (current_fd_table) {
        current_fd_table->files[0] = stdin_file;
        current_fd_table->files[1] = stdout_file;
        current_fd_table->files[2] = stderr_file;
        current_fd_table->count = 3;
    }
    
    printf("[VFS] Initialized (console registered)\n");
}

// ========== ОСНОВНЫЕ ОПЕРАЦИИ ==========

int vfs_open(const char *path, int flags) {
    if (!path || !*path) return -1;
    
    printf("[VFS] open(%s, flags=0x%x)\n", path, flags);
    
    // Специальные файлы устройств
    if (strcmp(path, "/dev/console") == 0) {
        int fd = alloc_fd();
        if (fd < 0) return -1;
        
        file_t *f = alloc_file();
        if (!f) return -1;
        
        f->fd = fd;
        f->inode = vfs_create_inode(100 + fd, FT_CHARDEV, NULL, NULL);
        f->flags = flags;
        f->offset = 0;
        f->ops = &console_fops;
        
        current_fd_table->files[fd] = f;
        current_fd_table->count++;
        
        return fd;
    }
    
    // Обычные файлы будут добавлены после интеграции FAT
    return -1;
}

int vfs_close(int fd) {
    if (fd < 0 || fd >= MAX_FD_PER_PROCESS) return -1;
    if (!current_fd_table->files[fd]) return -1;
    
    file_t *f = current_fd_table->files[fd];
    
    if (f->ops && f->ops->close) {
        f->ops->close(f);
    }
    
    if (f->inode) {
        f->inode->ref_count--;
        if (f->inode->ref_count <= 0) {
            kfree(f->inode);
        }
    }
    
    free_file(f);
    current_fd_table->files[fd] = NULL;
    current_fd_table->count--;
    
    return 0;
}

int vfs_read(int fd, void *buf, size_t count) {
    if (fd < 0 || fd >= MAX_FD_PER_PROCESS) return -1;
    if (!current_fd_table->files[fd]) return -1;
    
    file_t *f = current_fd_table->files[fd];
    
    if (f->ops && f->ops->read) {
        return f->ops->read(f, buf, count);
    }
    
    return -1;
}

int vfs_write(int fd, const void *buf, size_t count) {
    if (fd < 0 || fd >= MAX_FD_PER_PROCESS) return -1;
    if (!current_fd_table->files[fd]) return -1;
    
    file_t *f = current_fd_table->files[fd];
    
    if (f->ops && f->ops->write) {
        return f->ops->write(f, buf, count);
    }
    
    return -1;
}

int vfs_seek(int fd, off_t offset, int whence) {
    if (fd < 0 || fd >= MAX_FD_PER_PROCESS) return -1;
    if (!current_fd_table->files[fd]) return -1;
    
    file_t *f = current_fd_table->files[fd];
    
    if (f->ops && f->ops->seek) {
        return f->ops->seek(f, offset, whence);
    }
    
    return -1;
}

// ========== РЕГИСТРАЦИЯ УСТРОЙСТВ ==========

void vfs_register_dev(const char *name, file_ops_t *fops, file_type_t type) {
    printf("[VFS] Registered device '%s'\n", name);
    // Пока просто логируем
    (void)fops;
    (void)type;
}

// ========== ПОЛУЧЕНИЕ КОРНЯ ==========

inode_t* vfs_get_root(void) {
    return root_inode;
}

// ========== ЗАГОТОВКИ ДЛЯ БУДУЩИХ ФУНКЦИЙ ==========

int vfs_mkdir(const char *path) {
    (void)path;
    return -1;  // Будет реализовано при интеграции FAT
}

int vfs_rmdir(const char *path) {
    (void)path;
    return -1;
}

int vfs_readdir(int fd, void *buf) {
    (void)fd;
    (void)buf;
    return -1;
}

inode_t* vfs_lookup(const char *path) {
    (void)path;
    return NULL;
}