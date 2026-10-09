#include "vfs.h"
#include "system/mm/heap.h"
#include "system/process/process.h"
#include "drivers/console/console.h"
#include "drivers/input/input.h"
#include "lib/stddef.h"
#include "lib/string.h"
#include "fs/fat/fat_mount.h"
#include "fs/ramfs/ramfs.h"
#include "fs/ext2/ext2_mount.h"
#include "fs/exfat/exfat_mount.h"

extern int vfs_open_lufirafs(const char *path, int flags);

extern int vfs_lufirafs_create(const char *path);
extern int vfs_lufirafs_mkdir(const char *path);
extern int vfs_lufirafs_unlink(const char *path);

extern inode_t* vfs_lufirafs_lookup(const char *path);
extern inode_t* vfs_lufirafs_get_root(void);

// _at()-варианты (lufirafs_vfs.c) — см. vfs_*_at() ниже и комментарий у их
// объявлений в vfs.h.
extern int vfs_lufirafs_open_at(uint32_t base_inode, const char *path, int flags);
extern int vfs_lufirafs_create_at(uint32_t base_inode, const char *path);
extern int vfs_lufirafs_mkdir_at(uint32_t base_inode, const char *path);
extern int vfs_lufirafs_unlink_at(uint32_t base_inode, const char *path);
extern inode_t* vfs_lufirafs_lookup_at(uint32_t base_inode, const char *path);

/* ========== ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ ========== */

file_t *file_table[MAX_FILES_SYSTEM] = {0};

// current_fd_table указывает на fd_table ТЕКУЩЕГО процесса — сама её
// память живёт внутри соответствующего process_t (process.h), а не здесь.
// process_init() наводит указатель на idle-процесс ДО вызова vfs_init();
// process_create()/switch_to_process() дальше переставляют его на fd_table
// каждого нового/текущего процесса.
fd_table_t *current_fd_table = NULL;

/* ========== ВСПОМОГАТЕЛЬНЫЕ ФУНКЦИИ ========== */
// memset()/strcmp() теперь берутся из lib/string.h — общей библиотеки ядра.

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

// Настоящее блокирующее чтение (раньше — заглушка, всегда EOF):
// console_input_read() (drivers/input/input.c) блокирует процесс
// (PROCESS_BLOCKED + schedule()) и сама будит его, когда
// input_keyboard_event() кладёт байт в кольцевой буфер клавиатуры — тот
// же приём, что у pipe_read()/pipe_write() ниже.
static int console_read(file_t *f, void *buf, size_t count) {
    (void)f;
    if (!buf || count == 0) return 0;
    return console_input_read((uint8_t *)buf, (int)count);
}

// Лочит EFLAGS.IF на всё время записи (тот же приём, что heap_lock()/
// elf_irq_save(): сохраняем реальное состояние, а не безусловный
// cli/sti, на случай вызова из уже отключённых прерываний) — иначе
// таймерный IRQ мог переключить CPU на другой процесс посреди put_char(),
// и два писавших одновременно процесса перемешивали вывод байт-в-байт
// (баг: два ELF подряд дали "mmap1 OKatches OK" вместо двух строк).
static inline uint64_t console_write_lock(void) {
    uint64_t flags;
    asm volatile("pushfq; popq %0" : "=r"(flags) :: "memory");
    asm volatile("cli");
    return flags;
}

static inline void console_write_unlock(uint64_t flags) {
    asm volatile("push %0; popfq" : : "r"(flags) : "memory", "cc");
}

// Управляющие последовательности для /dev/console — свой простой формат,
// не настоящий ANSI (эта ОС не обещает совместимости с внешними
// терминалами). ESC (0x1B), затем однобайтовая команда:
//   ESC 'f' <color>              — set_foreground_color (0..15)
//   ESC 'b' <color>              — set_background_color (0..15)
//   ESC 'p' <x_hi><x_lo><y_hi><y_lo> — set_cursor_position (big-endian u16)
//   ESC 'c'                      — clear_screen
//   ESC 'l'                      — move_cursor_left (НЕ '\b': тот стирает
//                                   символ под курсором, см. put_char())
//   ESC 'r'                      — move_cursor_right
// Каждая последовательность должна укладываться ЦЕЛИКОМ в один sys_write()
// (парсер не хранит состояние между вызовами) — так их и собирает
// userspace/common/console.h.
static int console_write(file_t *f, const void *buf, size_t count) {
    (void)f;
    if (!buf || count == 0) return 0;

    uint64_t flags = console_write_lock();

    const uint8_t *str = (const uint8_t *)buf;
    int written = 0;
    size_t i = 0;
    while (i < count) {
        if (str[i] == '\0') break;

        if (str[i] == 0x1B && i + 1 < count) {
            char cmd = (char)str[i + 1];
            if (cmd == 'f' && i + 2 < count) {
                set_foreground_color((ConsoleColor)str[i + 2]);
                i += 3; written += 3; continue;
            }
            if (cmd == 'b' && i + 2 < count) {
                set_background_color((ConsoleColor)str[i + 2]);
                i += 3; written += 3; continue;
            }
            if (cmd == 'p' && i + 5 < count) {
                uint32_t x = ((uint32_t)str[i + 2] << 8) | str[i + 3];
                uint32_t y = ((uint32_t)str[i + 4] << 8) | str[i + 5];
                set_cursor_position(x, y);
                i += 6; written += 6; continue;
            }
            if (cmd == 'c') {
                clear_screen();
                i += 2; written += 2; continue;
            }
            if (cmd == 'l') {
                move_cursor_left();
                i += 2; written += 2; continue;
            }
            if (cmd == 'r') {
                move_cursor_right();
                i += 2; written += 2; continue;
            }
            // Незнакомая/обрезанная команда — печатаем ESC как обычный
            // символ, не застреваем.
        }

        put_char((char)str[i]);
        i++;
        written++;
    }

    console_write_unlock(flags);
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
    // Процесс, заблокированный в pipe_read()/pipe_write() (не более
    // одного с каждой стороны, как у console_input_waiter в input.c).
    // Раньше блокировка (PROCESS_BLOCKED + schedule()) ничего не будила
    // обратно в PROCESS_READY — планировщик пропускает всё, что не
    // PROCESS_READY, так что читатель, дождавшийся пустого буфера раньше
    // писателя, вис навсегда (не ловилось: test/pipe_test.elf всегда
    // пишет раньше, чем читает).
    process_t *read_waiter;
    process_t *write_waiter;
} pipe_t;

static void pipe_wake(process_t **waiter_slot) {
    if (*waiter_slot) {
        (*waiter_slot)->state = PROCESS_READY;
        *waiter_slot = NULL;
    }
}

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

            // НАЙДЕННЫЙ БАГ (терминал в GUI пустой: ни промпта, ни эха,
            // хотя шелл честно пишет в пайп) — цикл раньше ждал полный
            // count байт перед возвратом, даже если что-то уже прочитано.
            // Настоящий read() на пайпе обязан вернуться при ХОТЯ БЫ
            // одном байте ("короткое чтение"); terminal.c читает 256 байт
            // разом, а шелл пишет промпт десятками байт одним sys_write() —
            // они читались, но цикл не добирал до 256 и блокировал процесс
            // навсегда (шелл тем временем ждёт ввод от терминала, который
            // сам застрял здесь) — классический pipe-дедлок. Поэтому: если
            // total>0, отдаём то, что есть, не дожидаясь остального.
            if (total > 0) break;

            // Ждём данных: уступаем CPU, пока кто-то не запишет или не
            // закроет последний write-конец (тот же приём, что и в
            // process_sleep()/process_wait()) — pipe_write()/pipe_close_write()
            // ниже явно будят нас обратно через pipe_wake(&p->read_waiter).
            p->read_waiter = current_process;
            current_process->state = PROCESS_BLOCKED;
            schedule();
            continue;
        }

        out[total] = p->buffer[p->read_pos];
        p->read_pos = (p->read_pos + 1) % p->size;
        p->count--;
        total++;

        // Буфер только что освободил место — если писатель ждал именно
        // этого (буфер был полон), пусть проверит своё условие снова.
        pipe_wake(&p->write_waiter);
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
            p->write_waiter = current_process;
            current_process->state = PROCESS_BLOCKED;
            schedule();
            continue;
        }

        p->buffer[p->write_pos] = in[total];
        p->write_pos = (p->write_pos + 1) % p->size;
        p->count++;
        total++;

        // Буфер только что получил байт — если читатель ждал именно
        // этого (буфер был пуст), пусть проверит своё условие снова.
        pipe_wake(&p->read_waiter);
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
        // Писатель мог быть заблокирован именно на readers==0 ("сломанная
        // труба") — теперь это условие могло стать истинным, пусть
        // перепроверит.
        pipe_wake(&p->write_waiter);
        pipe_free_if_orphaned(p);
    }
    return 0;
}

static int pipe_close_write(file_t *f) {
    pipe_t *p = (pipe_t *)(f->inode ? f->inode->private_data : NULL);
    if (p) {
        p->writers--;
        // Читатель мог быть заблокирован в ожидании данных именно ПОТОМУ,
        // что writers > 0 — теперь, когда это могло стать false (EOF),
        // пусть перепроверит своё условие вместо вечного сна.
        pipe_wake(&p->read_waiter);
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

int vfs_poll_check(int fd, int events, int *out_revents) {
    *out_revents = 0;
    if (fd < 0 || fd >= MAX_FD_PER_PROCESS) return -1;
    if (!current_fd_table || !current_fd_table->files[fd]) return -1;

    file_t *f = current_fd_table->files[fd];

    if (f->ops == &pipe_read_fops) {
        pipe_t *p = (pipe_t *)(f->inode ? f->inode->private_data : NULL);
        // Готов к чтению: есть данные, ИЛИ все писатели уже закрылись
        // (EOF сам по себе тоже результат, которого ждёт poll()/read()).
        if (p && (events & 1) && (p->count > 0 || p->writers == 0))
            *out_revents |= 1;
        return 0;
    }
    if (f->ops == &pipe_write_fops) {
        pipe_t *p = (pipe_t *)(f->inode ? f->inode->private_data : NULL);
        // Готов к записи: есть место, ИЛИ читателей не осталось (тогда
        // следующий write() сразу вернёт "сломанная труба", а не заблокирует).
        if (p && (events & 2) && (p->count < p->size || p->readers == 0))
            *out_revents |= 2;
        return 0;
    }
    if (f->ops == &console_fops) {
        if ((events & 1) && console_input_has_data()) *out_revents |= 1;
        if (events & 2) *out_revents |= 2; // печать на экран никогда не блокирует
        return 0;
    }

    // Обычный файл/директория — LufiraFS целиком в RAM, I/O никогда не
    // блокирует, так что готов всегда.
    if (events & 1) *out_revents |= 1;
    if (events & 2) *out_revents |= 2;
    return 0;
}

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
    p->read_waiter = NULL;
    p->write_waiter = NULL;

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

    // Смонтированный FAT (/mnt/...) и RAMFS (/tmp и т.п.) — перед
    // LufiraFS: примонтированный путь должен перекрывать то, что там
    // было раньше. -2 значит "path не под этим монтированием", тогда
    // просто продолжаем как обычно.
    {
        int fat_fd = vfs_fat_open(path, flags);
        if (fat_fd != -2) return fat_fd;
    }
    {
        int ramfs_fd = vfs_ramfs_open(path, flags);
        if (ramfs_fd != -2) return ramfs_fd;
    }
    {
        int ext2_fd = vfs_ext2_open(path, flags);
        if (ext2_fd != -2) return ext2_fd;
    }
    {
        int exfat_fd = vfs_exfat_open(path, flags);
        if (exfat_fd != -2) return exfat_fd;
    }

    int fd =
        vfs_open_lufirafs(path, flags);

    if (fd >= 0)
        return fd;

    if (flags & O_CREAT) {

        if (vfs_lufirafs_create(path) != 0)
            return -1;

        // После создания открываем обычным способом.
        return vfs_open_lufirafs(path, flags);
    }

    if (strcmp(path, "/dev/console") == 0 ||
        strcmp(path, "console") == 0)
    {
        return vfs_open_console(flags);
    }

    return -1;
}

// То же самое, что vfs_open() выше, но разрешает path от base_inode, а не
// всегда от корня (см. vfs.h). Логика O_CREAT/console — дословная копия
// vfs_open()'а; отдельная функция, а не параметр по умолчанию у
// vfs_open(), чтобы не трогать сигнатуру уже широко используемой функции.
int vfs_open_at(uint32_t base_inode, const char *path, int flags)
{
    if (!path || !*path)
        return -1;

    if (path[0] == '/') {
        int fat_fd = vfs_fat_open(path, flags);
        if (fat_fd != -2) return fat_fd;
        int ramfs_fd = vfs_ramfs_open(path, flags);
        if (ramfs_fd != -2) return ramfs_fd;
        int ext2_fd = vfs_ext2_open(path, flags);
        if (ext2_fd != -2) return ext2_fd;
        int exfat_fd = vfs_exfat_open(path, flags);
        if (exfat_fd != -2) return exfat_fd;
    }

    int fd = vfs_lufirafs_open_at(base_inode, path, flags);
    if (fd >= 0)
        return fd;

    if (flags & O_CREAT) {
        if (vfs_lufirafs_create_at(base_inode, path) != 0)
            return -1;
        return vfs_lufirafs_open_at(base_inode, path, flags);
    }

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

    // f (file_t*) — ОДИН общий объект на все fd, что на него указывают:
    // fork() копирует fd_table родителя ребёнку присваиванием структуры
    // (process_fork(), process.c) — массив указателей, не сами file_t —
    // так что files[i] родителя и ребёнка указывают на один и тот же
    // file_t (тот же приём у vfs_dup2()). Раньше free_file(f) вызывался
    // здесь безусловно: первый же process_exit() форкнутого ребёнка
    // освобождал РОДИТЕЛЬСКИЙ file_t (например, у шелла) из-под ног —
    // double-free/use-after-free, на практике — шелл, ломающийся после
    // нескольких fork()+exec() подряд. f->inode->ref_count (vfs_dup_fd()
    // увеличивает его на каждое дублирование fd) освобождает f только на
    // последней ссылке.
    int last_ref = 1;
    if (f->inode) {
        f->inode->ref_count--;
        last_ref = (f->inode->ref_count <= 0);
        if (last_ref) {
            kfree(f->inode);
        }
    }

    if (last_ref) {
        free_file(f);
    }
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

    // current_fd_table уже указывает на fd_table idle-процесса (наводит
    // process_init(), раньше vfs_init()) — заполняем её stdin/stdout/stderr.
    vfs_init_fd_table(current_fd_table);

    printf("[VFS] Initialized (LufiraFS + console, per-process fd tables)\n");
}

/* ========== ЗАГОТОВКИ ========== */

void vfs_register_dev(const char *name, file_ops_t *fops, file_type_t type) {
    (void)name; (void)fops; (void)type;
}

int vfs_create(const char *path)
{
    if (!path || !*path)
        return -1;

    int fat_fd = vfs_fat_open(path, O_CREAT | O_RDONLY);
    if (fat_fd != -2) {
        if (fat_fd < 0) return -1;
        vfs_close(fat_fd);
        return 0;
    }
    int ramfs_fd = vfs_ramfs_open(path, O_CREAT | O_RDONLY);
    if (ramfs_fd != -2) {
        if (ramfs_fd < 0) return -1;
        vfs_close(ramfs_fd);
        return 0;
    }
    int ext2_fd = vfs_ext2_open(path, O_CREAT | O_RDONLY);
    if (ext2_fd != -2) {
        if (ext2_fd < 0) return -1;
        vfs_close(ext2_fd);
        return 0;
    }
    int exfat_fd = vfs_exfat_open(path, O_CREAT | O_RDONLY);
    if (exfat_fd != -2) {
        if (exfat_fd < 0) return -1;
        vfs_close(exfat_fd);
        return 0;
    }

    return vfs_lufirafs_create(path);
}


int vfs_mkdir(const char *path)
{
    if (!path || !*path)
        return -1;

    int r = vfs_fat_mkdir(path);
    if (r != -2) return r;
    r = vfs_ramfs_mkdir(path);
    if (r != -2) return r;
    r = vfs_ext2_mkdir(path);
    if (r != -2) return r;
    r = vfs_exfat_mkdir(path);
    if (r != -2) return r;

    return vfs_lufirafs_mkdir(path);
}


int vfs_unlink(const char *path)
{
    if (!path || !*path)
        return -1;

    int r = vfs_fat_unlink(path);
    if (r != -2) return r;
    r = vfs_ramfs_unlink(path);
    if (r != -2) return r;
    r = vfs_ext2_unlink(path);
    if (r != -2) return r;
    r = vfs_exfat_unlink(path);
    if (r != -2) return r;

    return vfs_lufirafs_unlink(path);
}


int vfs_rmdir(const char *path)
{
    if (!path || !*path)
        return -1;

    int r = vfs_fat_unlink(path);
    if (r != -2) return r;
    r = vfs_ramfs_unlink(path);
    if (r != -2) return r;
    r = vfs_ext2_unlink(path);
    if (r != -2) return r;
    r = vfs_exfat_unlink(path);
    if (r != -2) return r;

    return vfs_lufirafs_unlink(path);
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

    inode_t *fat_inode = vfs_fat_lookup(path);
    if (fat_inode) return fat_inode;
    inode_t *ramfs_inode = vfs_ramfs_lookup(path);
    if (ramfs_inode) return ramfs_inode;
    inode_t *ext2_inode = vfs_ext2_lookup(path);
    if (ext2_inode) return ext2_inode;
    inode_t *exfat_inode = vfs_exfat_lookup(path);
    if (exfat_inode) return exfat_inode;

    return vfs_lufirafs_lookup(path);
}


int vfs_mkdir_at(uint32_t base_inode, const char *path)
{
    if (!path || !*path)
        return -1;

    if (path[0] == '/') {
        int r = vfs_fat_mkdir(path);
        if (r != -2) return r;
        r = vfs_ramfs_mkdir(path);
        if (r != -2) return r;
        r = vfs_ext2_mkdir(path);
        if (r != -2) return r;
        r = vfs_exfat_mkdir(path);
        if (r != -2) return r;
    }

    return vfs_lufirafs_mkdir_at(base_inode, path);
}


int vfs_rmdir_at(uint32_t base_inode, const char *path)
{
    if (!path || !*path)
        return -1;

    if (path[0] == '/') {
        int r = vfs_fat_unlink(path);
        if (r != -2) return r;
        r = vfs_ramfs_unlink(path);
        if (r != -2) return r;
        r = vfs_ext2_unlink(path);
        if (r != -2) return r;
        r = vfs_exfat_unlink(path);
        if (r != -2) return r;
    }

    return vfs_lufirafs_unlink_at(base_inode, path);
}


int vfs_unlink_at(uint32_t base_inode, const char *path)
{
    if (!path || !*path)
        return -1;

    if (path[0] == '/') {
        int r = vfs_fat_unlink(path);
        if (r != -2) return r;
        r = vfs_ramfs_unlink(path);
        if (r != -2) return r;
        r = vfs_ext2_unlink(path);
        if (r != -2) return r;
        r = vfs_exfat_unlink(path);
        if (r != -2) return r;
    }

    return vfs_lufirafs_unlink_at(base_inode, path);
}


int vfs_create_at(uint32_t base_inode, const char *path)
{
    if (!path || !*path)
        return -1;

    if (path[0] == '/') {
        int fat_fd = vfs_fat_open(path, O_CREAT | O_RDONLY);
        if (fat_fd != -2) {
            if (fat_fd < 0) return -1;
            vfs_close(fat_fd);
            return 0;
        }
        int ramfs_fd = vfs_ramfs_open(path, O_CREAT | O_RDONLY);
        if (ramfs_fd != -2) {
            if (ramfs_fd < 0) return -1;
            vfs_close(ramfs_fd);
            return 0;
        }
        int ext2_fd = vfs_ext2_open(path, O_CREAT | O_RDONLY);
        if (ext2_fd != -2) {
            if (ext2_fd < 0) return -1;
            vfs_close(ext2_fd);
            return 0;
        }
        int exfat_fd = vfs_exfat_open(path, O_CREAT | O_RDONLY);
        if (exfat_fd != -2) {
            if (exfat_fd < 0) return -1;
            vfs_close(exfat_fd);
            return 0;
        }
    }

    return vfs_lufirafs_create_at(base_inode, path);
}


inode_t* vfs_lookup_at(uint32_t base_inode, const char *path)
{
    if (!path || !*path)
        return NULL;

    if (path[0] == '/') {
        inode_t *fat_inode = vfs_fat_lookup(path);
        if (fat_inode) return fat_inode;
        inode_t *ramfs_inode = vfs_ramfs_lookup(path);
        if (ramfs_inode) return ramfs_inode;
        inode_t *ext2_inode = vfs_ext2_lookup(path);
        if (ext2_inode) return ext2_inode;
        inode_t *exfat_inode = vfs_exfat_lookup(path);
        if (exfat_inode) return exfat_inode;
    }

    return vfs_lufirafs_lookup_at(base_inode, path);
}


inode_t* vfs_get_root(void)
{
    return vfs_lufirafs_get_root();
}