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
// шеллом (команда "exec"), и системным вызовом SYS_EXEC.
int do_exec(const char *filename) {
    if (!filename || !*filename) return -1;

    int fd = vfs_open(filename, O_RDONLY);
    if (fd < 0) {
        printf("[EXEC] Failed to open %s\n", filename);
        return -1;
    }

    file_t *f = current_fd_table->files[fd];
    if (!f || !f->inode) {
        vfs_close(fd);
        return -1;
    }
    uint32_t size = f->inode->size;
    if (size == 0) {
        vfs_close(fd);
        return -1;
    }

    uint8_t *buf = (uint8_t *)kmalloc(size);
    if (!buf) {
        vfs_close(fd);
        return -1;
    }

    int bytes_read = vfs_read(fd, buf, size);
    vfs_close(fd);
    if (bytes_read != (int)size) {
        kfree(buf);
        return -1;
    }

    // elf_exec_replace() освобождает buf при любом исходе (успех или
    // неудача), поэтому здесь его повторно не освобождаем.
    return elf_exec_replace(buf, size, filename);
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

    return (uint64_t)vfs_open((const char *)filename_ptr, (int)flags);
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
    (void)argv_ptr; (void)envp_ptr; (void)unused1; (void)unused2;
    if (!current_process) return (uint64_t)-EFAULT;
    if (validate_user_string(current_process->page_table, filename_ptr, USER_STRING_MAX) < 0)
        return (uint64_t)-EFAULT;
    const char *filename = (const char *)filename_ptr;

    // do_exec() -> elf_exec_replace() не возвращается по этому стеку
    // вызовов при успехе (настоящий execve()) — возврат сюда возможен
    // только при ошибке.
    return (uint64_t)do_exec(filename);
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
    
    DLOG("[SYSCALL] 19 system calls registered\n");
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
