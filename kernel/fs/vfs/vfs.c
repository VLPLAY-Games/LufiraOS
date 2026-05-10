#include "vfs.h"
#include "system/mm/heap.h"
#include "drivers/console/console.h"
#include "lib/stddef.h"

// Forward declaration for FAT
extern int vfs_open_fat(const char *path, int flags);

// ========== ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ ==========

file_t *file_table[MAX_FILES_SYSTEM] = {0};
fd_table_t *current_fd_table = NULL;

// ========== ВСПОМОГАТЕЛЬНЫЕ ФУНКЦИИ ==========

static void *memset(void *s, int c, size_t n) {
    unsigned char *p = (unsigned char *)s;
    while (n--) *p++ = (unsigned char)c;
    return s;
}

static int strcmp(const char *s1, const char *s2) {
    while (*s1 && *s1 == *s2) { s1++; s2++; }
    return *(unsigned char *)s1 - *(unsigned char *)s2;
}

int alloc_fd(void) {
    for (int i = 0; i < MAX_FD_PER_PROCESS; i++) {
        if (current_fd_table->files[i] == NULL) {
            return i;
        }
    }
    return -1;
}

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

// ========== СОЗДАНИЕ INODE ==========

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

// ========== КОНСОЛЬ ==========

static int console_read(file_t *f, void *buf, size_t count) {
    (void)f; (void)buf; (void)count;
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
    (void)f; (void)offset; (void)whence;
    return -1;
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

// ========== VFS OPEN ==========

static int vfs_open_console(int flags) {
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

int vfs_open(const char *path, int flags) {
    if (!path || !*path) return -1;
    
    // Сначала пробуем FAT
    int fd = vfs_open_fat(path, flags);
    if (fd >= 0) return fd;
    
    // Затем специальные устройства
    if (strcmp(path, "/dev/console") == 0 || strcmp(path, "console") == 0) {
        return vfs_open_console(flags);
    }
    
    return -1;
}

// ========== ОСТАЛЬНЫЕ ОПЕРАЦИИ ==========

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

// ========== ИНИЦИАЛИЗАЦИЯ ==========

void vfs_init(void) {
    for (int i = 0; i < MAX_FILES_SYSTEM; i++) {
        file_table[i] = NULL;
    }
    
    current_fd_table = (fd_table_t*)kmalloc(sizeof(fd_table_t));
    if (current_fd_table) {
        memset(current_fd_table, 0, sizeof(fd_table_t));
    }
    
    // Предопределённые fd: 0,1,2
    file_t *stdin_f = alloc_file();
    if (stdin_f) {
        stdin_f->fd = 0;
        stdin_f->inode = vfs_create_inode(101, FT_CHARDEV, NULL, NULL);
        stdin_f->flags = O_RDONLY;
        stdin_f->ops = &console_fops;
    }
    
    file_t *stdout_f = alloc_file();
    if (stdout_f) {
        stdout_f->fd = 1;
        stdout_f->inode = vfs_create_inode(102, FT_CHARDEV, NULL, NULL);
        stdout_f->flags = O_WRONLY;
        stdout_f->ops = &console_fops;
    }
    
    file_t *stderr_f = alloc_file();
    if (stderr_f) {
        stderr_f->fd = 2;
        stderr_f->inode = vfs_create_inode(103, FT_CHARDEV, NULL, NULL);
        stderr_f->flags = O_WRONLY;
        stderr_f->ops = &console_fops;
    }
    
    if (current_fd_table) {
        current_fd_table->files[0] = stdin_f;
        current_fd_table->files[1] = stdout_f;
        current_fd_table->files[2] = stderr_f;
        current_fd_table->count = 3;
    }
    
    printf("[VFS] Initialized (FAT + console)\n");
}

// ========== ЗАГОТОВКИ ==========

void vfs_register_dev(const char *name, file_ops_t *fops, file_type_t type) {
    (void)name; (void)fops; (void)type;
}

int vfs_mkdir(const char *path) { (void)path; return -1; }
int vfs_rmdir(const char *path) { (void)path; return -1; }
int vfs_readdir(int fd, void *buf) { (void)fd; (void)buf; return -1; }
inode_t* vfs_lookup(const char *path) { (void)path; return NULL; }
inode_t* vfs_get_root(void) { return NULL; }