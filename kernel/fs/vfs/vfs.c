#include "vfs.h"
#include "system/mm/heap.h"
#include "system/process/process.h"
#include "drivers/console/console.h"
#include "lib/stddef.h"

extern int vfs_open_fat(const char *path, int flags);

extern int vfs_fat_create(const char *path);
extern int vfs_fat_mkdir(const char *path);
extern int vfs_fat_unlink(const char *path);

extern inode_t* vfs_fat_lookup(const char *path);
extern inode_t* vfs_fat_get_root(void);

/* ========== ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ ========== */

file_t *file_table[MAX_FILES_SYSTEM] = {0};

// current_fd_table указывает на fd_table ТЕКУЩЕГО процесса — сама её
// память живёт внутри соответствующего process_t (process.h), а не здесь.
// process_init() наводит указатель на idle-процесс ДО вызова vfs_init();
// process_create()/switch_to_process() дальше переставляют его на fd_table
// каждого нового/текущего процесса.
fd_table_t *current_fd_table = NULL;

/* ========== ВСПОМОГАТЕЛЬНЫЕ ФУНКЦИИ ========== */

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

file_t* alloc_file(void) {
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

/* ========== СОЗДАНИЕ INODE ========== */

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

/* ========== КОНСОЛЬ ========== */

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

static uint32_t dev_inode_counter = 200;

/* ========== PIPES ========== */

#define PIPE_BUF_SIZE 4096

// Общее состояние одного анонимного pipe(). У read-конца и write-конца —
// СВОИ отдельные inode_t (каждый со своим ref_count, который увеличивает
// vfs_dup_fd() при fork()/dup2()), но оба указывают через
// inode->private_data на ОДИН и тот же pipe_t. readers/writers считают
// именно количество живых file_t-ссылок на соответствующий конец (а не
// просто "открыт/закрыт"), чтобы fork() не путал дело.
typedef struct pipe {
    uint8_t *buffer;
    uint32_t size;
    uint32_t read_pos;
    uint32_t write_pos;
    uint32_t count;      // байт сейчас в буфере
    int readers;
    int writers;
} pipe_t;

static void pipe_free_if_orphaned(pipe_t *p) {
    if (p->readers == 0 && p->writers == 0) {
        kfree(p->buffer);
        kfree(p);
    }
}

static int pipe_read(file_t *f, void *buf, size_t count) {
    pipe_t *p = (pipe_t *)(f->inode ? f->inode->private_data : NULL);
    if (!p || !buf) return -1;

    uint8_t *out = (uint8_t *)buf;
    size_t total = 0;

    while (total < count) {
        if (p->count == 0) {
            if (p->writers == 0) {
                break; // писателей больше нет и буфер пуст - EOF
            }
            // Ждём данных: уступаем CPU, пока кто-то не запишет или не
            // закроет последний write-конец (тот же приём, что и в
            // process_sleep()/process_wait()).
            current_process->state = PROCESS_BLOCKED;
            schedule();
            continue;
        }

        out[total] = p->buffer[p->read_pos];
        p->read_pos = (p->read_pos + 1) % p->size;
        p->count--;
        total++;
    }

    return (int)total;
}

static int pipe_write(file_t *f, const void *buf, size_t count) {
    pipe_t *p = (pipe_t *)(f->inode ? f->inode->private_data : NULL);
    if (!p || !buf) return -1;

    const uint8_t *in = (const uint8_t *)buf;
    size_t total = 0;

    while (total < count) {
        if (p->readers == 0) {
            break; // никто больше не читает - "сломанная труба"
        }

        if (p->count == p->size) {
            current_process->state = PROCESS_BLOCKED;
            schedule();
            continue;
        }

        p->buffer[p->write_pos] = in[total];
        p->write_pos = (p->write_pos + 1) % p->size;
        p->count++;
        total++;
    }

    if (total == 0 && count > 0)
        return -1;

    return (int)total;
}

static int pipe_seek(file_t *f, off_t offset, int whence) {
    (void)f; (void)offset; (void)whence;
    return -1; // пайпы не поддерживают seek
}

static int pipe_close_read(file_t *f) {
    pipe_t *p = (pipe_t *)(f->inode ? f->inode->private_data : NULL);
    if (p) {
        p->readers--;
        pipe_free_if_orphaned(p);
    }
    return 0;
}

static int pipe_close_write(file_t *f) {
    pipe_t *p = (pipe_t *)(f->inode ? f->inode->private_data : NULL);
    if (p) {
        p->writers--;
        pipe_free_if_orphaned(p);
    }
    return 0;
}

static file_ops_t pipe_read_fops = {
    .read = pipe_read,
    .write = NULL,
    .seek = pipe_seek,
    .close = pipe_close_read,
};

static file_ops_t pipe_write_fops = {
    .read = NULL,
    .write = pipe_write,
    .seek = pipe_seek,
    .close = pipe_close_write,
};

// Регистрирует ещё одну ссылку на уже открытый file_t (используется
// fork()'ом и vfs_dup2() — оба случая, когда один и тот же file_t
// оказывается в двух разных fd-слотах/процессах одновременно). Помимо
// обычного inode->ref_count, для пайпов дополнительно увеличивает
// readers/writers — иначе fork() ребёнка пайпа "потерял" бы одну из двух
// независимых будущих vfs_close().
void vfs_dup_fd(file_t *f) {
    if (!f || !f->inode)
        return;

    f->inode->ref_count++;

    if (f->inode->type == FT_PIPE && f->inode->private_data) {
        pipe_t *p = (pipe_t *)f->inode->private_data;
        if (f->ops == &pipe_read_fops) p->readers++;
        else if (f->ops == &pipe_write_fops) p->writers++;
    }
}

int vfs_pipe(int fds[2]) {
    if (!fds || !current_fd_table)
        return -1;

    pipe_t *p = (pipe_t *)kmalloc(sizeof(pipe_t));
    if (!p) return -1;

    p->buffer = (uint8_t *)kmalloc(PIPE_BUF_SIZE);
    if (!p->buffer) { kfree(p); return -1; }

    p->size = PIPE_BUF_SIZE;
    p->read_pos = 0;
    p->write_pos = 0;
    p->count = 0;
    p->readers = 1;
    p->writers = 1;

    int read_fd = alloc_fd();
    file_t *rf = (read_fd >= 0) ? alloc_file() : NULL;
    if (!rf) { kfree(p->buffer); kfree(p); return -1; }

    rf->fd = read_fd;
    rf->inode = vfs_create_inode(dev_inode_counter++, FT_PIPE, NULL, p);
    rf->flags = O_RDONLY;
    rf->offset = 0;
    rf->ops = &pipe_read_fops;
    current_fd_table->files[read_fd] = rf;
    current_fd_table->count++;

    int write_fd = alloc_fd();
    file_t *wf = (write_fd >= 0) ? alloc_file() : NULL;
    if (!wf) {
        vfs_close(read_fd); // откатываем уже открытый read-конец целиком
        return -1;
    }

    wf->fd = write_fd;
    wf->inode = vfs_create_inode(dev_inode_counter++, FT_PIPE, NULL, p);
    wf->flags = O_WRONLY;
    wf->offset = 0;
    wf->ops = &pipe_write_fops;
    current_fd_table->files[write_fd] = wf;
    current_fd_table->count++;

    fds[0] = read_fd;
    fds[1] = write_fd;
    return 0;
}

int vfs_dup2(int oldfd, int newfd) {
    if (!current_fd_table)
        return -1;
    if (oldfd < 0 || oldfd >= MAX_FD_PER_PROCESS) return -1;
    if (newfd < 0 || newfd >= MAX_FD_PER_PROCESS) return -1;
    if (!current_fd_table->files[oldfd]) return -1;

    if (oldfd == newfd)
        return newfd;

    if (current_fd_table->files[newfd]) {
        vfs_close(newfd);
    }

    file_t *f = current_fd_table->files[oldfd];
    vfs_dup_fd(f);
    current_fd_table->files[newfd] = f;
    current_fd_table->count++;

    return newfd;
}

static int vfs_open_console(int flags) {
    int fd = alloc_fd();
    if (fd < 0) return -1;

    file_t *f = alloc_file();
    if (!f) return -1;

    f->fd = fd;
    f->inode = vfs_create_inode(dev_inode_counter++, FT_CHARDEV, NULL, NULL);
    f->flags = flags;
    f->offset = 0;
    f->ops = &console_fops;

    current_fd_table->files[fd] = f;
    current_fd_table->count++;
    return fd;
}

/* ========== VFS OPEN ========== */

int vfs_open(const char *path, int flags)
{
    if (!path || !*path)
        return -1;

    /*
     * FAT сначала.
     */
    int fd =
        vfs_open_fat(path, flags);

    if (fd >= 0)
        return fd;

    /*
     * O_CREAT:
     *
     * Если файла нет — создаём.
     */
    if (flags & O_CREAT) {

        if (vfs_fat_create(path) != 0)
            return -1;

        /*
         * После создания открываем обычным способом.
         */
        return vfs_open_fat(path, flags);
    }

    /*
     * Console.
     */
    if (strcmp(path, "/dev/console") == 0 ||
        strcmp(path, "console") == 0)
    {
        return vfs_open_console(flags);
    }

    return -1;
}

/* ========== ОСТАЛЬНЫЕ ОПЕРАЦИИ ========== */

int vfs_close(int fd) {
    if (fd < 0 || fd >= MAX_FD_PER_PROCESS) return -1;
    if (!current_fd_table || !current_fd_table->files[fd]) return -1;

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
    if (!current_fd_table || !current_fd_table->files[fd]) return -1;

    file_t *f = current_fd_table->files[fd];
    if (f->ops && f->ops->read) {
        return f->ops->read(f, buf, count);
    }
    return -1;
}

int vfs_write(int fd, const void *buf, size_t count) {
    if (fd < 0 || fd >= MAX_FD_PER_PROCESS) return -1;
    if (!current_fd_table || !current_fd_table->files[fd]) return -1;

    file_t *f = current_fd_table->files[fd];
    if (f->ops && f->ops->write) {
        return f->ops->write(f, buf, count);
    }
    return -1;
}

int vfs_seek(int fd, off_t offset, int whence) {
    if (fd < 0 || fd >= MAX_FD_PER_PROCESS) return -1;
    if (!current_fd_table || !current_fd_table->files[fd]) return -1;

    file_t *f = current_fd_table->files[fd];
    if (f->ops && f->ops->seek) {
        return f->ops->seek(f, offset, whence);
    }
    return -1;
}

/* ========== ИНИЦИАЛИЗАЦИЯ ========== */

// Заполняет table стандартными stdin/stdout/stderr (консоль). alloc_fd()/
// alloc_file() всегда работают через current_fd_table, поэтому на время
// заполнения ЧУЖОЙ (не обязательно текущей) таблицы временно подменяем
// указатель и возвращаем его обратно.
void vfs_init_fd_table(fd_table_t *table) {
    if (!table) return;

    fd_table_t *saved = current_fd_table;
    current_fd_table = table;
    memset(table, 0, sizeof(fd_table_t));

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

    table->files[0] = stdin_f;
    table->files[1] = stdout_f;
    table->files[2] = stderr_f;
    table->count = 3;

    current_fd_table = saved;
}

void vfs_init(void) {
    for (int i = 0; i < MAX_FILES_SYSTEM; i++) {
        file_table[i] = NULL;
    }

    /*
     * current_fd_table уже указывает на fd_table idle-процесса — его
     * туда наводит process_init(), который выполняется раньше vfs_init()
     * при загрузке. Заполняем её стандартными stdin/stdout/stderr.
     */
    vfs_init_fd_table(current_fd_table);

    printf("[VFS] Initialized (FAT + console, per-process fd tables)\n");
}

/* ========== ЗАГОТОВКИ ========== */

void vfs_register_dev(const char *name, file_ops_t *fops, file_type_t type) {
    (void)name; (void)fops; (void)type;
}

int vfs_create(const char *path)
{
    if (!path || !*path)
        return -1;

    return vfs_fat_create(path);
}


int vfs_mkdir(const char *path)
{
    if (!path || !*path)
        return -1;

    return vfs_fat_mkdir(path);
}


int vfs_unlink(const char *path)
{
    if (!path || !*path)
        return -1;

    return vfs_fat_unlink(path);
}


int vfs_rmdir(const char *path)
{
    if (!path || !*path)
        return -1;

    return vfs_fat_unlink(path);
}


int vfs_readdir(int fd, void *buf)
{
    if (fd < 0 ||
        fd >= MAX_FD_PER_PROCESS)
    {
        return -1;
    }

    if (!current_fd_table)
        return -1;

    file_t *f =
        current_fd_table->files[fd];

    if (!f || !f->inode)
        return -1;

    if (f->inode->type != FT_DIRECTORY)
        return -1;

    if (!f->inode->ops ||
        !f->inode->ops->readdir)
    {
        return -1;
    }

    int result =
        f->inode->ops->readdir(
            f->inode,
            buf,
            (int)f->offset
        );

    if (result > 0)
        f->offset++;

    return result;
}


inode_t* vfs_lookup(const char *path)
{
    if (!path || !*path)
        return NULL;

    return vfs_fat_lookup(path);
}


inode_t* vfs_get_root(void)
{
    return vfs_fat_get_root();
}