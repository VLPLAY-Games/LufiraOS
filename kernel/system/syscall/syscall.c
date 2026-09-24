#include "syscall.h"
#include "drivers/console/console.h"
#include "system/process/process.h"
#include "system/elf/elf.h"
#include "system/timer/pit.h"
#include "system/cpu/gdt.h"
#include "system/mm/heap.h"
#include "system/mm/pmm.h"
#include "system/mm/paging.h"
#include "lib/stddef.h"
#include "lib/string.h"
#include "fs/vfs/vfs.h"
#include "fs/lufirafs/lufirafs.h"
#include "system/devmode/devmode.h"

extern lufirafs_t lufirafs;

// Открывает filename через VFS, читает его целиком и заменяет им текущий
// процесс через elf_exec_replace() (настоящий execve()). Используется и
// шеллом (команда "exec"), и системным вызовом SYS_EXEC. argv/envp
// передаются elf_exec_replace() как есть — ОНА забирает владение ими (см.
// комментарий у её объявления в elf.h) и освобождает их на каждом СВОЁМ
// пути отказа; но если do_exec() проваливается РАНЬШЕ вызова
// elf_exec_replace() (файл не нашёлся/не прочитался/пуст), освобождать
// их обязаны мы сами здесь — иначе они просто утекут.
int do_exec(const char *filename, char *argv[], char *envp[]) {
    if (!filename || !*filename) {
        free_argv_envp(argv, envp);
        return -1;
    }

    // Проверка бита исполнения — VFS-пути всегда разрешаются от корня (см.
    // комментарий вверху lufirafs_vfs.c), поэтому резолвим так же, а не
    // через cwd_inode (в отличие от sys_chmod/sys_chown ниже).
    if (current_process) {
        uint32_t ino;
        if (lufirafs_lookup(&lufirafs, lufirafs.sb.root_inode, filename, &ino) == 0) {
            lufirafs_inode_t inode;
            if (lufirafs_read_inode(&lufirafs, ino, &inode) == 0 &&
                !lufirafs_check_access(&inode, current_process->uid, current_process->gid, 0, 0, 1)) {
                free_argv_envp(argv, envp);
                return -1;
            }
        }
    }

    int fd = vfs_open(filename, O_RDONLY);
    if (fd < 0) {
        printf("[EXEC] Failed to open %s\n", filename);
        free_argv_envp(argv, envp);
        return -1;
    }

    file_t *f = current_fd_table->files[fd];
    if (!f || !f->inode) {
        vfs_close(fd);
        free_argv_envp(argv, envp);
        return -1;
    }
    uint32_t size = f->inode->size;
    if (size == 0) {
        vfs_close(fd);
        free_argv_envp(argv, envp);
        return -1;
    }

    uint8_t *buf = (uint8_t *)kmalloc(size);
    if (!buf) {
        vfs_close(fd);
        free_argv_envp(argv, envp);
        return -1;
    }

    int bytes_read = vfs_read(fd, buf, size);
    vfs_close(fd);
    if (bytes_read != (int)size) {
        kfree(buf);
        free_argv_envp(argv, envp);
        return -1;
    }

    // elf_exec_replace() освобождает и buf, и argv/envp при любом исходе
    // (успех или неудача) — начиная с этой точки владение уже её.
    return elf_exec_replace(buf, size, filename, argv, envp);
}

typedef uint64_t (*syscall_fn_t)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

// Проверяет, что addr указывает на NUL-терминированную строку длиной не
// более max_len байт (без учёта терминатора), целиком лежащую в читаемой
// памяти ТЕКУЩЕГО процесса — is_user_accessible() проверяется на каждой
// впервые пересечённой странице, ПЕРЕД тем как эта страница разыменовывается
// в поиске '\0'. pml4_phys ОБЯЗАН быть активным CR3 (вызывается только из
// обработчиков syscall'ов, которые всегда исполняются под собственным CR3
// вызывающего процесса) — иначе прямое разыменование (const char*)addr
// ниже указывало бы не туда. Возвращает длину строки (>=0) при успехе, -1
// при невалидном адресе/странице или если '\0' не найден в пределах max_len.
static int64_t validate_user_string(uint64_t pml4_phys, uint64_t addr, uint64_t max_len) {
    if (addr == 0) return -1;

    uint64_t checked_page = 0;
    int have_checked = 0;

    for (uint64_t i = 0; i < max_len; i++) {
        uint64_t cur = addr + i;
        uint64_t page = cur & ~(uint64_t)(PAGE_SIZE - 1);
        if (!have_checked || page != checked_page) {
            if (!is_user_accessible(pml4_phys, page, 0)) return -1;
            checked_page = page;
            have_checked = 1;
        }
        if (*(const char*)cur == '\0') return (int64_t)i;
    }
    return -1;
}

// Копирует NUL-терминированный массив указателей на NUL-терминированные
// строки (argv[]/envp[]-стиль) из ПОЛЬЗОВАТЕЛЬСКОЙ памяти ТЕКУЩЕГО
// процесса в свежевыделенный kernel-side буфер (kmalloc на сам массив
// указателей + отдельный kmalloc на каждую строку) — та самая форма,
// которую dl_exec_replace()/free_argv_envp() (elf.h) ожидают/освобождают.
// pml4_phys ОБЯЗАН быть активным CR3 (те же условия, что и у
// validate_user_string() выше). array_ptr==0 трактуется как "аргументов
// нет вовсе" (argc=0), а не ошибка — вызывающему (sys_exec) не нужно
// отдельно различать "argv не передан" и "передан пустым". Возвращает
// NULL при любой другой ошибке (плохой указатель, больше MAX_EXEC_ARGS
// элементов, строка длиннее USER_STRING_MAX) — и тогда ничего не остаётся
// частично выделенным.
static char **copy_user_string_array(uint64_t pml4_phys, uint64_t array_ptr) {
    if (array_ptr == 0) {
        char **empty = (char **)kmalloc(sizeof(char*));
        if (empty) empty[0] = NULL;
        return empty;
    }

    if (!is_user_range_valid(pml4_phys, array_ptr, (uint64_t)(MAX_EXEC_ARGS + 1) * sizeof(uint64_t), 0))
        return NULL;

    const uint64_t *user_array = (const uint64_t *)array_ptr;
    int count = 0;
    while (count < MAX_EXEC_ARGS && user_array[count] != 0) count++;
    if (count >= MAX_EXEC_ARGS) return NULL;   // терминатор не найден в разумных пределах

    char **result = (char **)kmalloc(sizeof(char*) * (size_t)(count + 1));
    if (!result) return NULL;
    for (int i = 0; i <= count; i++) result[i] = NULL;

    for (int i = 0; i < count; i++) {
        int64_t slen = validate_user_string(pml4_phys, user_array[i], USER_STRING_MAX);
        if (slen < 0) {
            for (int j = 0; j < i; j++) kfree(result[j]);
            kfree(result);
            return NULL;
        }
        char *copy = (char *)kmalloc((size_t)slen + 1);
        if (!copy) {
            for (int j = 0; j < i; j++) kfree(result[j]);
            kfree(result);
            return NULL;
        }
        memcpy(copy, (const void *)user_array[i], (size_t)slen + 1);
        result[i] = copy;
    }

    return result;
}

// ========== РЕАЛИЗАЦИИ СИСТЕМНЫХ ВЫЗОВОВ ==========

// SYS_WRITE (0): fd, buffer, length
static uint64_t sys_write(uint64_t fd, uint64_t buffer, uint64_t length,
                          uint64_t unused1, uint64_t unused2) {
    (void)unused1;
    (void)unused2;

    if (length == 0) return 0;
    if (!current_process || !is_user_range_valid(current_process->page_table, buffer, length, 0))
        return (uint64_t)-EFAULT;

    // Используем VFS!
    return (uint64_t)vfs_write((int)fd, (const void *)buffer, (size_t)length);
}

// SYS_READ (1): fd, buffer, length
static uint64_t sys_read(uint64_t fd, uint64_t buffer, uint64_t length,
                         uint64_t unused1, uint64_t unused2) {
    (void)unused1;
    (void)unused2;

    if (length == 0) return 0;
    // need_write=1: ядро ПИШЕТ в buffer прочитанные байты.
    if (!current_process || !is_user_range_valid(current_process->page_table, buffer, length, 1))
        return (uint64_t)-EFAULT;

    // Используем VFS!
    return (uint64_t)vfs_read((int)fd, (void *)buffer, (size_t)length);
}

// SYS_EXIT (2): exit_code
static uint64_t sys_exit(uint64_t exit_code, uint64_t unused1, uint64_t unused2,
                         uint64_t unused3, uint64_t unused4) {
    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    
    printf("\n[%u] Exit(%u)\n", 
           current_process ? current_process->pid : 0, 
           (uint32_t)exit_code);
    
    process_exit((int)exit_code);
    while (1) __asm__("hlt");
    return 0;
}

// SYS_GETPID (3)
static uint64_t sys_getpid(uint64_t unused1, uint64_t unused2, uint64_t unused3,
                           uint64_t unused4, uint64_t unused5) {
    (void)unused1; (void)unused2; (void)unused3; (void)unused4; (void)unused5;
    return current_process ? current_process->pid : 0;
}

// SYS_YIELD (4)
static uint64_t sys_yield(uint64_t unused1, uint64_t unused2, uint64_t unused3,
                          uint64_t unused4, uint64_t unused5) {
    (void)unused1; (void)unused2; (void)unused3; (void)unused4; (void)unused5;
    schedule();
    return 0;
}

// SYS_GETTICK (5)
static uint64_t sys_gettick(uint64_t unused1, uint64_t unused2, uint64_t unused3,
                            uint64_t unused4, uint64_t unused5) {
    (void)unused1; (void)unused2; (void)unused3; (void)unused4; (void)unused5;
    return pit_get_ticks();
}

// SYS_OPEN (6): filename, flags, mode
static uint64_t sys_open(uint64_t filename_ptr, uint64_t flags, uint64_t mode,
                         uint64_t unused1, uint64_t unused2) {
    (void)mode;
    (void)unused1;
    (void)unused2;

    if (!current_process) return (uint64_t)-EFAULT;
    if (validate_user_string(current_process->page_table, filename_ptr, USER_STRING_MAX) < 0)
        return (uint64_t)-EFAULT;

    const char *filename = (const char *)filename_ptr;

    // Проверка прав — VFS всегда резолвит пути от корня (см. комментарий
    // вверху lufirafs_vfs.c), поэтому резолвим так же здесь, отдельно от
    // самого vfs_open() (который своей проверки не делает вообще).
    uint64_t accmode = flags & 0x3;
    int want_read = (accmode != O_WRONLY);
    int want_write = (accmode != O_RDONLY);

    uint32_t ino;
    if (lufirafs_lookup(&lufirafs, lufirafs.sb.root_inode, filename, &ino) == 0) {
        lufirafs_inode_t inode;
        if (lufirafs_read_inode(&lufirafs, ino, &inode) == 0 &&
            !lufirafs_check_access(&inode, current_process->uid, current_process->gid,
                                    want_read, want_write, 0)) {
            return (uint64_t)-EACCES;
        }
    } else if (flags & O_CREAT) {
        uint32_t parent;
        char leaf[LUFIRAFS_MAX_NAME + 1];
        if (lufirafs_resolve_parent(&lufirafs, lufirafs.sb.root_inode, filename, &parent, leaf) == 0) {
            lufirafs_inode_t pinode;
            if (lufirafs_read_inode(&lufirafs, parent, &pinode) == 0 &&
                !lufirafs_check_access(&pinode, current_process->uid, current_process->gid, 0, 1, 1)) {
                return (uint64_t)-EACCES;
            }
        }
    }

    return (uint64_t)vfs_open(filename, (int)flags);
}

// SYS_CLOSE (7): fd
static uint64_t sys_close(uint64_t fd, uint64_t unused1, uint64_t unused2,
                          uint64_t unused3, uint64_t unused4) {
    (void)unused1; (void)unused2; (void)unused3; (void)unused4;
    
    return (uint64_t)vfs_close((int)fd);
}

// SYS_SEEK (8): fd, offset, whence
static uint64_t sys_seek(uint64_t fd, uint64_t offset, uint64_t whence,
                         uint64_t unused1, uint64_t unused2) {
    (void)unused1;
    (void)unused2;
    
    return (uint64_t)vfs_seek((int)fd, (off_t)offset, (int)whence);
}

// SYS_MMAP (9): addr, length, prot, flags, fd
// Только анонимная память (MAP_ANONYMOUS обязателен, addr/fd игнорируются —
// MAP_FIXED не поддерживается, своего адреса не бывает). offset шестым
// аргументом не нужен для анонимного mmap и диспетчер всё равно передаёт
// только 5 аргументов (см. комментарий у syscall_frame_t в process.c) —
// файловый mmap с offset остаётся будущей задачей.
//
// Выделение "eager": все страницы физически выделяются и маппятся прямо
// здесь, а не по требованию через page fault — обработчик page fault
// (isr_common_handler(), idt.c) сейчас безусловно останавливает систему на
// ЛЮБОМ фолте, реального пути восстановления для demand paging нет.
//
// Вызывается как syscall самого процесса, значит current_process->page_table
// — это уже активный CR3: свежесмапленная страница сразу доступна по своему
// пользовательскому адресу без обхода через phys_to_virt().
//
// Вытеснение планировщиком не может прервать эту функцию посередине: таймер
// преемптит только когда прерванный код был в ring3 (CS==0x33, см. pit.c) —
// сам syscall-обработчик всегда исполняется в ring0, так что блокировка
// прерываний тут не нужна отдельно.
static uint64_t sys_mmap(uint64_t addr, uint64_t length, uint64_t prot,
                         uint64_t flags, uint64_t fd) {
    (void)addr;
    (void)fd;

    if (!current_process || length == 0)
        return (uint64_t)-1;
    if (!(flags & MAP_ANONYMOUS))
        return (uint64_t)-1;   // файловый mmap не поддерживается
    if (flags & MAP_FIXED)
        return (uint64_t)-1;   // свой адрес в этой версии не учитывается

    uint64_t aligned_len = (length + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t num_pages = aligned_len / PAGE_SIZE;

    int slot = -1;
    for (int i = 0; i < MAX_MMAP_REGIONS; i++) {
        if (current_process->mmap_regions[i].length == 0) { slot = i; break; }
    }
    if (slot < 0)
        return (uint64_t)-1;   // некуда записать новый регион

    uint64_t base = current_process->next_mmap_addr;
    uint64_t pml4_phys = current_process->page_table;

    uint64_t page_flags = PAGE_PRESENT | PAGE_USER;
    if (prot & PROT_WRITE) page_flags |= PAGE_WRITE;
    if (!(prot & PROT_EXEC)) page_flags |= PAGE_NX;

    uint64_t mapped;
    for (mapped = 0; mapped < num_pages; mapped++) {
        uint64_t phys = pmm_alloc_page();
        if (!phys) break;

        uint64_t virt = base + mapped * PAGE_SIZE;
        if (map_page_in_pml4(pml4_phys, virt, phys, page_flags) != 0) {
            pmm_free_page(phys);
            break;
        }

        // Анонимная память обязана приходить обнулённой (POSIX-семантика,
        // на неё будет полагаться malloc() будущей libc).
        memset((void*)virt, 0, PAGE_SIZE);
    }

    if (mapped < num_pages) {
        // Не хватило физической памяти на часть запроса — откатываем то,
        // что уже успели замаппить (unmap_page() сама же освобождает и
        // физическую страницу), а не оставляем недостроенный регион.
        for (uint64_t i = 0; i < mapped; i++) {
            unmap_page(base + i * PAGE_SIZE);
        }
        return (uint64_t)-1;
    }

    current_process->next_mmap_addr += aligned_len;
    current_process->mmap_regions[slot].addr = base;
    current_process->mmap_regions[slot].length = aligned_len;

    return base;
}

// SYS_MUNMAP (10): addr, length — должны ТОЧНО совпадать с ранее
// возвращённым mmap()-регионом целиком (частичный/поддиапазонный unmap, как
// у настоящего munmap(), в этой версии не поддерживается).
static uint64_t sys_munmap(uint64_t addr, uint64_t length,
                           uint64_t unused1, uint64_t unused2, uint64_t unused3) {
    (void)unused1;
    (void)unused2;
    (void)unused3;

    if (!current_process || length == 0)
        return (uint64_t)-1;

    uint64_t aligned_len = (length + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);

    for (int i = 0; i < MAX_MMAP_REGIONS; i++) {
        mmap_region_t *r = &current_process->mmap_regions[i];
        if (r->length != 0 && r->addr == addr && r->length == aligned_len) {
            uint64_t num_pages = aligned_len / PAGE_SIZE;
            for (uint64_t p = 0; p < num_pages; p++) {
                unmap_page(addr + p * PAGE_SIZE);   // освобождает и физ. страницу
            }
            r->addr = 0;
            r->length = 0;
            return 0;
        }
    }
    return (uint64_t)-1;   // точного совпадения не нашлось
}

// SYS_EXEC (11): filename_ptr, argv_ptr, envp_ptr
static uint64_t sys_exec(uint64_t filename_ptr, uint64_t argv_ptr,
                         uint64_t envp_ptr, uint64_t unused1, uint64_t unused2) {
    (void)unused1; (void)unused2;
    if (!current_process) return (uint64_t)-EFAULT;
    if (validate_user_string(current_process->page_table, filename_ptr, USER_STRING_MAX) < 0)
        return (uint64_t)-EFAULT;
    const char *filename = (const char *)filename_ptr;

    char **argv = copy_user_string_array(current_process->page_table, argv_ptr);
    if (!argv) return (uint64_t)-EFAULT;
    char **envp = copy_user_string_array(current_process->page_table, envp_ptr);
    if (!envp) { free_argv_envp(argv, NULL); return (uint64_t)-EFAULT; }

    // do_exec() (и, за ней, elf_exec_replace()) забирает владение argv/envp
    // и освобождает их сама на любом исходе — do_exec() -> elf_exec_replace()
    // не возвращается по этому стеку вызовов при УСПЕХЕ (настоящий
    // execve()), возврат сюда возможен только при ошибке, но освобождать
    // их здесь всё равно НЕ нужно ни в каком случае (уже сделано внутри).
    return (uint64_t)do_exec(filename, argv, envp);
}

// SYS_FORK (12) обрабатывается отдельно в syscall_handler() (см. ниже) —
// ему нужен указатель на весь сохранённый кадр регистров, а не только
// обычные 5 аргументов, поэтому он не попадает в общую syscall_table.

// SYS_WAIT (13): pid (0 = любой ребёнок), status_ptr (может быть 0), options
static uint64_t sys_wait(uint64_t pid, uint64_t status_ptr, uint64_t options,
                         uint64_t unused1, uint64_t unused2) {
    (void)options;
    (void)unused1;
    (void)unused2;

    // Проверяем указатель ДО блокирующего process_wait() — плохой указатель
    // должен провалиться сразу, а не после того, как мы уже дождались
    // ребёнка (и тем более не должен разыменовываться напрямую после).
    if (status_ptr != 0) {
        if (!current_process || !is_user_range_valid(current_process->page_table, status_ptr, sizeof(int), 1))
            return (uint64_t)-EFAULT;
    }

    int status = 0;
    int result = process_wait((uint32_t)pid, &status);

    if (result >= 0 && status_ptr != 0) {
        *(int *)status_ptr = status;
    }

    return (uint64_t)result;
}

// SYS_GETCWD (14): buffer, size — копирует cwd текущего процесса (с NUL) в
// buffer, если влезает. Возвращает длину строки (без NUL) при успехе.
static uint64_t sys_getcwd(uint64_t buffer, uint64_t size,
                           uint64_t unused1, uint64_t unused2, uint64_t unused3) {
    (void)unused1;
    (void)unused2;
    (void)unused3;

    if (!current_process || buffer == 0) return (uint64_t)-EFAULT;
    if (size == 0) return (uint64_t)-EINVAL;
    if (!is_user_range_valid(current_process->page_table, buffer, size, 1))
        return (uint64_t)-EFAULT;

    size_t len = strlen(current_process->cwd_path);
    if (len + 1 > size) return (uint64_t)-ERANGE;

    memcpy((void *)buffer, current_process->cwd_path, len + 1);
    return (uint64_t)len;
}

// SYS_CHDIR (15): path — та же логика, что и command_cd() (kernel/shell/
// commands/filesystem.c), но на current_process->cwd_*, а не на
// шелл-глобалах (которые теперь и есть эти же поля, см. shell.h), и с
// проверкой указателя вместо прямого разыменования.
static uint64_t sys_chdir(uint64_t path_ptr, uint64_t unused1, uint64_t unused2,
                          uint64_t unused3, uint64_t unused4) {
    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;

    if (!current_process) return (uint64_t)-EFAULT;

    int64_t slen = validate_user_string(current_process->page_table, path_ptr, USER_STRING_MAX);
    if (slen < 0) return (uint64_t)-EFAULT;
    if (slen == 0) return (uint64_t)-EINVAL;

    const char *path = (const char *)path_ptr;

    uint32_t new_inode;
    if (lufirafs_lookup(&lufirafs, current_process->cwd_inode, path, &new_inode) != 0)
        return (uint64_t)-ENOENT;

    lufirafs_inode_t inode;
    if (lufirafs_read_inode(&lufirafs, new_inode, &inode) != 0 ||
        inode.mode != LUFIRAFS_MODE_DIR)
        return (uint64_t)-ENOTDIR;

    if (lufirafs_get_path(&lufirafs, new_inode, current_process->cwd_path,
                          sizeof(current_process->cwd_path)) != 0) {
        current_process->cwd_path[0] = '/';
        current_process->cwd_path[1] = '\0';
    }
    current_process->cwd_inode = new_inode;

    return 0;
}

// SYS_SLEEP (16): milliseconds
static uint64_t sys_sleep(uint64_t milliseconds,
                          uint64_t unused1,
                          uint64_t unused2,
                          uint64_t unused3,
                          uint64_t unused4) {
    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;

    if (milliseconds == 0)
        return 0;

    process_sleep(milliseconds);

    return 0;
}

// SYS_KILL (17): pid, sig (0 = SIGTERM по умолчанию)
static uint64_t sys_kill(uint64_t pid,
                         uint64_t sig,
                         uint64_t unused1,
                         uint64_t unused2,
                         uint64_t unused3) {
    (void)unused1;
    (void)unused2;
    (void)unused3;

    if (pid == 0)
        return (uint64_t)-1;

    int signal = sig ? (int)sig : SIGTERM;

    return (uint64_t)process_signal((uint32_t)pid, signal);
}

// SYS_PIPE (18): fds_ptr (указывает на int[2] в памяти вызывающего:
// fds[0] = конец на чтение, fds[1] = конец на запись)
static uint64_t sys_pipe(uint64_t fds_ptr,
                         uint64_t unused1,
                         uint64_t unused2,
                         uint64_t unused3,
                         uint64_t unused4) {
    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;

    if (!current_process || !is_user_range_valid(current_process->page_table, fds_ptr, 2 * sizeof(int), 1))
        return (uint64_t)-EFAULT;

    int fds[2];
    if (vfs_pipe(fds) != 0)
        return (uint64_t)-1;

    int *out = (int *)fds_ptr;
    out[0] = fds[0];
    out[1] = fds[1];

    return 0;
}

// SYS_CHMOD (19): path, mode (напр. 0644) — как sys_chdir, резолвит path
// относительно current_process->cwd_inode. Владелец файла или root.
static uint64_t sys_chmod(uint64_t path_ptr, uint64_t mode, uint64_t unused1,
                          uint64_t unused2, uint64_t unused3) {
    (void)unused1; (void)unused2; (void)unused3;

    if (!current_process) return (uint64_t)-EFAULT;
    int64_t slen = validate_user_string(current_process->page_table, path_ptr, USER_STRING_MAX);
    if (slen < 0) return (uint64_t)-EFAULT;
    if (slen == 0) return (uint64_t)-EINVAL;
    const char *path = (const char *)path_ptr;

    uint32_t ino;
    if (lufirafs_lookup(&lufirafs, current_process->cwd_inode, path, &ino) != 0)
        return (uint64_t)-ENOENT;

    lufirafs_inode_t inode;
    if (lufirafs_read_inode(&lufirafs, ino, &inode) != 0) return (uint64_t)-ENOENT;
    if (current_process->uid != 0 && current_process->uid != inode.uid)
        return (uint64_t)-EPERM;

    inode.perm = (uint32_t)mode & 0777u;
    lufirafs_write_inode(&lufirafs, ino, &inode);
    lufirafs_sync(&lufirafs);
    return 0;
}

// SYS_CHOWN (20): path, uid, gid — только root (без POSIX-нюанса "owner
// может сменить группу на свою собственную").
static uint64_t sys_chown(uint64_t path_ptr, uint64_t new_uid, uint64_t new_gid,
                          uint64_t unused1, uint64_t unused2) {
    (void)unused1; (void)unused2;

    if (!current_process) return (uint64_t)-EFAULT;
    int64_t slen = validate_user_string(current_process->page_table, path_ptr, USER_STRING_MAX);
    if (slen < 0) return (uint64_t)-EFAULT;
    if (slen == 0) return (uint64_t)-EINVAL;
    if (current_process->uid != 0) return (uint64_t)-EPERM;
    const char *path = (const char *)path_ptr;

    uint32_t ino;
    if (lufirafs_lookup(&lufirafs, current_process->cwd_inode, path, &ino) != 0)
        return (uint64_t)-ENOENT;

    lufirafs_inode_t inode;
    if (lufirafs_read_inode(&lufirafs, ino, &inode) != 0) return (uint64_t)-ENOENT;
    inode.uid = (uint32_t)new_uid;
    inode.gid = (uint32_t)new_gid;
    lufirafs_write_inode(&lufirafs, ino, &inode);
    lufirafs_sync(&lufirafs);
    return 0;
}

// SYS_GETUID (21) / SYS_GETGID (22) — без аргументов.
static uint64_t sys_getuid(uint64_t u1, uint64_t u2, uint64_t u3, uint64_t u4, uint64_t u5) {
    (void)u1; (void)u2; (void)u3; (void)u4; (void)u5;
    if (!current_process) return (uint64_t)-EFAULT;
    return current_process->uid;
}
static uint64_t sys_getgid(uint64_t u1, uint64_t u2, uint64_t u3, uint64_t u4, uint64_t u5) {
    (void)u1; (void)u2; (void)u3; (void)u4; (void)u5;
    if (!current_process) return (uint64_t)-EFAULT;
    return current_process->gid;
}

// SYS_MKDIR (23): path, mode (пока игнорируется — LufiraFS даёт новым
// директориям фиксированный LUFIRAFS_DEFAULT_DIR_PERM; параметр принят
// только ради совместимости сигнатуры с POSIX mkdir(2), как уже сделано у
// SYS_OPEN-а mode). Резолвит path относительно current_process->cwd_inode —
// тот же выбор, что уже сделан у SYS_CHDIR/SYS_CHMOD/SYS_CHOWN выше, а не у
// более старых SYS_OPEN/SYS_EXEC (те всегда идут от корня, см. комментарий
// вверху lufirafs_vfs.c). Эти четыре новых syscall'а раньше просто не
// существовали — vfs_mkdir()/vfs_rmdir()/vfs_unlink()/vfs_readdir() (vfs.h)
// уже были реализованы и рабочи, но не были доступны из ring3 ни одним
// syscall'ом.
static uint64_t sys_mkdir(uint64_t path_ptr, uint64_t mode, uint64_t unused1,
                          uint64_t unused2, uint64_t unused3) {
    (void)mode; (void)unused1; (void)unused2; (void)unused3;

    if (!current_process) return (uint64_t)-EFAULT;
    int64_t slen = validate_user_string(current_process->page_table, path_ptr, USER_STRING_MAX);
    if (slen < 0) return (uint64_t)-EFAULT;
    if (slen == 0) return (uint64_t)-EINVAL;
    const char *path = (const char *)path_ptr;

    // Право на создание записи проверяется на РОДИТЕЛЬСКОЙ директории
    // (write+exec), не на самом path — его ещё не существует. Тот же
    // паттерн, что уже используется в SYS_OPEN-е для O_CREAT выше.
    uint32_t parent;
    char leaf[LUFIRAFS_MAX_NAME + 1];
    if (lufirafs_resolve_parent(&lufirafs, current_process->cwd_inode, path, &parent, leaf) != 0)
        return (uint64_t)-ENOENT;

    lufirafs_inode_t pinode;
    if (lufirafs_read_inode(&lufirafs, parent, &pinode) != 0)
        return (uint64_t)-ENOENT;
    if (!lufirafs_check_access(&pinode, current_process->uid, current_process->gid, 0, 1, 1))
        return (uint64_t)-EACCES;

    int res = vfs_mkdir_at(current_process->cwd_inode, path);
    return (res == 0) ? 0 : (uint64_t)-1;
}

// SYS_RMDIR (24) / SYS_UNLINK (25): path — LufiraFS не различает "удалить
// файл" и "удалить директорию" на уровне lufirafs_unlink() (см.
// vfs_rmdir()/vfs_unlink() в vfs.c — обе зовут один и тот же
// vfs_lufirafs_unlink()), так что оба syscall'а идут через один и тот же
// статический хелпер; типовая проверка (файл это или директория) —
// не задача этого фундамента, см. план.
static uint64_t sys_remove(uint64_t path_ptr, int is_rmdir) {
    if (!current_process) return (uint64_t)-EFAULT;
    int64_t slen = validate_user_string(current_process->page_table, path_ptr, USER_STRING_MAX);
    if (slen < 0) return (uint64_t)-EFAULT;
    if (slen == 0) return (uint64_t)-EINVAL;
    const char *path = (const char *)path_ptr;

    uint32_t parent;
    char leaf[LUFIRAFS_MAX_NAME + 1];
    if (lufirafs_resolve_parent(&lufirafs, current_process->cwd_inode, path, &parent, leaf) != 0)
        return (uint64_t)-ENOENT;

    lufirafs_inode_t pinode;
    if (lufirafs_read_inode(&lufirafs, parent, &pinode) != 0)
        return (uint64_t)-ENOENT;
    if (!lufirafs_check_access(&pinode, current_process->uid, current_process->gid, 0, 1, 1))
        return (uint64_t)-EACCES;

    int res = is_rmdir ? vfs_rmdir_at(current_process->cwd_inode, path)
                        : vfs_unlink_at(current_process->cwd_inode, path);
    return (res == 0) ? 0 : (uint64_t)-1;
}

static uint64_t sys_rmdir(uint64_t path_ptr, uint64_t unused1, uint64_t unused2,
                          uint64_t unused3, uint64_t unused4) {
    (void)unused1; (void)unused2; (void)unused3; (void)unused4;
    return sys_remove(path_ptr, 1);
}

static uint64_t sys_unlink(uint64_t path_ptr, uint64_t unused1, uint64_t unused2,
                           uint64_t unused3, uint64_t unused4) {
    (void)unused1; (void)unused2; (void)unused3; (void)unused4;
    return sys_remove(path_ptr, 0);
}

// SYS_READDIR (26): fd, buf_ptr (указывает на vfs_dirent_t в памяти
// вызывающего). Права не проверяются отдельно — fd уже прошёл проверку
// доступа на чтение при открытии (SYS_OPEN выше).
static uint64_t sys_readdir(uint64_t fd, uint64_t buf_ptr, uint64_t unused1,
                            uint64_t unused2, uint64_t unused3) {
    (void)unused1; (void)unused2; (void)unused3;

    if (!current_process ||
        !is_user_range_valid(current_process->page_table, buf_ptr, sizeof(vfs_dirent_t), 1))
        return (uint64_t)-EFAULT;

    return (uint64_t)vfs_readdir((int)fd, (void *)buf_ptr);
}

// ========== ТАБЛИЦА СИСТЕМНЫХ ВЫЗОВОВ ==========

static syscall_fn_t syscall_table[256] = {
    [SYS_WRITE]   = sys_write,
    [SYS_READ]    = sys_read,
    [SYS_EXIT]    = sys_exit,
    [SYS_GETPID]  = sys_getpid,
    [SYS_YIELD]   = sys_yield,
    [SYS_GETTICK] = sys_gettick,
    [SYS_OPEN]    = sys_open,
    [SYS_CLOSE]   = sys_close,
    [SYS_SEEK]    = sys_seek,
    [SYS_MMAP]    = sys_mmap,
    [SYS_MUNMAP]  = sys_munmap,
    [SYS_EXEC]    = sys_exec,
    // SYS_FORK намеренно не в этой таблице — см. syscall_handler().
    [SYS_WAIT]    = sys_wait,
    [SYS_GETCWD]  = sys_getcwd,
    [SYS_CHDIR]   = sys_chdir,
    [SYS_SLEEP]   = sys_sleep,
    [SYS_KILL]    = sys_kill,
    [SYS_PIPE]    = sys_pipe,
    [SYS_CHMOD]   = sys_chmod,
    [SYS_CHOWN]   = sys_chown,
    [SYS_GETUID]  = sys_getuid,
    [SYS_GETGID]  = sys_getgid,
    [SYS_MKDIR]   = sys_mkdir,
    [SYS_RMDIR]   = sys_rmdir,
    [SYS_UNLINK]  = sys_unlink,
    [SYS_READDIR] = sys_readdir,
};

// ========== ИНИЦИАЛИЗАЦИЯ ==========

void syscall_init(void) {
    // STAR MSR: kernel CS (47:32) и user CS (63:48)
    uint64_t star = ((uint64_t)GDT_KERNEL_CODE << 32) | 
                    ((uint64_t)0x30 << 48);
    asm volatile("wrmsr" : : "c"(0xC0000081), "a"((uint32_t)star), 
                 "d"((uint32_t)(star >> 32)));
    
    // LSTAR MSR: адрес syscall_entry
    extern void syscall_entry(void);
    uint64_t handler = (uint64_t)&syscall_entry;
    asm volatile("wrmsr" : : "c"(0xC0000082), "a"((uint32_t)handler),
                 "d"((uint32_t)(handler >> 32)));
    
    // FMASK MSR: очищаем IF при входе
    uint64_t fmask = 0x200;
    asm volatile("wrmsr" : : "c"(0xC0000084), "a"((uint32_t)fmask),
                 "d"((uint32_t)(fmask >> 32)));
    
    // EFER: включаем SCE
    uint32_t efer_low, efer_high;
    asm volatile("rdmsr" : "=a"(efer_low), "=d"(efer_high) : "c"(0xC0000080));
    uint64_t efer = ((uint64_t)efer_high << 32) | efer_low;
    efer |= 1;
    asm volatile("wrmsr" : : "c"(0xC0000080), "a"((uint32_t)efer),
                 "d"((uint32_t)(efer >> 32)));
    
    DLOG("[SYSCALL] 27 system calls registered\n");
}

// ========== ДИСПАТЧЕР ==========

uint64_t syscall_handler(uint64_t syscall_num, uint64_t arg1, uint64_t arg2,
                         uint64_t arg3, uint64_t arg4, uint64_t arg5,
                         uint64_t frame_ptr) {
    // SYS_FORK — особый случай: ему нужен указатель на весь сохранённый
    // кадр регистров (rip/rflags/callee-saved), а не только 5 обычных
    // аргументов, поэтому он обрабатывается до общей таблицы диспетчера.
    if (syscall_num == SYS_FORK) {
        return process_fork(frame_ptr);
    }

    if (syscall_num >= 256 || !syscall_table[syscall_num]) {
        printf("[SYSCALL] Unknown: %u\n", (uint32_t)syscall_num);
        return (uint64_t)-1;
    }

    return syscall_table[syscall_num](arg1, arg2, arg3, arg4, arg5);
}
