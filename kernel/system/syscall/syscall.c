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
#include "system/users/users.h"
#include "fs/fat/fat_mount.h"
#include "system/acpi/acpi.h"
#include "drivers/usb/xhci.h"

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

// Проверяет, что addr указывает на NUL-терминированную строку (<=max_len
// байт без терминатора), целиком в читаемой памяти текущего процесса —
// is_user_accessible() проверяется на каждой впервые пересечённой странице
// перед разыменованием. pml4_phys обязан быть активным CR3 (вызывается
// только из обработчиков syscall'ов, всегда под CR3 вызывающего). Возвращает
// длину строки при успехе, -1 при невалидном адресе или отсутствии '\0'.
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

// Копирует NUL-терминированный массив указателей на строки (argv[]/envp[]
// -стиль) из пользовательской памяти текущего процесса в kernel-side буфер
// (kmalloc на массив указателей + отдельный kmalloc на каждую строку) — в
// форме, которую elf_exec_replace()/free_argv_envp() ожидают/освобождают.
// array_ptr==0 трактуется как argc=0, а не ошибка. Возвращает NULL при любой
// другой ошибке (плохой указатель, больше MAX_EXEC_ARGS элементов, слишком
// длинная строка) — без частично выделенного состояния.
//
// ВАЖНО для вызывающих SYS_EXEC/SYS_FORK+SYS_EXEC из userspace (v0.7 план,
// этап 5, под-этап 6 — найдено при написании первого прямого вызывателя
// SYS_EXEC вне кернел-нативного шелла): is_user_range_valid() ниже
// проверяет ФИКСИРОВАННЫЙ диапазон (MAX_EXEC_ARGS+1)*8 байт от array_ptr,
// а не только до фактического NULL-терминатора — так дешевле (не нужно
// сначала безопасно прочитать переменную длину, чтобы узнать, сколько
// проверять). Небольшой argv[] как ЛОКАЛЬНАЯ переменная на стеке (а не
// static/global) может оказаться слишком близко к верху 16KB
// пользовательского стека (USER_STACK_SIZE, process.h) — тогда этот
// фиксированный диапазон вылетает за пределы замапленной страницы и
// is_user_range_valid() честно возвращает отказ (-EFAULT), даже если
// реальный, короткий argv[] с NULL-терминатором сам по себе целиком в
// пределах маппинга. Единственный практичный способ обойти это на стороне
// вызывающего — держать argv[]/envp[] в static/global памяти (.data/.bss,
// свой собственный маппинг с большим запасом), а не в кадре стека — так и
// стоит делать будущему shell.elf.
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
    
    // "[pid] Exit(code)" — только в devmode (DLOG), чтобы не засорять вывод
    // обычному пользователю: shell.elf сам печатает код выхода программы,
    // когда это реально нужно (например wait), это сообщение — чисто
    // отладочное, видно каждый раз, когда ЛЮБОЙ процесс завершается.
    DLOG("\n[%u] Exit(%u)\n",
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
// Только анонимная память (MAP_ANONYMOUS обязателен, addr/fd игнорируются,
// MAP_FIXED не поддерживается); файловый mmap — будущая задача. Выделение
// "eager": страницы физически выделяются и маппятся прямо здесь, а не по
// требованию через page fault — обработчик page fault безусловно
// останавливает систему на любом фолте, реального пути восстановления для
// demand paging нет. Вызывается под собственным CR3 процесса, поэтому
// свежесмапленная страница сразу доступна без phys_to_virt(). Таймер
// преемптит только ring3-код (CS==0x33) — syscall-обработчик всегда в ring0,
// так что отдельная блокировка прерываний здесь не нужна.
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
// Смонтированный FAT (/mnt/...) в cwd — у него нет настоящего lufirafs-
// inode (см. fat_mount.h), так что cwd_inode не может хранить на него
// ссылку как обычно. LUFIRAFS_FAT_MOUNT_CWD_INODE — заведомо невалидный
// номер инода (lufirafs_read_inode()/lufirafs_lookup() отвергают любой
// ino > sb.inode_count, а реальных инодов на 16MB-образе всегда разы
// меньше UINT32_MAX), безопасный как часовой: случайная относительная
// операция (mkdir/lufirafs_lookup и т.п.) с ним просто вернёт ENOENT, а
// не прочитает мусор. Источник истины при этом — cwd_path (строка),
// ровно как и для настоящего lufirafs-cwd.
#define LUFIRAFS_FAT_MOUNT_CWD_INODE 0xFFFFFFFFu

// SYS_CHDIR (15): path — см. комментарий у pathutil.h (userspace/common/
// pathutil.h): "cd" — единственная команда, которая отправляет сюда СЫРОЙ
// (возможно относительный) аргумент пользователя, не склеенный заранее с
// cwd (все остальные пакеты сначала резолвят путь в абсолютный через
// resolve_path()+SYS_GETCWD). Поэтому, в отличие от sys_mkdir()/sys_remove()
// выше, здесь нужно самим обрабатывать и случай "уже стоим в смонтированном
// FAT, относительный '..'".
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

    // Смонтированный FAT — ДО LufiraFS, тот же приоритет, что уже у
    // vfs_open()/sys_mkdir()/sys_remove() (см. их комментарии): найдено
    // при живом тестировании ("mount 0 /mnt/a" успевает, но "cd /mnt/a"
    // отвечает ENOENT) — sys_chdir() был единственным путём, который так
    // и не получил этот фикс при VFS-интеграции монтирования.
    if (path[0] == '/') {
        struct inode *fat_inode = vfs_fat_lookup(path);
        if (fat_inode) {
            int is_dir = (fat_inode->type == FT_DIRECTORY);
            if (fat_inode->private_data) kfree(fat_inode->private_data);
            kfree(fat_inode);
            if (!is_dir) return (uint64_t)-ENOTDIR;

            int n = 0;
            while (path[n] && n < (int)sizeof(current_process->cwd_path) - 1) {
                current_process->cwd_path[n] = path[n];
                n++;
            }
            current_process->cwd_path[n] = '\0';
            current_process->cwd_inode = LUFIRAFS_FAT_MOUNT_CWD_INODE;
            return 0;
        }
    }

    // ".."/"." из уже смонтированного FAT (нет настоящего lufirafs-inode,
    // которому можно было бы задать такой относительный вопрос) — строим
    // родительский путь прямо из cwd_path и уходим в обычную lufirafs-ветку
    // ниже с ним вместо исходного относительного path.
    if (current_process->cwd_inode == LUFIRAFS_FAT_MOUNT_CWD_INODE) {
        if (strcmp(path, ".") == 0) return 0;

        if (strcmp(path, "..") != 0) return (uint64_t)-ENOENT;

        char parent[sizeof(current_process->cwd_path)];
        int len = (int)strlen(current_process->cwd_path);
        int last_slash = -1;
        for (int i = 0; i < len; i++) if (current_process->cwd_path[i] == '/') last_slash = i;

        if (last_slash <= 0) {
            parent[0] = '/'; parent[1] = '\0';
        } else {
            int i = 0;
            for (; i < last_slash; i++) parent[i] = current_process->cwd_path[i];
            parent[i] = '\0';
        }

        // "mount" регистрирует только сам префикс (vfs_fat_mount()), а не
        // создаёт его родителя в lufirafs — найдено тем же живым
        // тестированием: "mount 0 /mnt/a" без предварительного "mkdir /mnt"
        // (ничего этого не требует) оставляет "/mnt" вообще не
        // существующим в lufirafs. Раз пользователь уже пришёл "снаружи"
        // (строка "/mnt/a" была так или иначе набрана), откат на корень —
        // безопасный и предсказуемый край, лучше чем ENOENT на самое
        // обычное действие "выйти из флешки".
        uint32_t new_inode = lufirafs.sb.root_inode;
        lufirafs_lookup(&lufirafs, lufirafs.sb.root_inode, parent, &new_inode);

        lufirafs_inode_t inode;
        if (lufirafs_read_inode(&lufirafs, new_inode, &inode) != 0 ||
            inode.mode != LUFIRAFS_MODE_DIR)
            new_inode = lufirafs.sb.root_inode;

        if (lufirafs_get_path(&lufirafs, new_inode, current_process->cwd_path,
                              sizeof(current_process->cwd_path)) != 0) {
            current_process->cwd_path[0] = '/';
            current_process->cwd_path[1] = '\0';
        }
        current_process->cwd_inode = new_inode;
        return 0;
    }

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

// SYS_MKDIR (23): path, mode (пока игнорируется — новые директории всегда
// получают LUFIRAFS_DEFAULT_DIR_PERM; параметр принят только ради
// совместимости с POSIX mkdir(2)). Резолвит path относительно cwd_inode, как
// SYS_CHDIR/SYS_CHMOD/SYS_CHOWN — в отличие от более старых SYS_OPEN/SYS_EXEC,
// которые всегда идут от корня (см. lufirafs_vfs.c).
static uint64_t sys_mkdir(uint64_t path_ptr, uint64_t mode, uint64_t unused1,
                          uint64_t unused2, uint64_t unused3) {
    (void)mode; (void)unused1; (void)unused2; (void)unused3;

    if (!current_process) return (uint64_t)-EFAULT;
    int64_t slen = validate_user_string(current_process->page_table, path_ptr, USER_STRING_MAX);
    if (slen < 0) return (uint64_t)-EFAULT;
    if (slen == 0) return (uint64_t)-EINVAL;
    const char *path = (const char *)path_ptr;

    // Смонтированный FAT — ДО всех проверок прав на LufiraFS: путь вообще
    // не существует как lufirafs-inode, lufirafs_resolve_parent() ниже
    // всегда вернёт -ENOENT для него (найдено при тестировании VFS-
    // интеграции монтирования, v0.7 план, этап 5, под-этап 6 — mkdir.elf
    // звал именно SYS_MKDIR, а не vfs_mkdir()/vfs_mkdir_at() напрямую, так
    // что более ранний фикс в vfs.c сюда просто не доходил).
    if (path[0] == '/') {
        int r = vfs_fat_mkdir(path);
        if (r != -2) return (r == 0) ? 0 : (uint64_t)-1;
    }

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

    // Смонтированный FAT — см. тот же комментарий в sys_mkdir() выше.
    if (path[0] == '/') {
        int r = vfs_fat_unlink(path);
        if (r != -2) return (r == 0) ? 0 : (uint64_t)-1;
    }

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

// SYS_STATFS (27): buf_ptr -> lufira_statfs_t. То же самое, что уже читает
// напрямую command_df() (kernel/shell/commands/filesystem.c) из
// lufirafs.sb — первый выход этих полей за пределы ядра (v0.7, этап 1).
static uint64_t sys_statfs(uint64_t buf_ptr, uint64_t unused1, uint64_t unused2,
                           uint64_t unused3, uint64_t unused4) {
    (void)unused1; (void)unused2; (void)unused3; (void)unused4;

    if (!current_process ||
        !is_user_range_valid(current_process->page_table, buf_ptr, sizeof(lufira_statfs_t), 1))
        return (uint64_t)-EFAULT;

    lufira_statfs_t out;
    out.block_size = lufirafs.sb.block_size;
    out.total_blocks = lufirafs.sb.total_blocks;
    out.free_blocks = lufirafs.sb.free_blocks;
    out.inode_count = lufirafs.sb.inode_count;
    out.free_inodes = lufirafs.sb.free_inodes;
    memcpy((void *)buf_ptr, &out, sizeof(out));
    return 0;
}

// SYS_MEMINFO (28): buf_ptr -> lufira_meminfo_t. То же, что command_free()
// (system.c) — pmm_get_total_pages()/pmm_get_used_pages() (pmm.c) +
// heap_get_stats() (heap.c), обе добавлены в 0.6.5 для той же команды.
static uint64_t sys_meminfo(uint64_t buf_ptr, uint64_t unused1, uint64_t unused2,
                            uint64_t unused3, uint64_t unused4) {
    (void)unused1; (void)unused2; (void)unused3; (void)unused4;

    if (!current_process ||
        !is_user_range_valid(current_process->page_table, buf_ptr, sizeof(lufira_meminfo_t), 1))
        return (uint64_t)-EFAULT;

    lufira_meminfo_t out;
    out.total_pages = pmm_get_total_pages();
    out.used_pages = pmm_get_used_pages();
    uint64_t heap_used, heap_free;
    heap_get_stats(&heap_used, &heap_free);
    out.heap_total_bytes = heap_used + heap_free;
    out.heap_used_bytes = heap_used;
    memcpy((void *)buf_ptr, &out, sizeof(out));
    return 0;
}

// SYS_CPULOAD (29): buf_ptr -> lufira_cpuload_t. Сырые тики PIT
// (pit_get_total_ticks()/pit_get_idle_ticks(), pit.c) — вызывающая
// сторона сама берёт два снимка с sys_msleep() между ними и считает %,
// как уже делает kernel-native command_cpuload() (system.c).
static uint64_t sys_cpuload(uint64_t buf_ptr, uint64_t unused1, uint64_t unused2,
                            uint64_t unused3, uint64_t unused4) {
    (void)unused1; (void)unused2; (void)unused3; (void)unused4;

    if (!current_process ||
        !is_user_range_valid(current_process->page_table, buf_ptr, sizeof(lufira_cpuload_t), 1))
        return (uint64_t)-EFAULT;

    lufira_cpuload_t out;
    out.total_ticks = pit_get_total_ticks();
    out.idle_ticks = pit_get_idle_ticks();
    memcpy((void *)buf_ptr, &out, sizeof(out));
    return 0;
}

// SYS_PSLIST (30): buf_ptr -> lufira_ps_entry_t[max_count], max_count —
// снимок ВСЕХ живых процессов одним вызовом (process_pslist(), process.c) —
// v0.7 план, этап 5, под-этап 4 ("ps"), единственная новая точка входа во
// всём этапе. max_count == 0 — вырожденный, но валидный случай (узнать
// нечего, буфер не нужен); is_user_range_valid() тогда не зовём вовсе,
// т.к. buf_ptr в этом случае может быть и NULL.
static uint64_t sys_pslist(uint64_t buf_ptr, uint64_t max_count, uint64_t unused1,
                           uint64_t unused2, uint64_t unused3) {
    (void)unused1; (void)unused2; (void)unused3;

    if (!current_process) return (uint64_t)-EFAULT;
    if (max_count == 0) return 0;
    if (!is_user_range_valid(current_process->page_table, buf_ptr,
                              max_count * sizeof(lufira_ps_entry_t), 1))
        return (uint64_t)-EFAULT;

    return (uint64_t)process_pslist((lufira_ps_entry_t *)buf_ptr, (uint32_t)max_count);
}

// SYS_SET_FOREGROUND (31): pid (0 — снять). См. комментарий в syscall.h —
// v0.7 план, этап 5, под-этап 6.
static uint64_t sys_set_foreground(uint64_t pid, uint64_t unused1, uint64_t unused2,
                                   uint64_t unused3, uint64_t unused4) {
    (void)unused1; (void)unused2; (void)unused3; (void)unused4;

    if (!current_process) return (uint64_t)-EFAULT;
    int res = process_set_foreground(current_process->pid, (uint32_t)pid);
    return (res == 0) ? 0 : (uint64_t)-1;
}

// SYS_SU (32): см. комментарий в syscall.h. username ограничен 32 байтами
// (размер username[] в user_entry_t) — более длинная строка без '\0' в
// этом диапазоне честно отбраковывается validate_user_string() как
// невалидная, искать её по users_lookup_by_name() смысла нет.
static uint64_t sys_su(uint64_t username_ptr, uint64_t password_ptr, uint64_t unused1,
                       uint64_t unused2, uint64_t unused3) {
    (void)unused1; (void)unused2; (void)unused3;

    if (!current_process) return (uint64_t)-EFAULT;

    int64_t ulen = validate_user_string(current_process->page_table, username_ptr, 32);
    if (ulen <= 0) return (uint64_t)-EFAULT;
    int64_t plen = validate_user_string(current_process->page_table, password_ptr, USER_STRING_MAX);
    if (plen < 0) return (uint64_t)-EFAULT;

    const char *username = (const char *)username_ptr;
    const char *password = (const char *)password_ptr;

    user_entry_t u;
    if (users_lookup_by_name(username, &u) != 0) return (uint64_t)-ENOENT;

    if (current_process->uid != 0 && !users_check_password(username, password))
        return (uint64_t)-EPERM;

    current_process->uid = u.uid;
    current_process->gid = u.gid;
    return 0;
}

// SYS_MOUNT (33) / SYS_UNMOUNT (34): см. комментарии в syscall.h. Просто
// тонкие обёртки — вся логика (включая проверку usb-устройства, чтение
// образа, real-time синк после записи) уже в vfs_fat_mount()/
// vfs_fat_unmount() (fat_mount.c), т.к. её нужно звать и из vfs.c (open/
// mkdir/unlink на уже смонтированном пути), не только отсюда.
static uint64_t sys_mount(uint64_t prefix_ptr, uint64_t usb_index, uint64_t unused1,
                          uint64_t unused2, uint64_t unused3) {
    (void)unused1; (void)unused2; (void)unused3;
    if (!current_process) return (uint64_t)-EFAULT;

    int64_t slen = validate_user_string(current_process->page_table, prefix_ptr, USER_STRING_MAX);
    if (slen <= 0) return (uint64_t)-EFAULT;

    int res = vfs_fat_mount((int)usb_index, (const char *)prefix_ptr);
    return (uint64_t)(int64_t)res;
}

static uint64_t sys_unmount(uint64_t prefix_ptr, uint64_t unused1, uint64_t unused2,
                            uint64_t unused3, uint64_t unused4) {
    (void)unused1; (void)unused2; (void)unused3; (void)unused4;
    if (!current_process) return (uint64_t)-EFAULT;

    int64_t slen = validate_user_string(current_process->page_table, prefix_ptr, USER_STRING_MAX);
    if (slen <= 0) return (uint64_t)-EFAULT;

    int res = vfs_fat_unmount((const char *)prefix_ptr);
    return (uint64_t)(int64_t)res;
}

// SYS_REBOOT (35) / SYS_SHUTDOWN (36) — см. комментарии в syscall.h.
// Прямой перенос command_reboot()/command_shutdown() (kernel/shell/
// commands/system.c, мёртвый код) без изменений в самой логике сброса —
// только root и синк диска гейтятся тут, а не в выводе на консоль (тот
// был смыслом для интерактивного шелла, не для syscall'а).
static uint64_t sys_reboot(uint64_t unused1, uint64_t unused2, uint64_t unused3,
                           uint64_t unused4, uint64_t unused5) {
    (void)unused1; (void)unused2; (void)unused3; (void)unused4; (void)unused5;
    if (!current_process) return (uint64_t)-EFAULT;
    if (current_process->uid != 0) return (uint64_t)-EPERM;

    lufirafs_flush(&lufirafs);
    __asm__ volatile ("outb %0, %1" : : "a"((uint8_t)0xFE), "Nd"((uint16_t)0x64));
    __asm__ volatile ("outw %0, %1" : : "a"((uint16_t)0x2000), "Nd"((uint16_t)0x604));
    return (uint64_t)-1; // не должны сюда дойти, если сброс сработал
}

static uint64_t sys_shutdown(uint64_t unused1, uint64_t unused2, uint64_t unused3,
                             uint64_t unused4, uint64_t unused5) {
    (void)unused1; (void)unused2; (void)unused3; (void)unused4; (void)unused5;
    if (!current_process) return (uint64_t)-EFAULT;
    if (current_process->uid != 0) return (uint64_t)-EPERM;

    lufirafs_flush(&lufirafs);
    acpi_shutdown();
    return (uint64_t)-1; // ACPI shutdown не сработал
}

// SYS_DEVMODE (37): mode (0=прочитать, 1=включить, 2=выключить) — см.
// комментарий в syscall.h.
static uint64_t sys_devmode(uint64_t mode, uint64_t unused1, uint64_t unused2,
                            uint64_t unused3, uint64_t unused4) {
    (void)unused1; (void)unused2; (void)unused3; (void)unused4;
    switch (mode) {
        case 0: return (uint64_t)devmode_is_enabled();
        case 1: return (devmode_set(1) == 0) ? 0 : (uint64_t)-1;
        case 2: return (devmode_set(0) == 0) ? 0 : (uint64_t)-1;
        default: return (uint64_t)-EINVAL;
    }
}

// Ограниченное копирование строки (ёмкость dest_size, последний байт
// всегда под '\0') — своя копия copy_bounded() из users.c (тоже static,
// не экспортирована).
static void copy_bounded_path(char *dest, const char *src, int dest_size) {
    int i = 0;
    while (i < dest_size - 1 && src[i]) { dest[i] = src[i]; i++; }
    dest[i] = '\0';
}

// Создаёт /home (если его ещё нет) и /home/<username> внутри него, owner —
// сам новый пользователь, perm 0700 — своя копия ensure_home_dir() из
// kernel/shell/commands/users.c (мёртвый код, не трогается и не
// экспортирует свои статические хелперы — тот же приём, что уже у
// fat_mount.c с mount.c). При любой неудаче тихо откатывается на "/".
static void syscall_ensure_home_dir(uint32_t uid, uint32_t gid, const char *username,
                                     char *out_home, int out_home_size) {
    uint32_t home_root_ino;
    if (lufirafs_lookup(&lufirafs, lufirafs.sb.root_inode, "/home", &home_root_ino) != 0) {
        if (lufirafs_create(&lufirafs, lufirafs.sb.root_inode, "home", LUFIRAFS_MODE_DIR,
                             0, 0, LUFIRAFS_DEFAULT_DIR_PERM, &home_root_ino) != 0) {
            copy_bounded_path(out_home, "/", out_home_size);
            return;
        }
        lufirafs_sync(&lufirafs);
    }

    uint32_t user_home_ino;
    if (lufirafs_lookup(&lufirafs, home_root_ino, username, &user_home_ino) != 0) {
        if (lufirafs_create(&lufirafs, home_root_ino, username, LUFIRAFS_MODE_DIR,
                             uid, gid, 0700, &user_home_ino) != 0) {
            copy_bounded_path(out_home, "/", out_home_size);
            return;
        }
        lufirafs_sync(&lufirafs);
    }

    int pos = 0;
    const char *prefix = "/home/";
    while (prefix[pos] && pos < out_home_size - 1) { out_home[pos] = prefix[pos]; pos++; }
    int i = 0;
    while (username[i] && pos < out_home_size - 1) { out_home[pos++] = username[i++]; }
    out_home[pos] = '\0';
}

// SYS_USERADD (38): username_ptr, password_ptr, group_ptr (0 — своя группа
// с именем пользователя). Root-only. См. комментарий в syscall.h.
static uint64_t sys_useradd(uint64_t username_ptr, uint64_t password_ptr, uint64_t group_ptr,
                            uint64_t unused1, uint64_t unused2) {
    (void)unused1; (void)unused2;
    if (!current_process) return (uint64_t)-EFAULT;
    if (current_process->uid != 0) return (uint64_t)-EPERM;

    int64_t ulen = validate_user_string(current_process->page_table, username_ptr, USER_STRING_MAX);
    if (ulen <= 0) return (uint64_t)-EFAULT;
    int64_t plen = validate_user_string(current_process->page_table, password_ptr, USER_STRING_MAX);
    if (plen < 0) return (uint64_t)-EFAULT;

    const char *username = (const char *)username_ptr;
    const char *password = (const char *)password_ptr;
    const char *groupname = NULL;
    if (group_ptr != 0) {
        int64_t glen = validate_user_string(current_process->page_table, group_ptr, USER_STRING_MAX);
        if (glen < 0) return (uint64_t)-EFAULT;
        if (glen > 0) groupname = (const char *)group_ptr;
    }

    if (users_lookup_by_name(username, NULL) == 0) return (uint64_t)-1; // уже существует

    uint32_t gid;
    if (groupname) {
        group_entry_t g;
        if (groups_lookup_by_name(groupname, &g) != 0) return (uint64_t)-ENOENT;
        gid = g.gid;
    } else {
        gid = groups_next_free_gid();
        if (groups_add(username, gid) != 0) return (uint64_t)-1;
    }

    uint32_t uid = users_next_free_uid();
    char home[64];
    syscall_ensure_home_dir(uid, gid, username, home, sizeof(home));

    if (users_add(username, uid, gid, password, home) != 0) return (uint64_t)-1;
    return 0;
}

// SYS_GROUPADD (39): groupname_ptr. Root-only.
static uint64_t sys_groupadd(uint64_t groupname_ptr, uint64_t unused1, uint64_t unused2,
                             uint64_t unused3, uint64_t unused4) {
    (void)unused1; (void)unused2; (void)unused3; (void)unused4;
    if (!current_process) return (uint64_t)-EFAULT;
    if (current_process->uid != 0) return (uint64_t)-EPERM;

    int64_t glen = validate_user_string(current_process->page_table, groupname_ptr, USER_STRING_MAX);
    if (glen <= 0) return (uint64_t)-EFAULT;

    const char *groupname = (const char *)groupname_ptr;
    if (groups_lookup_by_name(groupname, NULL) == 0) return (uint64_t)-1; // уже существует

    uint32_t gid = groups_next_free_gid();
    return (groups_add(groupname, gid) == 0) ? 0 : (uint64_t)-1;
}

// SYS_PASSWD (40): username_ptr (0 — свой собственный пароль, без проверки
// прав), new_password_ptr. Ненулевой username_ptr — root-only.
static uint64_t sys_passwd(uint64_t username_ptr, uint64_t new_password_ptr, uint64_t unused1,
                           uint64_t unused2, uint64_t unused3) {
    (void)unused1; (void)unused2; (void)unused3;
    if (!current_process) return (uint64_t)-EFAULT;

    int64_t plen = validate_user_string(current_process->page_table, new_password_ptr, USER_STRING_MAX);
    if (plen < 0) return (uint64_t)-EFAULT;
    const char *new_password = (const char *)new_password_ptr;

    const char *target_name;
    char self_name[32];
    if (username_ptr == 0) {
        user_entry_t self;
        if (users_lookup_by_uid(current_process->uid, &self) != 0) return (uint64_t)-ENOENT;
        copy_bounded_path(self_name, self.username, sizeof(self_name));
        target_name = self_name;
    } else {
        if (current_process->uid != 0) return (uint64_t)-EPERM;
        int64_t ulen = validate_user_string(current_process->page_table, username_ptr, USER_STRING_MAX);
        if (ulen <= 0) return (uint64_t)-EFAULT;
        target_name = (const char *)username_ptr;
        if (users_lookup_by_name(target_name, NULL) != 0) return (uint64_t)-ENOENT;
    }

    return (users_set_password(target_name, new_password) == 0) ? 0 : (uint64_t)-1;
}

// SYS_USB_COUNT (41) / SYS_USB_INFO (42) / SYS_USB_READ (43) / SYS_USB_WRITE
// (44) — см. комментарии в syscall.h. Тонкие обёртки над xhci_msd_*()
// (xhci.h) — прямой перенос command_usbinfo()/usbread()/usbwrite()
// (kernel/shell/commands/usb.c, мёртвый код), уже проверенных на
// безопасность (те же функции используются fat_mount.c для монтирования).
static uint64_t sys_usb_count(uint64_t unused1, uint64_t unused2, uint64_t unused3,
                              uint64_t unused4, uint64_t unused5) {
    (void)unused1; (void)unused2; (void)unused3; (void)unused4; (void)unused5;
    return (uint64_t)xhci_msd_device_count();
}

static uint64_t sys_usb_info(uint64_t index, uint64_t out_ptr, uint64_t unused1,
                             uint64_t unused2, uint64_t unused3) {
    (void)unused1; (void)unused2; (void)unused3;
    if (!current_process ||
        !is_user_range_valid(current_process->page_table, out_ptr, sizeof(lufira_usb_info_t), 1))
        return (uint64_t)-EFAULT;

    lufira_usb_info_t out;
    if (xhci_msd_get_info((int)index, &out.max_lba, &out.block_size) != 0)
        return (uint64_t)-ENOENT;

    memcpy((void *)out_ptr, &out, sizeof(out));
    return 0;
}

static uint64_t sys_usb_read(uint64_t index, uint64_t lba, uint64_t buf_ptr, uint64_t buf_size,
                             uint64_t unused1) {
    (void)unused1;
    if (!current_process) return (uint64_t)-EFAULT;

    uint32_t max_lba, block_size;
    if (xhci_msd_get_info((int)index, &max_lba, &block_size) != 0) return (uint64_t)-ENOENT;
    if (lba > max_lba) return (uint64_t)-EINVAL;
    if (buf_size < block_size) return (uint64_t)-EINVAL;

    if (!is_user_range_valid(current_process->page_table, buf_ptr, block_size, 1))
        return (uint64_t)-EFAULT;

    if (xhci_msd_read_block((int)index, (uint32_t)lba, (void *)buf_ptr, block_size) != 0)
        return (uint64_t)-1;
    return (uint64_t)block_size;
}

static uint64_t sys_usb_write(uint64_t index, uint64_t lba, uint64_t buf_ptr, uint64_t buf_size,
                              uint64_t unused1) {
    (void)unused1;
    if (!current_process) return (uint64_t)-EFAULT;
    if (current_process->uid != 0) return (uint64_t)-EPERM; // см. комментарий в syscall.h

    uint32_t max_lba, block_size;
    if (xhci_msd_get_info((int)index, &max_lba, &block_size) != 0) return (uint64_t)-ENOENT;
    if (lba > max_lba) return (uint64_t)-EINVAL;
    if (buf_size < block_size) return (uint64_t)-EINVAL;

    if (!is_user_range_valid(current_process->page_table, buf_ptr, block_size, 0))
        return (uint64_t)-EFAULT;

    if (xhci_msd_write_block((int)index, (uint32_t)lba, (const void *)buf_ptr, block_size) != 0)
        return (uint64_t)-1;
    return (uint64_t)block_size;
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
    [SYS_STATFS]  = sys_statfs,
    [SYS_MEMINFO] = sys_meminfo,
    [SYS_CPULOAD] = sys_cpuload,
    [SYS_PSLIST]  = sys_pslist,
    [SYS_SET_FOREGROUND] = sys_set_foreground,
    [SYS_SU] = sys_su,
    [SYS_MOUNT] = sys_mount,
    [SYS_UNMOUNT] = sys_unmount,
    [SYS_REBOOT] = sys_reboot,
    [SYS_SHUTDOWN] = sys_shutdown,
    [SYS_DEVMODE] = sys_devmode,
    [SYS_USERADD] = sys_useradd,
    [SYS_GROUPADD] = sys_groupadd,
    [SYS_PASSWD] = sys_passwd,
    [SYS_USB_COUNT] = sys_usb_count,
    [SYS_USB_INFO] = sys_usb_info,
    [SYS_USB_READ] = sys_usb_read,
    [SYS_USB_WRITE] = sys_usb_write,
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
