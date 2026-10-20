#include "syscall.h"
#include "drivers/console/console.h"
#include "system/process/process.h"
#include "system/elf/elf.h"
#include "system/timer/pit.h"
#include "system/timer/rtc.h"
#include "system/cpu/gdt.h"
#include "system/mm/heap.h"
#include "system/mm/pmm.h"
#include "system/mm/paging.h"
#include "system/mm/shm.h"
#include "lib/stddef.h"
#include "lib/string.h"
#include "fs/vfs/vfs.h"
#include "fs/lufirafs/lufirafs.h"
#include "system/devmode/devmode.h"
#include "system/users/users.h"
#include "fs/fat/fat_mount.h"
#include "fs/ramfs/ramfs.h"
#include "fs/ext2/ext2_mount.h"
#include "fs/exfat/exfat_mount.h"
#include "system/acpi/acpi.h"
#include "drivers/usb/xhci.h"
#include "drivers/console/graphics2d.h"
#include "system/ipc/mailbox.h"
#include "system/ipc/wm_protocol.h"
#include "drivers/input/input.h"
#include "system/ipc/netd_protocol.h"
#include "system/ipc/soundd_protocol.h"
#include "drivers/pci/pci.h"
#include "system/driver/driver.h"
#include "version.h"

extern lufirafs_t lufirafs;

// Вперёд-объявление — полное определение и разбор см. у wm_call() ниже;
// нужно уже здесь, в SYS_SOUND_*, которые идут раньше по файлу.
static int service_call(uint32_t service_pid, const void *req, uint32_t req_len,
                        void *reply, uint32_t reply_len);

// Открывает filename через VFS, читает целиком и заменяет текущий процесс
// через elf_exec_replace() (execve()). Используется и шеллом ("exec"), и
// SYS_EXEC. elf_exec_replace() забирает владение argv/envp и освобождает их
// сама (см. elf.h); если do_exec() проваливается раньше её вызова, освобождаем
// их здесь сами.
int do_exec(const char *filename, char *argv[], char *envp[]) {
    if (!filename || !*filename) {
        free_argv_envp(argv, envp);
        return -1;
    }

    // Проверка бита исполнения — VFS-пути всегда резолвятся от корня (см.
    // lufirafs_vfs.c), а не через cwd_inode (в отличие от sys_chmod/sys_chown).
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

    // elf_exec_replace() освобождает buf и argv/envp при любом исходе —
    // владение уже её.
    return elf_exec_replace(buf, size, filename, argv, envp);
}

typedef uint64_t (*syscall_fn_t)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

// Проверяет, что addr указывает на NUL-терминированную строку (<=max_len
// байт без терминатора) целиком в читаемой памяти процесса — is_user_accessible()
// проверяется на каждой впервые пересечённой странице. pml4_phys обязан быть
// активным CR3. Возвращает длину строки, -1 при невалидном адресе/нет '\0'.
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

// Копирует NUL-терминированный массив указателей на строки (argv[]/envp[])
// из памяти процесса в kernel-side буфер, в форме, которую
// elf_exec_replace()/free_argv_envp() ожидают/освобождают. array_ptr==0 —
// argc=0, не ошибка. NULL при любой другой ошибке, без частично
// выделенного состояния.
//
// Важно для вызывающих SYS_EXEC из userspace: is_user_range_valid() ниже
// проверяет фиксированный диапазон (MAX_EXEC_ARGS+1)*8 байт от array_ptr, а
// не только до NULL-терминатора (дешевле). Если argv[] лежит как локальная
// переменная у самого верха 16KB пользовательского стека (USER_STACK_SIZE),
// этот фиксированный диапазон может вылезти за пределы замапленной страницы
// и схватить -EFAULT, даже если реальный короткий argv[] был бы валиден —
// держите argv[]/envp[] в static/global памяти, а не на стеке.
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
    
    // Только devmode (DLOG) — shell.elf сам печатает код выхода, когда нужно
    // (wait); это чисто отладочный след для КАЖДОГО завершения.
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

    // Проверка прав — VFS всегда резолвит пути от корня (lufirafs_vfs.c);
    // vfs_open() сам этой проверки не делает.
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
// MAP_FIXED не поддерживается). Выделение eager: страницы физически
// выделяются и маппятся здесь же, не по требованию — обработчик page fault
// безусловно останавливает систему, demand paging не реализован. Под
// собственным CR3 процесса, свежая страница доступна без phys_to_virt().
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

    // MAP_SHARED: область переживает fork() алиасингом физических страниц
    // (не копированием), см. clone_address_space_deep() (process.c) и
    // реестр в shm.c. MAX_SHARED_REGION_PAGES — тот же потолок, что у shm.c.
    int want_shared = (flags & MAP_SHARED) != 0;
    if (want_shared && num_pages > MAX_SHARED_REGION_PAGES)
        return (uint64_t)-EINVAL;

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
    if (want_shared) page_flags |= PAGE_MMAP_SHARED;

    // kmalloc(), не стековый массив: MAX_SHARED_REGION_PAGES — 4096 (v0.9.5,
    // см. shm.h) ради SYS_NET_FETCH, 32KB на кернел-стеке (KERNEL_STACK_SIZE,
    // process.h — всего 16KB) переполнили бы его на КАЖДЫЙ sys_mmap(), даже
    // без MAP_SHARED. Выделяем только когда реально нужно и ровно под
    // num_pages, не под весь потолок.
    uint64_t *shared_pages = NULL;
    if (want_shared) {
        shared_pages = (uint64_t *)kmalloc(num_pages * sizeof(uint64_t));
        if (!shared_pages) return (uint64_t)-1;
    }

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
        if (want_shared) shared_pages[mapped] = phys;
    }

    if (mapped < num_pages) {
        // Не хватило памяти на часть запроса — откатываем замапленное.
        // PAGE_MMAP_SHARED мог быть выставлен, но shm_create() ещё не
        // звали — unmap_page() корректно откатится на pmm_free_page()
        // (shm_find_region_by_page() региона ещё не найдёт).
        for (uint64_t i = 0; i < mapped; i++) {
            unmap_page(base + i * PAGE_SIZE);
        }
        if (shared_pages) kfree(shared_pages);
        return (uint64_t)-1;
    }

    int shared_id = -1;
    if (want_shared) {
        shared_id = shm_create(shared_pages, num_pages);
        kfree(shared_pages); // shm_create() уже скопировало всё, что нужно, в свой реестр
        if (shared_id < 0) {
            // Реестр shm.c переполнен — то же самое, что "некуда записать
            // новый регион" чуть выше, только обнаруживается позже (после
            // того, как страницы уже выделены) — откатываем ровно так же.
            for (uint64_t i = 0; i < num_pages; i++) {
                unmap_page(base + i * PAGE_SIZE);
            }
            return (uint64_t)-1;
        }
    }

    current_process->next_mmap_addr += aligned_len;
    current_process->mmap_regions[slot].addr = base;
    current_process->mmap_regions[slot].length = aligned_len;
    current_process->mmap_regions[slot].shared_id = shared_id;

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

    // do_exec()/elf_exec_replace() забирает владение argv/envp и освобождает
    // их само на любом исходе — здесь освобождать не нужно.
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

    // Проверяем указатель до блокирующего process_wait() — плохой указатель
    // должен провалиться сразу, а не после ожидания ребёнка.
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

// SYS_CHDIR (15): та же логика, что command_cd() (shell/commands/filesystem.c),
// но на current_process->cwd_*, с проверкой указателя вместо разыменования.
// Смонтированные FAT (/mnt/...) и RAMFS (/tmp и т.п.) не имеют lufirafs-
// inode (fat_mount.h/ramfs.h), так что cwd_inode не может на них ссылаться
// обычным образом.
// NON_LUFIRAFS_MOUNT_CWD_INODE — заведомо невалидный номер инода (отвергается
// lufirafs_read_inode()/lufirafs_lookup()), безопасный часовой, общий для
// ОБОИХ видов монтирования — случайная операция с ним вернёт ENOENT, а не
// мусор. Источник истины — cwd_path; ".."/"." ниже реконструируют путь из
// него и не зависят от того, какая именно ФС стоит за монтированием.
#define NON_LUFIRAFS_MOUNT_CWD_INODE 0xFFFFFFFFu

// "cd" — единственная команда, отправляющая сюда сырой (возможно
// относительный) путь, не склеенный заранее с cwd (см. pathutil.h) —
// поэтому здесь, в отличие от sys_mkdir()/sys_remove(), нужно самим
// обрабатывать относительный ".." из смонтированного FAT.
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

    // Смонтированные FAT/RAMFS/ext2/exFAT — до LufiraFS, тот же приоритет,
    // что у vfs_open()/sys_mkdir()/sys_remove().
    if (path[0] == '/') {
        // private_data освобождаем для ВСЕХ, КРОМЕ RAMFS: у FAT (корень
        // монтирования)/ext2/exFAT это одноразовая обёртка под конкретный
        // lookup() (fat_root_dir_private_t / ext2_file_private_t / аналог
        // в exfat_mount.c); у RAMFS private_data — указатель на ЖИВОЙ узел
        // постоянного дерева (ramfs.c), который освобождает только
        // vfs_ramfs_unlink()/ramfs_unmount(), не это lookup.
        struct inode *mount_inode = vfs_fat_lookup(path);
        int is_ramfs = 0;
        if (!mount_inode) { mount_inode = vfs_ramfs_lookup(path); is_ramfs = (mount_inode != NULL); }
        if (!mount_inode) mount_inode = vfs_ext2_lookup(path);
        if (!mount_inode) mount_inode = vfs_exfat_lookup(path);
        if (mount_inode) {
            int is_dir = (mount_inode->type == FT_DIRECTORY);
            if (!is_ramfs && mount_inode->private_data) kfree(mount_inode->private_data);
            kfree(mount_inode);
            if (!is_dir) return (uint64_t)-ENOTDIR;

            int n = 0;
            while (path[n] && n < (int)sizeof(current_process->cwd_path) - 1) {
                current_process->cwd_path[n] = path[n];
                n++;
            }
            current_process->cwd_path[n] = '\0';
            current_process->cwd_inode = NON_LUFIRAFS_MOUNT_CWD_INODE;
            return 0;
        }
    }

    // Относительный "cd" из смонтированного FAT/RAMFS (нет lufirafs-inode
    // для такого вопроса — у cwd_inode тут сентинел, не настоящий инод) —
    // строим ЦЕЛЕВОЙ абсолютный путь из cwd_path и пробуем его СНАЧАЛА как
    // (воз)можно другое место внутри того же (или другого) FAT/RAMFS-
    // монтирования — обязательно для RAMFS: в отличие от FAT (только
    // корневой уровень), у неё есть настоящие вложенные каталоги, и "cd bar"
    // из "/tmp/foo" ("/tmp/foo/bar") иначе ничем не резолвился бы, кроме как
    // абсолютным путём. Условие на path[0] != '/' обязательно: абсолютные
    // пути (напр. "cd /mnt2") должны проваливаться в lufirafs_lookup(),
    // который и так резолвит их от корня независимо от cwd_inode.
    if (current_process->cwd_inode == NON_LUFIRAFS_MOUNT_CWD_INODE && path[0] != '/') {
        if (strcmp(path, ".") == 0) return 0;

        char target[sizeof(current_process->cwd_path)];
        if (strcmp(path, "..") == 0) {
            int len = (int)strlen(current_process->cwd_path);
            int last_slash = -1;
            for (int i = 0; i < len; i++) if (current_process->cwd_path[i] == '/') last_slash = i;

            if (last_slash <= 0) {
                target[0] = '/'; target[1] = '\0';
            } else {
                int i = 0;
                for (; i < last_slash; i++) target[i] = current_process->cwd_path[i];
                target[i] = '\0';
            }
        } else {
            int n = 0;
            int clen = (int)strlen(current_process->cwd_path);
            while (n < clen && n < (int)sizeof(target) - 1) { target[n] = current_process->cwd_path[n]; n++; }
            if (n > 0 && n < (int)sizeof(target) - 1 && target[n - 1] != '/') target[n++] = '/';
            int pn = 0;
            while (path[pn] && n < (int)sizeof(target) - 1) { target[n++] = path[pn++]; }
            target[n] = '\0';
        }

        struct inode *sub_inode = vfs_fat_lookup(target);
        int sub_is_ramfs = 0;
        if (!sub_inode) { sub_inode = vfs_ramfs_lookup(target); sub_is_ramfs = (sub_inode != NULL); }
        if (!sub_inode) sub_inode = vfs_ext2_lookup(target);
        if (!sub_inode) sub_inode = vfs_exfat_lookup(target);
        if (sub_inode) {
            int is_dir = (sub_inode->type == FT_DIRECTORY);
            if (!sub_is_ramfs && sub_inode->private_data) kfree(sub_inode->private_data);
            kfree(sub_inode);
            if (!is_dir) return (uint64_t)-ENOTDIR;

            int tn = 0;
            while (target[tn] && tn < (int)sizeof(current_process->cwd_path) - 1) {
                current_process->cwd_path[tn] = target[tn];
                tn++;
            }
            current_process->cwd_path[tn] = '\0';
            return 0; // cwd_inode остаётся тем же сентинелом
        }

        if (strcmp(path, "..") != 0) return (uint64_t)-ENOENT;

        // ".." вышла за пределы ЛЮБОГО FAT/RAMFS-монтирования — обычный
        // lufirafs-путь. "mount"/ramfs_mount() регистрируют только префикс,
        // не создают его родителя в lufirafs — "/mnt"/"/tmp" может вообще
        // не существовать как inode. Откат на корень — безопасный край,
        // лучше чем ENOENT на обычное "выйти из флешки/ramfs".
        uint32_t new_inode = lufirafs.sb.root_inode;
        lufirafs_lookup(&lufirafs, lufirafs.sb.root_inode, target, &new_inode);

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

    // Смонтированные FAT/RAMFS/ext2/exFAT — до всех проверок прав на
    // LufiraFS: путь не существует как lufirafs-inode, lufirafs_resolve_parent()
    // вернул бы -ENOENT (mkdir.elf зовёт SYS_MKDIR, не vfs_mkdir() напрямую —
    // более ранний фикс в vfs.c сюда не доходил).
    if (path[0] == '/') {
        int r = vfs_fat_mkdir(path);
        if (r != -2) return (r == 0) ? 0 : (uint64_t)-1;
        r = vfs_ramfs_mkdir(path);
        if (r != -2) return (r == 0) ? 0 : (uint64_t)-1;
        r = vfs_ext2_mkdir(path);
        if (r != -2) return (r == 0) ? 0 : (uint64_t)-1;
        r = vfs_exfat_mkdir(path);
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

    // Смонтированные FAT/RAMFS/ext2/exFAT — см. тот же комментарий в
    // sys_mkdir() выше.
    if (path[0] == '/') {
        int r = vfs_fat_unlink(path);
        if (r != -2) return (r == 0) ? 0 : (uint64_t)-1;
        r = vfs_ramfs_unlink(path);
        if (r != -2) return (r == 0) ? 0 : (uint64_t)-1;
        r = vfs_ext2_unlink(path);
        if (r != -2) return (r == 0) ? 0 : (uint64_t)-1;
        r = vfs_exfat_unlink(path);
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

// SYS_RTC_GETTIME (67): buf_ptr -> lufira_rtc_time_t. Тонкая обёртка над
// rtc_get_time() (kernel/system/timer/rtc.c) — тот же стиль, что
// SYS_MEMINFO/SYS_CPULOAD выше: вся реальная работа уже сделана драйвером,
// здесь только проверка user-указателя и копирование.
static uint64_t sys_rtc_gettime(uint64_t buf_ptr, uint64_t unused1, uint64_t unused2,
                                 uint64_t unused3, uint64_t unused4) {
    (void)unused1; (void)unused2; (void)unused3; (void)unused4;

    if (!current_process ||
        !is_user_range_valid(current_process->page_table, buf_ptr, sizeof(lufira_rtc_time_t), 1))
        return (uint64_t)-EFAULT;

    rtc_time_t t;
    if (rtc_get_time(&t) != 0) return (uint64_t)-EIO;

    lufira_rtc_time_t out;
    out.year = t.year;
    out.month = t.month;
    out.day = t.day;
    out.hour = t.hour;
    out.minute = t.minute;
    out.second = t.second;
    memcpy((void *)buf_ptr, &out, sizeof(out));
    return 0;
}

// ===== v0.9.5: звук вынесен из ядра в userspace-демон soundd — см.
// подробный разбор архитектуры у SYS_SOUNDD_REGISTER/SYS_SOUND_CONSUME в
// syscall.h. INFO/SET_VOLUME/STOP ниже релеят control-plane запрос через
// service_call() (soundd_protocol.h); PLAY_PCM/CONSUME — через кольцо ниже
// (control-plane 96-байтный mailbox для PCM-данных не годится). =====

// Кольцо фреймов между ЛЮБЫМ клиентом (производитель, SYS_SOUND_PLAY_PCM) и
// soundd (единственный потребитель, SYS_SOUND_CONSUME). ~5.7с запаса при
// 48кГц — в штатной работе soundd вычитывает непрерывно и кольцо не
// переполняется; производитель НЕ блокируется (пишет сколько влезло и
// возвращает это число — тот же компромисс, что у pipe-записи без
// блокировки), потребитель блокируется честно, пока кольцо пусто (тот же
// cli/sti+PROCESS_BLOCKED приём, что mailbox_recv()/driver_irq_wait()).
#define SOUND_RING_FRAMES (256u * 1024u)
// kmalloc(), не статический массив: ~1MB для стерео interleaved-буфера в
// .bss раздул бы РАЗМЕР ФАЙЛА kernel.bin на столько же — линкер-скрипт
// (linker.ld) кладёt .bss как обычную секцию без NOLOAD, objcopy -O binary
// материализует её нулями в самом бинарнике. Выделяется один раз в
// syscall_init().
static int16_t *g_sound_ring = NULL;
static uint32_t g_sound_ring_head = 0, g_sound_ring_tail = 0, g_sound_ring_count = 0;
static process_t *g_sound_ring_waiter = NULL; // soundd, заблокирован в SYS_SOUND_CONSUME

static inline uint64_t sound_ring_lock(void) {
    uint64_t flags;
    asm volatile("pushfq; popq %0" : "=r"(flags) :: "memory");
    asm volatile("cli");
    return flags;
}
static inline void sound_ring_unlock(uint64_t flags) {
    asm volatile("push %0; popfq" : : "r"(flags) : "memory", "cc");
}

static uint32_t sound_ring_write(const int16_t *samples, uint32_t frames) {
    if (!g_sound_ring) return 0; // kmalloc() в syscall_init() не удался — звук недоступен

    uint64_t flags = sound_ring_lock();
    uint32_t free_space = SOUND_RING_FRAMES - g_sound_ring_count;
    uint32_t n = frames < free_space ? frames : free_space;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t slot = (g_sound_ring_tail + i) % SOUND_RING_FRAMES;
        g_sound_ring[slot * 2]     = samples[i * 2];
        g_sound_ring[slot * 2 + 1] = samples[i * 2 + 1];
    }
    g_sound_ring_tail = (g_sound_ring_tail + n) % SOUND_RING_FRAMES;
    g_sound_ring_count += n;
    process_t *waiter = g_sound_ring_waiter;
    g_sound_ring_waiter = NULL;
    sound_ring_unlock(flags);

    if (waiter) waiter->state = PROCESS_READY; // вне критической секции — как mailbox_send()
    return n;
}

// timeout_ms — тот же трёхвариантный приём, что mailbox_recv() (mailbox.h):
// <0 ждать неограниченно (настоящая блокировка), 0 вернуть 0 немедленно если
// пусто, >0 короткий опрос с шагом в тик. soundd мультиплексирует этим PCM
// (кольцо) и control-plane (mailbox, SOUNDD_OP_*) одним циклом — короткий
// таймаут здесь, а не вечная блокировка, иначе SYS_SOUND_SET_VOLUME/INFO/
// STOP зависли бы, пока не придёт следующий кадр PCM.
static uint32_t sound_ring_consume(int16_t *out, uint32_t max_frames, int64_t timeout_ms) {
    uint64_t elapsed_ms = 0;
    for (;;) {
        uint64_t flags = sound_ring_lock();
        if (g_sound_ring_count > 0) {
            uint32_t n = g_sound_ring_count < max_frames ? g_sound_ring_count : max_frames;
            for (uint32_t i = 0; i < n; i++) {
                uint32_t slot = (g_sound_ring_head + i) % SOUND_RING_FRAMES;
                out[i * 2]     = g_sound_ring[slot * 2];
                out[i * 2 + 1] = g_sound_ring[slot * 2 + 1];
            }
            g_sound_ring_head = (g_sound_ring_head + n) % SOUND_RING_FRAMES;
            g_sound_ring_count -= n;
            sound_ring_unlock(flags);
            return n;
        }

        if (timeout_ms == 0) {
            sound_ring_unlock(flags);
            return 0;
        }

        if (timeout_ms > 0) {
            sound_ring_unlock(flags);
            if (elapsed_ms >= (uint64_t)timeout_ms) return 0;
            process_sleep(10);
            elapsed_ms += 10;
            continue;
        }

        // timeout_ms < 0 — настоящая блокировка, без опроса. Тот же приём,
        // что mailbox_recv()/driver_irq_wait() (не отпускаем cli перед
        // schedule(), см. их же комментарий).
        g_sound_ring_waiter = current_process;
        current_process->state = PROCESS_BLOCKED;
        schedule();
    }
}

void sound_ring_clear(void) {
    uint64_t flags = sound_ring_lock();
    g_sound_ring_head = g_sound_ring_tail = g_sound_ring_count = 0;
    sound_ring_unlock(flags);
}

// SYS_SOUND_INFO (68): buf_ptr -> lufira_sound_info_t. "Звука нет"/"soundd
// не запущен" — валидный ответ (available=0), а не -EIO.
static uint64_t sys_sound_info(uint64_t buf_ptr, uint64_t unused1, uint64_t unused2,
                                uint64_t unused3, uint64_t unused4) {
    (void)unused1; (void)unused2; (void)unused3; (void)unused4;

    if (!current_process ||
        !is_user_range_valid(current_process->page_table, buf_ptr, sizeof(lufira_sound_info_t), 1))
        return (uint64_t)-EFAULT;

    lufira_sound_info_t out;
    memset(&out, 0, sizeof(out));

    uint32_t soundd_pid = process_get_soundd_pid();
    if (soundd_pid) {
        soundd_request_t req; memset(&req, 0, sizeof(req));
        req.opcode = SOUNDD_OP_INFO;
        soundd_reply_t reply;
        if (service_call(soundd_pid, &req, sizeof(req), &reply, sizeof(reply)) == 0) {
            out.available = (uint8_t)reply.result;
            out.volume = (uint8_t)reply.a[0];
            out.sample_rate = (uint32_t)reply.a[1];
            size_t i = 0;
            for (; reply.str[i] && i < sizeof(out.driver) - 1; i++) out.driver[i] = reply.str[i];
            out.driver[i] = '\0';
        }
    }

    memcpy((void *)buf_ptr, &out, sizeof(out));
    return 0;
}

// SYS_SOUND_SET_VOLUME (69): volume (0-100) — soundd сам обрезает значения
// выше 100, не дублируем клэмп здесь.
static uint64_t sys_sound_set_volume(uint64_t volume, uint64_t unused1, uint64_t unused2,
                                      uint64_t unused3, uint64_t unused4) {
    (void)unused1; (void)unused2; (void)unused3; (void)unused4;

    uint32_t soundd_pid = process_get_soundd_pid();
    if (!soundd_pid) return (uint64_t)-1;

    soundd_request_t req; memset(&req, 0, sizeof(req));
    req.opcode = SOUNDD_OP_SET_VOLUME;
    req.a[0] = (int32_t)volume;
    soundd_reply_t reply;
    if (service_call(soundd_pid, &req, sizeof(req), &reply, sizeof(reply)) != 0) return (uint64_t)-1;
    return (uint64_t)reply.result;
}

// SYS_SOUND_PLAY_PCM (70): (buf_ptr, frames) — см. комментарий в syscall.h.
// frames*4 байт (int16_t стерео, interleaved) — та же проверка диапазона,
// что и везде на буферы фиксированного размера, плюс явный предохранитель
// от integer-overflow в frames*4 на 32-битной арифметике ниже (16МБ с лихвой
// перекрывает один ~28мс-слайс doom-порта).
#define SOUND_PLAY_PCM_MAX_FRAMES (16u * 1024u * 1024u / 4u)

static uint64_t sys_sound_play_pcm(uint64_t buf_ptr, uint64_t frames, uint64_t unused1,
                                    uint64_t unused2, uint64_t unused3) {
    (void)unused1; (void)unused2; (void)unused3;

    if (!process_get_soundd_pid()) return (uint64_t)-1;
    if (frames == 0 || frames > SOUND_PLAY_PCM_MAX_FRAMES) return (uint64_t)-EINVAL;
    if (!current_process ||
        !is_user_range_valid(current_process->page_table, buf_ptr, (size_t)frames * 4, 0))
        return (uint64_t)-EFAULT;

    uint32_t written = sound_ring_write((const int16_t *)buf_ptr, (uint32_t)frames);
    return written > 0 ? 0 : (uint64_t)-1;
}

// SYS_SOUNDD_REGISTER (89) — root-only, см. syscall.h.
static uint64_t sys_soundd_register(uint64_t unused0, uint64_t unused1, uint64_t unused2,
                                     uint64_t unused3, uint64_t unused4) {
    (void)unused0; (void)unused1; (void)unused2; (void)unused3; (void)unused4;
    if (!current_process) return (uint64_t)-EFAULT;
    if (current_process->uid != 0) return (uint64_t)-EPERM;
    if (process_soundd_register(current_process->pid) != 0) return (uint64_t)-1;
    return 0;
}

// SYS_NETD_REGISTER (92) — root-only, см. syscall.h.
static uint64_t sys_netd_register(uint64_t unused0, uint64_t unused1, uint64_t unused2,
                                   uint64_t unused3, uint64_t unused4) {
    (void)unused0; (void)unused1; (void)unused2; (void)unused3; (void)unused4;
    if (!current_process) return (uint64_t)-EFAULT;
    if (current_process->uid != 0) return (uint64_t)-EPERM;
    if (process_netd_register(current_process->pid) != 0) return (uint64_t)-1;
    return 0;
}

// SYS_SOUND_CONSUME (90) — только зарегистрированному soundd, см. syscall.h.
static uint64_t sys_sound_consume(uint64_t out_ptr, uint64_t max_frames, uint64_t timeout_ms,
                                   uint64_t unused3, uint64_t unused4) {
    (void)unused3; (void)unused4;
    if (!current_process) return (uint64_t)-EFAULT;
    if (current_process->pid != process_get_soundd_pid()) return (uint64_t)-EPERM;
    if (max_frames == 0 || max_frames > SOUND_PLAY_PCM_MAX_FRAMES) return (uint64_t)-EINVAL;
    if (!is_user_range_valid(current_process->page_table, out_ptr, (size_t)max_frames * 4, 1))
        return (uint64_t)-EFAULT;

    return (uint64_t)sound_ring_consume((int16_t *)out_ptr, (uint32_t)max_frames, (int64_t)timeout_ms);
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

// Создаёт path и все отсутствующие родительские директории в lufirafs
// ("mkdir -p"). mount раньше регистрировал только строковый префикс
// (fat_mount.c), оставляя точку монтирования несуществующей как lufirafs-
// директория, так что cd/ls на неё падали в ENOENT. Молча не настаивает на
// успехе — удобство, а не обязательное условие: mount всё равно работает
// через fat_mount.c по строковому префиксу, даже если тут не задалось.
static void lufirafs_mkdir_p(const char *path) {
    if (!path || path[0] != '/') return;

    uint32_t cur = lufirafs.sb.root_inode;
    uint32_t uid = current_process ? current_process->uid : 0;
    uint32_t gid = current_process ? current_process->gid : 0;
    int i = 1; // пропускаем ведущий '/'
    char component[LUFIRAFS_MAX_NAME + 1];
    int created_any = 0;

    while (path[i]) {
        int start = i;
        while (path[i] && path[i] != '/') i++;
        int len = i - start;
        if (len > 0 && len <= LUFIRAFS_MAX_NAME) {
            memcpy(component, path + start, (size_t)len);
            component[len] = '\0';

            uint32_t next;
            if (lufirafs_lookup(&lufirafs, cur, component, &next) == 0) {
                cur = next;
            } else {
                uint32_t new_ino;
                if (lufirafs_create(&lufirafs, cur, component, LUFIRAFS_MODE_DIR,
                                     uid, gid, LUFIRAFS_DEFAULT_DIR_PERM, &new_ino) != 0)
                    return; // не настаиваем - mount попробует сам по строковому префиксу
                cur = new_ino;
                created_any = 1;
            }
        }
        while (path[i] == '/') i++;
    }

    if (created_any) lufirafs_sync(&lufirafs);
}

// SYS_MOUNT (33) / SYS_UNMOUNT (34): тонкие обёртки — логика уже в
// vfs_fat_mount()/vfs_fat_unmount() (fat_mount.c) и их ext2/exFAT
// эквивалентах. Один syscall без выбора формата — автоопределение:
// пробуем по очереди (FAT, затем ext2, затем exFAT), каждый сам
// отказывается монтировать образ не своего формата (неверный magic),
// так что успевает ровно один. Тот же порядок проб и в sys_unmount() —
// снять то монтирование, которое реально держит этот prefix.
static uint64_t sys_mount(uint64_t prefix_ptr, uint64_t usb_index, uint64_t unused1,
                          uint64_t unused2, uint64_t unused3) {
    (void)unused1; (void)unused2; (void)unused3;
    if (!current_process) return (uint64_t)-EFAULT;

    int64_t slen = validate_user_string(current_process->page_table, prefix_ptr, USER_STRING_MAX);
    if (slen <= 0) return (uint64_t)-EFAULT;

    lufirafs_mkdir_p((const char *)prefix_ptr);

    int res = vfs_fat_mount((int)usb_index, (const char *)prefix_ptr);
    if (res < 0) res = vfs_ext2_mount((int)usb_index, (const char *)prefix_ptr);
    if (res < 0) res = vfs_exfat_mount((int)usb_index, (const char *)prefix_ptr);
    return (uint64_t)(int64_t)res;
}

static uint64_t sys_unmount(uint64_t prefix_ptr, uint64_t unused1, uint64_t unused2,
                            uint64_t unused3, uint64_t unused4) {
    (void)unused1; (void)unused2; (void)unused3; (void)unused4;
    if (!current_process) return (uint64_t)-EFAULT;

    int64_t slen = validate_user_string(current_process->page_table, prefix_ptr, USER_STRING_MAX);
    if (slen <= 0) return (uint64_t)-EFAULT;

    int res = vfs_fat_unmount((const char *)prefix_ptr);
    if (res != 0) res = vfs_ext2_unmount((const char *)prefix_ptr);
    if (res != 0) res = vfs_exfat_unmount((const char *)prefix_ptr);
    return (uint64_t)(int64_t)res;
}

// SYS_REBOOT (35) / SYS_SHUTDOWN (36) — перенос command_reboot()/
// command_shutdown() (shell/commands/system.c), логика сброса синхронизирована
// с ними 1:1 (см. acpi_reset()/RST_CNT там); root/синк диска гейтятся тут,
// а не в выводе на консоль.
static uint64_t sys_reboot(uint64_t unused1, uint64_t unused2, uint64_t unused3,
                           uint64_t unused4, uint64_t unused5) {
    (void)unused1; (void)unused2; (void)unused3; (void)unused4; (void)unused5;
    if (!current_process) return (uint64_t)-EFAULT;
    if (current_process->uid != 0) return (uint64_t)-EPERM;

    lufirafs_flush(&lufirafs);
    acpi_reset(); // спецификационный сброс через FADT RESET_REG, если платформа его поддерживает
    __asm__ volatile ("outb %0, %1" : : "a"((uint8_t)0xFE), "Nd"((uint16_t)0x64)); // 8042-контроллер
    // PCI RST_CNT (0xCF9) — см. подробный комментарий в command_reboot() (system.c).
    __asm__ volatile ("outb %0, %1" : : "a"((uint8_t)0x06), "Nd"((uint16_t)0xCF9));
    __asm__ volatile ("outb %0, %1" : : "a"((uint8_t)0x0E), "Nd"((uint16_t)0xCF9));
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

// Создаёт /home (если нет) и /home/<username>, owner — новый пользователь,
// perm 0700 — своя копия ensure_home_dir() из shell/commands/users.c (её
// статические хелперы не экспортированы). При неудаче откатывается на "/".
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
// (44) — тонкие обёртки над xhci_msd_*() (xhci.h), те же функции, что
// fat_mount.c использует для монтирования.
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

// SYS_POLL (45) — см. подробный комментарий в syscall.h. Опрос с шагом в
// один тик (process_sleep(10), уже существующий блокирующий примитив —
// тот же, что и у SYS_SLEEP) вместо честного пробуждения по событию.
#define LUFIRA_POLL_MAX_NFDS 64

static uint64_t sys_poll(uint64_t fds_ptr, uint64_t nfds, uint64_t timeout_ms,
                         uint64_t unused1, uint64_t unused2) {
    (void)unused1; (void)unused2;
    if (!current_process) return (uint64_t)-EFAULT;
    if (nfds == 0) return 0;
    if (nfds > LUFIRA_POLL_MAX_NFDS) return (uint64_t)-EINVAL;

    size_t bytes = (size_t)nfds * sizeof(lufira_pollfd_t);
    if (!is_user_range_valid(current_process->page_table, fds_ptr, bytes, 1))
        return (uint64_t)-EFAULT;

    lufira_pollfd_t *fds = (lufira_pollfd_t *)fds_ptr;
    int64_t timeout = (int64_t)timeout_ms; // <0 = ждать неограниченно
    uint64_t elapsed_ms = 0;

    for (;;) {
        int ready = 0;
        for (uint64_t i = 0; i < nfds; i++) {
            int revents = 0;
            if (vfs_poll_check(fds[i].fd, fds[i].events, &revents) != 0) revents = 0;
            fds[i].revents = revents;
            if (revents) ready++;
        }
        if (ready > 0) return (uint64_t)ready;
        if (timeout == 0) return 0;
        if (timeout > 0 && elapsed_ms >= (uint64_t)timeout) return 0;

        process_sleep(10);
        elapsed_ms += 10;
    }
}

// SYS_SIGACTION (46) — см. комментарий в syscall.h. SIGINT/SIGTERM/SIGALRM
// (последний — для SYS_ALARM, v0.8-мост, пункт 6).
static uint64_t sys_sigaction(uint64_t sig, uint64_t handler_ptr, uint64_t unused1,
                              uint64_t unused2, uint64_t unused3) {
    (void)unused1; (void)unused2; (void)unused3;
    if (!current_process) return (uint64_t)-EFAULT;
    if (sig != SIGINT && sig != SIGTERM && sig != SIGALRM) return (uint64_t)-EINVAL;

    current_process->signal_handlers[sig] = handler_ptr;
    return 0;
}

// SYS_ALARM (48) — см. комментарий в syscall.h. Один тик = 10мс
// (PIT_FREQUENCY), тот же округляющий приём, что и у SYS_SLEEP.
static uint64_t sys_alarm(uint64_t milliseconds, uint64_t unused1, uint64_t unused2,
                          uint64_t unused3, uint64_t unused4) {
    (void)unused1; (void)unused2; (void)unused3; (void)unused4;
    if (!current_process) return (uint64_t)-EFAULT;

    if (milliseconds == 0) {
        current_process->alarm_tick = 0; // снять уже взведённый, если был
        return 0;
    }

    uint64_t ticks = (milliseconds + 9) / 10;
    current_process->alarm_tick = pit_get_ticks() + ticks;
    return 0;
}

// SYS_GET_FOREGROUND (49) — см. комментарий в syscall.h. Нет аргументов,
// нет проверки доступа (просто чтение) — симметрично тому, что сам
// foreground_pid и так всегда был виден изнутри ядра всем.
static uint64_t sys_get_foreground(uint64_t unused0, uint64_t unused1, uint64_t unused2,
                                   uint64_t unused3, uint64_t unused4) {
    (void)unused0; (void)unused1; (void)unused2; (void)unused3; (void)unused4;
    return (uint64_t)process_get_foreground();
}

// ===== v0.8 (GUI+WM), этап 3 — WM вынесен в userspace. Разбор архитектуры:
// kernel/system/ipc/wm_protocol.h. SYS_WIN_* ниже — тонкие RPC-обёртки
// поверх generic mailbox.h, см. wm_call() сразу под ними. =====

// Разделяемая с userspace раскладка события — см. struct lufira_gui_event
// в libc/include/lufira/syscall.h (поля и порядок обязаны совпадать 1:1).
// Ядро больше не знает смысла type/key_or_button (раньше — GUI_EVENT_* из
// удалённого gui.h) — просто прозрачно переносит int'ы из wm_reply_t
// (wm_protocol.h), которые наполняет уже сам userspace WM.
typedef struct __attribute__((packed)) {
    int32_t type;
    int32_t x, y;
    int32_t key_or_button;
} lufira_gui_event_t;

// Общий синхронный RPC поверх mailbox.h — отправить req service_pid и
// заблокированно дождаться ОТВЕТА ИМЕННО ОТ НЕГО. Используется и для WM
// (wm_call() ниже), и для soundd (v0.9.5, service_call(g_soundd_pid, ...)
// у SYS_SOUND_INFO/SET_VOLUME/STOP) — любой будущий демон этого же вида
// получает фикс бесплатно.
//
// БЕЗОПАСНОСТЬ: любой процесс может вызвать SYS_IPC_SEND с произвольным
// dest_pid (mailbox_send() не проверяет владение) — без проверки
// msg.sender_pid здесь атакующий мог бы прислать жертве, заблокированной в
// этом вызове, поддельный "ответ" раньше настоящего сервиса. Несовпадающие
// sender_pid тихо отбрасываются и ожидание продолжается: у обычного
// клиентского процесса в любой момент ровно один вызов в полёте, новых
// легитимных сообщений от кого-то ещё в этом окне быть не должно.
static int service_call(uint32_t service_pid, const void *req, uint32_t req_len,
                        void *reply, uint32_t reply_len) {
    if (!service_pid) return -1;
    if (mailbox_send(service_pid, current_process->pid, req, req_len) != 0) return -1;

    for (;;) {
        lufira_ipc_msg_t msg;
        if (mailbox_recv(&msg, -1) != 0) return -1; // ждать ответа неограниченно
        if (msg.sender_pid != service_pid) continue; // подмена — см. разбор выше
        if (msg.len != reply_len) return -1;
        memcpy(reply, msg.payload, reply_len);
        return 0;
    }
}

static int wm_call(const wm_request_t *req, wm_reply_t *reply) {
    uint32_t wm_pid = process_get_wm_pid();
    return service_call(wm_pid, req, sizeof(*req), reply, sizeof(*reply));
}

// SYS_WIN_CREATE (50): x, y, w, h, title_ptr.
static uint64_t sys_win_create(uint64_t x, uint64_t y, uint64_t w, uint64_t h,
                               uint64_t title_ptr) {
    if (!current_process) return (uint64_t)-EFAULT;

    wm_request_t req;
    memset(&req, 0, sizeof(req));
    req.opcode = WM_OP_WIN_CREATE;
    req.a[0] = (int32_t)x; req.a[1] = (int32_t)y;
    req.a[2] = (int32_t)w; req.a[3] = (int32_t)h;
    if (title_ptr) {
        int64_t slen = validate_user_string(current_process->page_table, title_ptr, USER_STRING_MAX);
        if (slen < 0) return (uint64_t)-EFAULT;
        int n = 0;
        const char *src = (const char *)title_ptr;
        while (src[n] && n < (int)sizeof(req.str) - 1) { req.str[n] = src[n]; n++; }
        req.str[n] = '\0';
    }

    wm_reply_t reply;
    if (wm_call(&req, &reply) != 0) return (uint64_t)-1;
    return (reply.result >= 0) ? (uint64_t)reply.result : (uint64_t)-1;
}

static uint64_t sys_win_destroy(uint64_t window_id, uint64_t unused1, uint64_t unused2,
                                uint64_t unused3, uint64_t unused4) {
    (void)unused1; (void)unused2; (void)unused3; (void)unused4;
    if (!current_process) return (uint64_t)-EFAULT;

    wm_request_t req; memset(&req, 0, sizeof(req));
    req.opcode = WM_OP_WIN_DESTROY;
    req.a[0] = (int32_t)window_id;

    wm_reply_t reply;
    if (wm_call(&req, &reply) != 0) return (uint64_t)-1;
    return (reply.result == 0) ? 0 : (uint64_t)-1;
}

static uint64_t sys_win_fill(uint64_t window_id, uint64_t color, uint64_t unused2,
                             uint64_t unused3, uint64_t unused4) {
    (void)unused2; (void)unused3; (void)unused4;
    if (!current_process) return (uint64_t)-EFAULT;

    wm_request_t req; memset(&req, 0, sizeof(req));
    req.opcode = WM_OP_WIN_FILL;
    req.a[0] = (int32_t)window_id;
    req.a[1] = (int32_t)color;

    wm_reply_t reply;
    if (wm_call(&req, &reply) != 0) return (uint64_t)-1;
    return (reply.result == 0) ? 0 : (uint64_t)-1;
}

// w/h упакованы в один аргумент ((w<<32)|h) — у syscall'а только 5
// регистровых слотов, а window_id/x/y/wh/color уже ровно пять.
static uint64_t sys_win_draw_rect(uint64_t window_id, uint64_t x, uint64_t y,
                                  uint64_t wh_packed, uint64_t color) {
    if (!current_process) return (uint64_t)-EFAULT;
    uint32_t w = (uint32_t)(wh_packed >> 32);
    uint32_t h = (uint32_t)(wh_packed & 0xFFFFFFFFu);

    wm_request_t req; memset(&req, 0, sizeof(req));
    req.opcode = WM_OP_WIN_DRAW_RECT;
    req.a[0] = (int32_t)window_id;
    req.a[1] = (int32_t)x; req.a[2] = (int32_t)y;
    req.a[3] = (int32_t)w; req.a[4] = (int32_t)h;
    req.a[5] = (int32_t)color;

    wm_reply_t reply;
    if (wm_call(&req, &reply) != 0) return (uint64_t)-1;
    return (reply.result == 0) ? 0 : (uint64_t)-1;
}

static uint64_t sys_win_draw_text(uint64_t window_id, uint64_t x, uint64_t y,
                                  uint64_t text_ptr, uint64_t color) {
    if (!current_process) return (uint64_t)-EFAULT;
    int64_t slen = validate_user_string(current_process->page_table, text_ptr, USER_STRING_MAX);
    if (slen < 0) return (uint64_t)-EFAULT;

    wm_request_t req; memset(&req, 0, sizeof(req));
    req.opcode = WM_OP_WIN_DRAW_TEXT;
    req.a[0] = (int32_t)window_id;
    req.a[1] = (int32_t)x; req.a[2] = (int32_t)y;
    req.a[3] = (int32_t)color;
    int n = 0;
    const char *src = (const char *)text_ptr;
    while (src[n] && n < (int)sizeof(req.str) - 1) { req.str[n] = src[n]; n++; }
    req.str[n] = '\0';

    wm_reply_t reply;
    if (wm_call(&req, &reply) != 0) return (uint64_t)-1;
    return (reply.result == 0) ? 0 : (uint64_t)-1;
}

static uint64_t sys_win_poll_event(uint64_t window_id, uint64_t event_buf_ptr,
                                   uint64_t unused2, uint64_t unused3, uint64_t unused4) {
    (void)unused2; (void)unused3; (void)unused4;
    if (!current_process) return (uint64_t)-EFAULT;
    if (!is_user_range_valid(current_process->page_table, event_buf_ptr,
                             sizeof(lufira_gui_event_t), 1))
        return (uint64_t)-EFAULT;

    wm_request_t req; memset(&req, 0, sizeof(req));
    req.opcode = WM_OP_WIN_POLL_EVENT;
    req.a[0] = (int32_t)window_id;

    wm_reply_t reply;
    if (wm_call(&req, &reply) != 0) return 0;
    if (reply.result != 1) return 0;

    lufira_gui_event_t *out = (lufira_gui_event_t *)event_buf_ptr;
    out->type = reply.ev_type;
    out->x = reply.ev_x;
    out->y = reply.ev_y;
    out->key_or_button = reply.ev_key;
    return 1;
}

static uint64_t sys_win_move(uint64_t window_id, uint64_t x, uint64_t y,
                             uint64_t unused3, uint64_t unused4) {
    (void)unused3; (void)unused4;
    if (!current_process) return (uint64_t)-EFAULT;

    wm_request_t req; memset(&req, 0, sizeof(req));
    req.opcode = WM_OP_WIN_MOVE;
    req.a[0] = (int32_t)window_id;
    req.a[1] = (int32_t)x; req.a[2] = (int32_t)y;

    wm_reply_t reply;
    if (wm_call(&req, &reply) != 0) return (uint64_t)-1;
    return (reply.result == 0) ? 0 : (uint64_t)-1;
}

// SYS_IPC_SEND (57): dest_pid, msg_ptr, len — см. syscall.h/mailbox.h.
static uint64_t sys_ipc_send(uint64_t dest_pid, uint64_t msg_ptr, uint64_t len,
                             uint64_t unused3, uint64_t unused4) {
    (void)unused3; (void)unused4;
    if (!current_process) return (uint64_t)-EFAULT;
    if (len > MAX_IPC_MSG_PAYLOAD) return (uint64_t)-EINVAL;
    if (len > 0 && !is_user_range_valid(current_process->page_table, msg_ptr, len, 0))
        return (uint64_t)-EFAULT;

    int res = mailbox_send((uint32_t)dest_pid, current_process->pid, (const void *)msg_ptr, (uint32_t)len);
    if (res == 0) return 0;
    return (res == -2) ? (uint64_t)-EAGAIN : (uint64_t)-EINVAL;
}

// SYS_IPC_RECV (58): msg_out_ptr, blocking — см. syscall.h/mailbox.h.
static uint64_t sys_ipc_recv(uint64_t msg_out_ptr, uint64_t timeout_ms, uint64_t unused2,
                             uint64_t unused3, uint64_t unused4) {
    (void)unused2; (void)unused3; (void)unused4;
    if (!current_process) return (uint64_t)-EFAULT;
    if (!is_user_range_valid(current_process->page_table, msg_out_ptr, sizeof(lufira_ipc_msg_t), 1))
        return (uint64_t)-EFAULT;

    if (mailbox_recv((void *)msg_out_ptr, (int)timeout_ms) != 0) return 0;
    return 1;
}

// SYS_WM_REGISTER (59) — см. syscall.h.
static uint64_t sys_wm_register(uint64_t unused0, uint64_t unused1, uint64_t unused2,
                                uint64_t unused3, uint64_t unused4) {
    (void)unused0; (void)unused1; (void)unused2; (void)unused3; (void)unused4;
    if (!current_process) return (uint64_t)-EFAULT;
    return (process_wm_register(current_process->pid) == 0) ? 0 : (uint64_t)-1;
}

// SYS_WM_UNREGISTER (71) — см. syscall.h.
static uint64_t sys_wm_unregister(uint64_t unused0, uint64_t unused1, uint64_t unused2,
                                  uint64_t unused3, uint64_t unused4) {
    (void)unused0; (void)unused1; (void)unused2; (void)unused3; (void)unused4;
    if (!current_process) return (uint64_t)-EFAULT;
    return (process_wm_unregister(current_process->pid) == 0) ? 0 : (uint64_t)-1;
}

// SYS_SOUND_STOP (72) — см. syscall.h. Сбрасывает и кольцо (иначе
// отложенные кадры заиграли бы при следующем SYS_SOUND_PLAY_PCM/при
// перезапуске soundd), и просит сам демон остановить аппаратный поток —
// порядок (кольцо сначала) не важен для корректности, оба независимы.
static uint64_t sys_sound_stop(uint64_t unused0, uint64_t unused1, uint64_t unused2,
                               uint64_t unused3, uint64_t unused4) {
    (void)unused0; (void)unused1; (void)unused2; (void)unused3; (void)unused4;

    sound_ring_clear();

    uint32_t soundd_pid = process_get_soundd_pid();
    if (soundd_pid) {
        soundd_request_t req; memset(&req, 0, sizeof(req));
        req.opcode = SOUNDD_OP_STOP;
        soundd_reply_t reply;
        service_call(soundd_pid, &req, sizeof(req), &reply, sizeof(reply));
    }
    return 0;
}

// SYS_PING (73) — см. syscall.h. Релей через netd (NETD_OP_PING) —
// icmp_ping_send()/icmp_ping_wait() теперь исполняются внутри netd, не
// здесь; эта функция только пакует аргументы и ждёт ответа.
static uint64_t sys_ping(uint64_t dst_ip, uint64_t id, uint64_t seq, uint64_t timeout_ms,
                         uint64_t unused4) {
    (void)unused4;

    uint32_t netd_pid = process_get_netd_pid();
    if (!netd_pid) return (uint64_t)-2;

    netd_request_t req; memset(&req, 0, sizeof(req));
    req.opcode = NETD_OP_PING;
    req.a[0] = (int32_t)dst_ip;
    req.a[1] = (int32_t)id;
    req.a[2] = (int32_t)seq;
    req.a[3] = (int32_t)timeout_ms;

    netd_reply_t reply;
    if (service_call(netd_pid, &req, sizeof(req), &reply, sizeof(reply)) != 0)
        return (uint64_t)-4;

    return (uint64_t)(int64_t)reply.result;
}

// SYS_OS_VERSION (74) — см. syscall.h.
static uint64_t sys_os_version(uint64_t buf_ptr, uint64_t unused1, uint64_t unused2,
                               uint64_t unused3, uint64_t unused4) {
    (void)unused1; (void)unused2; (void)unused3; (void)unused4;

    if (!current_process ||
        !is_user_range_valid(current_process->page_table, buf_ptr, sizeof(lufira_os_version_t), 1))
        return (uint64_t)-EFAULT;

    lufira_os_version_t out;
    out.major = LUFIRA_VERSION_MAJOR;
    out.minor = LUFIRA_VERSION_MINOR;
    out.patch = LUFIRA_VERSION_PATCH;
    memcpy(out.build_date, __DATE__, sizeof(__DATE__)); // sizeof включает NUL, помещается в [12]
    memcpy(out.build_time, __TIME__, sizeof(__TIME__)); // то же, в [9]

    memcpy((void *)buf_ptr, &out, sizeof(out));
    return 0;
}

// SYS_NET_CONFIG_GET (75) — см. syscall.h. "Карты нет"/"netd не запущен" —
// валидный ответ (link_up=0), а не -EIO (тот же стиль, что у SYS_SOUND_INFO).
static uint64_t sys_net_config_get(uint64_t buf_ptr, uint64_t unused1, uint64_t unused2,
                                   uint64_t unused3, uint64_t unused4) {
    (void)unused1; (void)unused2; (void)unused3; (void)unused4;

    if (!current_process ||
        !is_user_range_valid(current_process->page_table, buf_ptr, sizeof(lufira_net_config_t), 1))
        return (uint64_t)-EFAULT;

    lufira_net_config_t out;
    memset(&out, 0, sizeof(out));

    uint32_t netd_pid = process_get_netd_pid();
    if (netd_pid) {
        netd_request_t req; memset(&req, 0, sizeof(req));
        req.opcode = NETD_OP_CONFIG_GET;
        netd_reply_t reply;
        if (service_call(netd_pid, &req, sizeof(req), &reply, sizeof(reply)) == 0) {
            out.link_up = (uint8_t)reply.a[0];
            memcpy(out.mac, reply.mac, 6);
            out.our_ip = reply.our_ip;
            out.netmask = reply.netmask;
            out.gateway = reply.gateway;
            out.dns_server = reply.dns_server;
        }
    }

    memcpy((void *)buf_ptr, &out, sizeof(out));
    return 0;
}

// SYS_NET_CONFIG_SET (76) — см. syscall.h.
static uint64_t sys_net_config_set(uint64_t ip, uint64_t netmask, uint64_t gateway,
                                   uint64_t unused3, uint64_t unused4) {
    (void)unused3; (void)unused4;

    uint32_t netd_pid = process_get_netd_pid();
    if (!netd_pid) return 0;

    netd_request_t req; memset(&req, 0, sizeof(req));
    req.opcode = NETD_OP_CONFIG_SET;
    req.a[0] = (int32_t)ip;
    req.a[1] = (int32_t)netmask;
    req.a[2] = (int32_t)gateway;
    netd_reply_t reply;
    service_call(netd_pid, &req, sizeof(req), &reply, sizeof(reply));
    return 0;
}

// SYS_FB_INFO (60) — см. syscall.h.
static uint64_t sys_fb_info(uint64_t out_ptr, uint64_t unused1, uint64_t unused2,
                            uint64_t unused3, uint64_t unused4) {
    (void)unused1; (void)unused2; (void)unused3; (void)unused4;
    if (!current_process) return (uint64_t)-EFAULT;
    if (!is_user_range_valid(current_process->page_table, out_ptr, sizeof(lufira_fb_info_t), 1))
        return (uint64_t)-EFAULT;

    lufira_fb_info_t *out = (lufira_fb_info_t *)out_ptr;
    out->width = screen_width_pixels;
    out->height = screen_height_pixels;
    out->pixel_format = pixel_format;
    return 0;
}

// SYS_FB_PRESENT (61) — см. подробный разбор dirty_xy/dirty_wh в
// syscall.h. Только зарегистрированный WM pid.
static uint64_t sys_fb_present(uint64_t buf_ptr, uint64_t w, uint64_t h,
                               uint64_t dirty_xy, uint64_t dirty_wh) {
    if (!current_process) return (uint64_t)-EFAULT;
    if (current_process->pid != process_get_wm_pid()) return (uint64_t)-EPERM;
    if (w != screen_width_pixels || h != screen_height_pixels) return (uint64_t)-EINVAL;

    size_t bytes = (size_t)w * (size_t)h * sizeof(uint32_t);
    if (!is_user_range_valid(current_process->page_table, buf_ptr, bytes, 0))
        return (uint64_t)-EFAULT;

    uint32_t drw = (uint32_t)(dirty_wh >> 32);
    uint32_t drh = (uint32_t)(dirty_wh & 0xFFFFFFFFu);
    if (drw == 0 || drh == 0) {
        // Сентинел "нет dirty rect" — презентуем кадр целиком (прежнее
        // поведение, и то, что получается при dirty_xy=dirty_wh=0 у
        // старого/неосведомлённого вызывающего).
        gfx_blit(0, 0, (const uint32_t *)buf_ptr, (uint32_t)w, (uint32_t)h, (uint32_t)w);
        return 0;
    }

    int32_t dx = (int32_t)(dirty_xy >> 32);
    int32_t dy = (int32_t)(dirty_xy & 0xFFFFFFFFu);
    // buf_ptr — ПОЛНЫЙ кадр со stride==w (проверено выше), dirty-прямоугольник
    // просто его под-окно — та же арифметика смещения, что и у клиента
    // (lufira-packages/apps/wm.c, copy_rect_from_fb()) при подготовке буфера.
    if (dx < 0 || dy < 0 || (uint32_t)dx >= w || (uint32_t)dy >= h ||
        drw > w - (uint32_t)dx || drh > h - (uint32_t)dy)
        return (uint64_t)-EINVAL;

    const uint32_t *sub = (const uint32_t *)buf_ptr + (size_t)dy * w + (size_t)dx;
    gfx_blit(dx, dy, sub, drw, drh, (uint32_t)w);
    return 0;
}

// SYS_FB_FONT (62) — см. syscall.h.
static uint64_t sys_fb_font(uint64_t out_ptr, uint64_t max_bytes, uint64_t unused2,
                            uint64_t unused3, uint64_t unused4) {
    (void)unused2; (void)unused3; (void)unused4;
    if (!current_process) return (uint64_t)-EFAULT;
    if (max_bytes > 0 && !is_user_range_valid(current_process->page_table, out_ptr, max_bytes, 1))
        return (uint64_t)-EFAULT;

    console_get_font_data((void *)out_ptr, (uint32_t)max_bytes);
    return (uint64_t)console_get_font_size();
}

// SYS_DUP2 (65) — см. syscall.h.
static uint64_t sys_dup2(uint64_t oldfd, uint64_t newfd, uint64_t unused2,
                         uint64_t unused3, uint64_t unused4) {
    (void)unused2; (void)unused3; (void)unused4;
    if (!current_process) return (uint64_t)-EFAULT;
    int res = vfs_dup2((int)oldfd, (int)newfd);
    return (res >= 0) ? (uint64_t)res : (uint64_t)-1;
}

// SYS_NET_FETCH (66) — см. syscall.h. Вся работа (URL/DNS/TCP/TLS/HTTP)
// внутри http_fetch() (net/http_client.c); здесь — только проверка
// пользовательских указателей и проброс результата. Отрицательные коды
// HTTP_FETCH_E* уходят в userspace КАК ЕСТЬ: они численно совпадают с
// NET_FETCH_E* из libc/include/lufira/syscall.h (см. комментарий об этом
// в net/http_client.h), поэтому перекодировать их не нужно и нельзя.
// NET_FETCH_E* — должны численно совпадать с libc/include/lufira/syscall.h
// (dlpg.c сравнивает результат sys_net_fetch() с ними напрямую) и с тем,
// что теперь возвращает http_fetch() ВНУТРИ netd (daemons/netd/
// http_client.h) — то же соглашение, что было здесь до переноса в
// userspace. Только ENODEV нужен прямо в ядре (netd не зарегистрирован);
// остальные коды просто проходят через reply.result как есть.
#define NET_FETCH_ENODEV (-7)

// Потолок out_cap — ограничивает, сколько физических страниц уйдёт под
// shm-область ниже (должен помещаться в MAX_SHARED_REGION_PAGES, shm.h).
#define NET_FETCH_MAX_CAP (MAX_SHARED_REGION_PAGES * PAGE_SIZE)

static uint64_t sys_net_fetch(uint64_t url_ptr, uint64_t out_buf_ptr, uint64_t out_cap,
                              uint64_t status_ptr, uint64_t unused4) {
    (void)unused4;
    if (!current_process) return (uint64_t)-EFAULT;

    if (validate_user_string(current_process->page_table, url_ptr, USER_STRING_MAX) < 0)
        return (uint64_t)-EFAULT;

    if (out_cap > 0 &&
        !is_user_range_valid(current_process->page_table, out_buf_ptr, out_cap, 1))
        return (uint64_t)-EFAULT;

    if (status_ptr != 0 &&
        !is_user_range_valid(current_process->page_table, status_ptr, sizeof(int), 1))
        return (uint64_t)-EFAULT;

    if (out_cap > NET_FETCH_MAX_CAP) return (uint64_t)-EINVAL;

    uint32_t netd_pid = process_get_netd_pid();
    if (!netd_pid) {
        if (status_ptr != 0) *(int *)status_ptr = 0;
        return (uint64_t)(int64_t)NET_FETCH_ENODEV;
    }

    // Тело ответа (килобайты-мегабайты) не умещается в 96-байтный payload
    // mailbox'а — заводим shm-область под out_cap байт: netd сам ATTACH её
    // (SYS_SHM_ATTACH) и пишет http_fetch() прямо туда; после ответа ядро
    // читает готовые байты напрямую через phys_to_virt() — универсальный
    // доступ ядра к любой физической странице, ATTACH с его стороны не
    // нужен (в отличие от netd, у ядра нет собственных "page tables
    // пользовательского процесса", только одна общая physmap).
    uint64_t num_pages = out_cap == 0 ? 0 : (out_cap + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t *pages = NULL;
    int shm_id = -1;

    if (num_pages > 0) {
        pages = (uint64_t *)kmalloc(num_pages * sizeof(uint64_t));
        if (!pages) return (uint64_t)-1;

        uint64_t allocated;
        for (allocated = 0; allocated < num_pages; allocated++) {
            uint64_t phys = pmm_alloc_page();
            if (!phys) break;
            pages[allocated] = phys;
        }
        if (allocated < num_pages) {
            for (uint64_t i = 0; i < allocated; i++) pmm_free_page(pages[i]);
            kfree(pages);
            return (uint64_t)-1;
        }

        shm_id = shm_create(pages, num_pages);
        if (shm_id < 0) {
            for (uint64_t i = 0; i < num_pages; i++) pmm_free_page(pages[i]);
            kfree(pages);
            return (uint64_t)-1;
        }
    }

    netd_request_t req; memset(&req, 0, sizeof(req));
    req.opcode = NETD_OP_FETCH;
    req.shm_id = (uint32_t)(shm_id >= 0 ? shm_id : 0);
    req.cap = (uint32_t)out_cap;
    size_t i = 0;
    const char *url = (const char *)url_ptr;
    for (; url[i] && i < sizeof(req.str) - 1; i++) req.str[i] = url[i];
    req.str[i] = '\0';

    netd_reply_t reply;
    int rc = service_call(netd_pid, &req, sizeof(req), &reply, sizeof(reply));

    int result;
    int status = 0;
    if (rc != 0) {
        // netd не ответил (умер во время обработки этого самого запроса —
        // то же известное ограничение, что у wm_call()/service_call()).
        result = NET_FETCH_ENODEV;
    } else {
        result = reply.result;
        status = reply.a[0];
        if (result > 0 && pages) {
            uint32_t remaining = (uint32_t)result;
            uint32_t off = 0;
            for (uint64_t p = 0; p < num_pages && remaining > 0; p++) {
                uint32_t take = remaining < PAGE_SIZE ? remaining : (uint32_t)PAGE_SIZE;
                memcpy((void *)(out_buf_ptr + off), phys_to_virt(pages[p]), take);
                off += take;
                remaining -= take;
            }
        }
    }

    if (shm_id >= 0) {
        // Снимает "неявную" долю ядра из refcount'а (shm_create() стартует
        // его с num_pages, представляя РОВНО это — см. комментарий в
        // shm.h): netd уже снял свою через SYS_MUNMAP (main.c) к моменту
        // ответа, так что это последнее снятие и реально освобождает
        // физические страницы через pmm_free_page() внутри shm_release().
        for (uint64_t i2 = 0; i2 < num_pages; i2++) shm_release(shm_id);
    }
    if (pages) kfree(pages);

    if (status_ptr != 0) *(int *)status_ptr = status;
    return (uint64_t)(int64_t)result;
}

// ===== Инфраструктура userspace-драйверов (driver.h) — см. подробные
// комментарии у каждого SYS_DRIVER_*/SYS_PCI_*/SYS_IO_*/SYS_MMAP_MMIO/
// SYS_DMA_ALLOC/SYS_IRQ_*/SYS_SHM_* в syscall.h. =====

static void bdf_from_packed(uint64_t packed, uint8_t *bus, uint8_t *device, uint8_t *function) {
    *bus = (uint8_t)(packed >> 16);
    *device = (uint8_t)(packed >> 8);
    *function = (uint8_t)packed;
}

static void bdf_offset_from_packed(uint64_t packed, uint8_t *bus, uint8_t *device,
                                   uint8_t *function, uint8_t *offset) {
    *bus = (uint8_t)(packed >> 24);
    *device = (uint8_t)(packed >> 16);
    *function = (uint8_t)(packed >> 8);
    *offset = (uint8_t)packed;
}

static void fill_lufira_pci_device(const pci_device_t *dev, lufira_pci_device_t *out) {
    memset(out, 0, sizeof(*out));
    out->bus = dev->bus; out->device = dev->device; out->function = dev->function;
    out->interrupt_line = dev->interrupt_line;
    out->vendor_id = dev->vendor_id; out->device_id = dev->device_id;
    for (int i = 0; i < 6; i++) {
        pci_bar_t bar;
        if (pci_get_bar(dev, (uint8_t)i, &bar) == 0) {
            out->bars[i].address = bar.address;
            out->bars[i].size = bar.size;
            out->bars[i].is_io = bar.is_io;
        }
    }
}

// SYS_DRIVER_CLAIM_DEVICE (77).
static uint64_t sys_driver_claim_device(uint64_t vendor_id, uint64_t device_id, uint64_t out_ptr,
                                        uint64_t unused3, uint64_t unused4) {
    (void)unused3; (void)unused4;
    if (!current_process) return (uint64_t)-EFAULT;
    if (current_process->uid != 0) return (uint64_t)-EPERM;
    if (!is_user_range_valid(current_process->page_table, out_ptr, sizeof(lufira_pci_device_t), 1))
        return (uint64_t)-EFAULT;

    pci_device_t dev;
    int rc = driver_claim_device(current_process->pid, (uint16_t)vendor_id, (uint16_t)device_id, &dev);
    if (rc == -2) return (uint64_t)-EAGAIN;
    if (rc != 0) return (uint64_t)-ENOENT;

    lufira_pci_device_t out;
    fill_lufira_pci_device(&dev, &out);
    memcpy((void *)out_ptr, &out, sizeof(out));
    return 0;
}

// SYS_DRIVER_CLAIM_DEVICE_BY_CLASS (91).
static uint64_t sys_driver_claim_device_by_class(uint64_t class_code, uint64_t subclass, uint64_t out_ptr,
                                                 uint64_t unused3, uint64_t unused4) {
    (void)unused3; (void)unused4;
    if (!current_process) return (uint64_t)-EFAULT;
    if (current_process->uid != 0) return (uint64_t)-EPERM;
    if (!is_user_range_valid(current_process->page_table, out_ptr, sizeof(lufira_pci_device_t), 1))
        return (uint64_t)-EFAULT;

    pci_device_t dev;
    int rc = driver_claim_device_by_class(current_process->pid, (uint8_t)class_code, (uint8_t)subclass, &dev);
    if (rc == -2) return (uint64_t)-EAGAIN;
    if (rc != 0) return (uint64_t)-ENOENT;

    lufira_pci_device_t out;
    fill_lufira_pci_device(&dev, &out);
    memcpy((void *)out_ptr, &out, sizeof(out));
    return 0;
}

// SYS_DRIVER_RELEASE_DEVICE (78).
static uint64_t sys_driver_release_device(uint64_t bdf_packed, uint64_t unused1, uint64_t unused2,
                                          uint64_t unused3, uint64_t unused4) {
    (void)unused1; (void)unused2; (void)unused3; (void)unused4;
    if (!current_process) return (uint64_t)-EFAULT;

    uint8_t bus, device, function;
    bdf_from_packed(bdf_packed, &bus, &device, &function);
    if (driver_release_device(current_process->pid, bus, device, function) != 0)
        return (uint64_t)-EPERM;
    return 0;
}

// SYS_PCI_CONFIG_READ (79).
static uint64_t sys_pci_config_read(uint64_t bdf_offset_packed, uint64_t width, uint64_t out_ptr,
                                    uint64_t unused3, uint64_t unused4) {
    (void)unused3; (void)unused4;
    if (!current_process) return (uint64_t)-EFAULT;
    if (!is_user_range_valid(current_process->page_table, out_ptr, sizeof(uint32_t), 1))
        return (uint64_t)-EFAULT;

    uint8_t bus, device, function, offset;
    bdf_offset_from_packed(bdf_offset_packed, &bus, &device, &function, &offset);
    if (!driver_owns_device(current_process->pid, bus, device, function))
        return (uint64_t)-EPERM;

    uint32_t value;
    switch (width) {
        case 1: value = pci_config_read8(bus, device, function, offset); break;
        case 2: value = pci_config_read16(bus, device, function, offset); break;
        case 4: value = pci_config_read32(bus, device, function, offset); break;
        default: return (uint64_t)-EINVAL;
    }
    *(uint32_t *)out_ptr = value;
    return 0;
}

// SYS_PCI_CONFIG_WRITE (80).
static uint64_t sys_pci_config_write(uint64_t bdf_offset_packed, uint64_t width, uint64_t value,
                                     uint64_t unused3, uint64_t unused4) {
    (void)unused3; (void)unused4;
    if (!current_process) return (uint64_t)-EFAULT;

    uint8_t bus, device, function, offset;
    bdf_offset_from_packed(bdf_offset_packed, &bus, &device, &function, &offset);
    if (!driver_owns_device(current_process->pid, bus, device, function))
        return (uint64_t)-EPERM;

    switch (width) {
        case 1: pci_config_write8(bus, device, function, offset, (uint8_t)value); break;
        case 2: pci_config_write16(bus, device, function, offset, (uint16_t)value); break;
        case 4: pci_config_write32(bus, device, function, offset, (uint32_t)value); break;
        default: return (uint64_t)-EINVAL;
    }
    return 0;
}

// SYS_IO_IN (81).
static uint64_t sys_io_in(uint64_t port, uint64_t width, uint64_t out_ptr,
                          uint64_t unused3, uint64_t unused4) {
    (void)unused3; (void)unused4;
    if (!current_process) return (uint64_t)-EFAULT;
    if (width != 1 && width != 2 && width != 4) return (uint64_t)-EINVAL;
    if (port > 0xFFFF) return (uint64_t)-EINVAL;
    if (!is_user_range_valid(current_process->page_table, out_ptr, sizeof(uint32_t), 1))
        return (uint64_t)-EFAULT;
    if (!driver_io_port_allowed(current_process->pid, (uint16_t)port, (uint8_t)width))
        return (uint64_t)-EPERM;

    uint32_t value;
    switch (width) {
        case 1: { uint8_t v; asm volatile("inb %1, %0" : "=a"(v) : "Nd"((uint16_t)port)); value = v; break; }
        case 2: { uint16_t v; asm volatile("inw %1, %0" : "=a"(v) : "Nd"((uint16_t)port)); value = v; break; }
        default: { uint32_t v; asm volatile("inl %1, %0" : "=a"(v) : "Nd"((uint16_t)port)); value = v; break; }
    }
    *(uint32_t *)out_ptr = value;
    return 0;
}

// SYS_IO_OUT (82).
static uint64_t sys_io_out(uint64_t port, uint64_t width, uint64_t value,
                           uint64_t unused3, uint64_t unused4) {
    (void)unused3; (void)unused4;
    if (!current_process) return (uint64_t)-EFAULT;
    if (width != 1 && width != 2 && width != 4) return (uint64_t)-EINVAL;
    if (port > 0xFFFF) return (uint64_t)-EINVAL;
    if (!driver_io_port_allowed(current_process->pid, (uint16_t)port, (uint8_t)width))
        return (uint64_t)-EPERM;

    switch (width) {
        case 1: asm volatile("outb %0, %1" : : "a"((uint8_t)value), "Nd"((uint16_t)port)); break;
        case 2: asm volatile("outw %0, %1" : : "a"((uint16_t)value), "Nd"((uint16_t)port)); break;
        default: asm volatile("outl %0, %1" : : "a"((uint32_t)value), "Nd"((uint16_t)port)); break;
    }
    return 0;
}

// SYS_MMAP_MMIO (83). Переиспользует mmap_regions[]/next_mmap_addr — те же
// слоты, что sys_mmap()/sys_dma_alloc()/sys_shm_create(), поэтому
// SYS_MUNMAP(base, length) позже корректно снимет маппинг без утечки VA
// (тот же механизм, что уже используется для обычного MAP_ANONYMOUS).
static uint64_t sys_mmap_mmio(uint64_t phys_addr, uint64_t length, uint64_t unused2,
                              uint64_t unused3, uint64_t unused4) {
    (void)unused2; (void)unused3; (void)unused4;
    if (!current_process || length == 0) return (uint64_t)-1;
    if (!driver_mmio_allowed(current_process->pid, phys_addr, length)) return (uint64_t)-1;

    int slot = -1;
    for (int i = 0; i < MAX_MMAP_REGIONS; i++) {
        if (current_process->mmap_regions[i].length == 0) { slot = i; break; }
    }
    if (slot < 0) return (uint64_t)-1;

    uint64_t phys_page = phys_addr & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t phys_offset = phys_addr - phys_page;
    uint64_t map_len = (phys_offset + length + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t num_pages = map_len / PAGE_SIZE;

    uint64_t base = current_process->next_mmap_addr;
    uint64_t pml4_phys = current_process->page_table;
    uint64_t page_flags = PAGE_PRESENT | PAGE_WRITE | PAGE_USER | PAGE_PCD | PAGE_MMIO_NOFREE;

    for (uint64_t i = 0; i < num_pages; i++) {
        if (map_page_in_pml4(pml4_phys, base + i * PAGE_SIZE, phys_page + i * PAGE_SIZE, page_flags) != 0) {
            for (uint64_t j = 0; j < i; j++) unmap_page(base + j * PAGE_SIZE);
            return (uint64_t)-1;
        }
    }

    current_process->next_mmap_addr += map_len;
    current_process->mmap_regions[slot].addr = base;
    current_process->mmap_regions[slot].length = map_len;
    current_process->mmap_regions[slot].shared_id = -1;

    return base + phys_offset;
}

// SYS_DMA_ALLOC (84).
#define MAX_DMA_ALLOC_PAGES 4096 // 16MB — с большим запасом над любым кольцом дескрипторов

static uint64_t sys_dma_alloc(uint64_t num_pages, uint64_t phys_out_ptr, uint64_t unused2,
                              uint64_t unused3, uint64_t unused4) {
    (void)unused2; (void)unused3; (void)unused4;
    if (!current_process || num_pages == 0 || num_pages > MAX_DMA_ALLOC_PAGES) return (uint64_t)-1;
    if (!driver_is_registered(current_process->pid)) return (uint64_t)-1;
    if (!is_user_range_valid(current_process->page_table, phys_out_ptr, sizeof(uint64_t), 1))
        return (uint64_t)-1;

    int slot = -1;
    for (int i = 0; i < MAX_MMAP_REGIONS; i++) {
        if (current_process->mmap_regions[i].length == 0) { slot = i; break; }
    }
    if (slot < 0) return (uint64_t)-1;

    uint64_t phys_base = pmm_alloc_contiguous_pages((uint32_t)num_pages);
    if (!phys_base) return (uint64_t)-1;

    uint64_t base = current_process->next_mmap_addr;
    uint64_t pml4_phys = current_process->page_table;
    uint64_t page_flags = PAGE_PRESENT | PAGE_WRITE | PAGE_USER;

    uint64_t mapped;
    for (mapped = 0; mapped < num_pages; mapped++) {
        uint64_t virt = base + mapped * PAGE_SIZE;
        if (map_page_in_pml4(pml4_phys, virt, phys_base + mapped * PAGE_SIZE, page_flags) != 0)
            break;
        memset((void *)virt, 0, PAGE_SIZE);
    }
    if (mapped < num_pages) {
        // [0, mapped) уже замаплены обычной (не shared, не MMIO) памятью —
        // unmap_page() корректно освободит их через pmm_free_page(). Страницы
        // [mapped, num_pages) ещё НЕ замаплены вообще — их нужно освободить
        // напрямую, по одной (так же, как их выделил pmm_alloc_contiguous_pages(),
        // см. pmm.h).
        for (uint64_t i = 0; i < mapped; i++) unmap_page(base + i * PAGE_SIZE);
        for (uint64_t i = mapped; i < num_pages; i++) pmm_free_page(phys_base + i * PAGE_SIZE);
        return (uint64_t)-1;
    }

    current_process->next_mmap_addr += num_pages * PAGE_SIZE;
    current_process->mmap_regions[slot].addr = base;
    current_process->mmap_regions[slot].length = num_pages * PAGE_SIZE;
    current_process->mmap_regions[slot].shared_id = -1;

    *(uint64_t *)phys_out_ptr = phys_base;
    return base;
}

// SYS_IRQ_REGISTER (85).
static uint64_t sys_irq_register(uint64_t irq_line, uint64_t unused1, uint64_t unused2,
                                 uint64_t unused3, uint64_t unused4) {
    (void)unused1; (void)unused2; (void)unused3; (void)unused4;
    if (!current_process) return (uint64_t)-EFAULT;

    // Вызывающий обязан владеть устройством, чей PCI_INTERRUPT_LINE равен
    // irq_line — иначе любой зарегистрированный драйвер мог бы подписаться
    // на чужую линию (например линию диска, который остаётся в ядре).
    int matches = 0;
    uint32_t count = pci_get_device_count();
    for (uint32_t i = 0; i < count && !matches; i++) {
        const pci_device_t *dev = pci_get_device(i);
        if (dev && dev->interrupt_line == (uint8_t)irq_line &&
            driver_owns_device(current_process->pid, dev->bus, dev->device, dev->function))
            matches = 1;
    }
    if (!matches) return (uint64_t)-EPERM;

    int rc = driver_irq_register(current_process->pid, (uint8_t)irq_line);
    if (rc == -1) return (uint64_t)-EINVAL;
    if (rc == -2) return (uint64_t)-EPERM;
    return 0;
}

// SYS_IRQ_WAIT (86).
static uint64_t sys_irq_wait(uint64_t irq_line, uint64_t unused1, uint64_t unused2,
                             uint64_t unused3, uint64_t unused4) {
    (void)unused1; (void)unused2; (void)unused3; (void)unused4;
    if (!current_process) return (uint64_t)-EFAULT;
    if (driver_irq_wait(current_process->pid, (uint8_t)irq_line) != 0)
        return (uint64_t)-EPERM;
    return 0;
}

// SYS_SHM_CREATE (87).
static uint64_t sys_shm_create(uint64_t num_pages, uint64_t id_out_ptr, uint64_t unused2,
                               uint64_t unused3, uint64_t unused4) {
    (void)unused2; (void)unused3; (void)unused4;
    if (!current_process || num_pages == 0 || num_pages > MAX_SHARED_REGION_PAGES)
        return (uint64_t)-1;
    if (!is_user_range_valid(current_process->page_table, id_out_ptr, sizeof(int32_t), 1))
        return (uint64_t)-1;

    int slot = -1;
    for (int i = 0; i < MAX_MMAP_REGIONS; i++) {
        if (current_process->mmap_regions[i].length == 0) { slot = i; break; }
    }
    if (slot < 0) return (uint64_t)-1;

    // kmalloc(), не стековый массив — см. комментарий у того же приёма в
    // sys_mmap() (MAX_SHARED_REGION_PAGES=4096, v0.9.5, не влезает в
    // 16KB-ный кернел-стек, KERNEL_STACK_SIZE в process.h).
    uint64_t *pages = (uint64_t *)kmalloc(num_pages * sizeof(uint64_t));
    if (!pages) return (uint64_t)-1;

    uint64_t base = current_process->next_mmap_addr;
    uint64_t pml4_phys = current_process->page_table;
    uint64_t page_flags = PAGE_PRESENT | PAGE_WRITE | PAGE_USER | PAGE_MMAP_SHARED;

    uint64_t mapped;
    for (mapped = 0; mapped < num_pages; mapped++) {
        uint64_t phys = pmm_alloc_page();
        if (!phys) break;
        uint64_t virt = base + mapped * PAGE_SIZE;
        if (map_page_in_pml4(pml4_phys, virt, phys, page_flags) != 0) {
            pmm_free_page(phys);
            break;
        }
        memset((void *)virt, 0, PAGE_SIZE);
        pages[mapped] = phys;
    }
    if (mapped < num_pages) {
        // shm_create() ещё не звали — unmap_page() корректно откатится на
        // pmm_free_page() (тот же приём, что у sys_mmap(), см. его комментарий).
        for (uint64_t i = 0; i < mapped; i++) unmap_page(base + i * PAGE_SIZE);
        kfree(pages);
        return (uint64_t)-1;
    }

    int shm_id = shm_create(pages, num_pages);
    kfree(pages); // shm_create() уже скопировало всё, что нужно, в свой реестр
    if (shm_id < 0) {
        for (uint64_t i = 0; i < num_pages; i++) unmap_page(base + i * PAGE_SIZE);
        return (uint64_t)-1;
    }

    uint64_t len = num_pages * PAGE_SIZE;
    current_process->next_mmap_addr += len;
    current_process->mmap_regions[slot].addr = base;
    current_process->mmap_regions[slot].length = len;
    current_process->mmap_regions[slot].shared_id = shm_id;

    *(int32_t *)id_out_ptr = shm_id;
    return base;
}

// SYS_SHM_ATTACH (88).
static uint64_t sys_shm_attach(uint64_t shm_id, uint64_t unused1, uint64_t unused2,
                               uint64_t unused3, uint64_t unused4) {
    (void)unused1; (void)unused2; (void)unused3; (void)unused4;
    if (!current_process) return (uint64_t)-1;

    uint64_t num_pages = shm_get_num_pages((int)shm_id);
    if (num_pages == 0) return (uint64_t)-1;

    // kmalloc(), не стековый массив — см. тот же приём и комментарий в
    // sys_mmap()/sys_shm_create(). Ровно num_pages, не весь
    // MAX_SHARED_REGION_PAGES потолок.
    uint64_t *pages = (uint64_t *)kmalloc(num_pages * sizeof(uint64_t));
    if (!pages) return (uint64_t)-1;
    if (shm_get_pages((int)shm_id, pages, num_pages, &num_pages) != 0) {
        kfree(pages);
        return (uint64_t)-1;
    }

    int slot = -1;
    for (int i = 0; i < MAX_MMAP_REGIONS; i++) {
        if (current_process->mmap_regions[i].length == 0) { slot = i; break; }
    }
    if (slot < 0) { kfree(pages); return (uint64_t)-1; }

    uint64_t base = current_process->next_mmap_addr;
    uint64_t pml4_phys = current_process->page_table;
    uint64_t page_flags = PAGE_PRESENT | PAGE_WRITE | PAGE_USER | PAGE_MMAP_SHARED;

    for (uint64_t i = 0; i < num_pages; i++) {
        if (map_page_in_pml4(pml4_phys, base + i * PAGE_SIZE, pages[i], page_flags) != 0) {
            for (uint64_t j = 0; j < i; j++) unmap_page(base + j * PAGE_SIZE);
            kfree(pages);
            return (uint64_t)-1;
        }
        // +1 за КАЖДУЮ реально замапленную страницу, сразу по месту — не
        // единой пачкой в конце: refcount считается постранично (shm.h), и
        // при частичном откате выше unmap_page() должен видеть ровно тот же
        // счётчик, что сам же и выставил, иначе отпустит чужую ссылку
        // (создателя региона), которую эта попытка attach ещё не брала.
        shm_add_ref((int)shm_id);
    }
    kfree(pages);

    uint64_t len = num_pages * PAGE_SIZE;
    current_process->next_mmap_addr += len;
    current_process->mmap_regions[slot].addr = base;
    current_process->mmap_regions[slot].length = len;
    current_process->mmap_regions[slot].shared_id = (int)shm_id;

    return base;
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
    [SYS_POLL] = sys_poll,
    [SYS_SIGACTION] = sys_sigaction,
    [SYS_ALARM] = sys_alarm,
    [SYS_GET_FOREGROUND] = sys_get_foreground,
    [SYS_WIN_CREATE] = sys_win_create,
    [SYS_WIN_DESTROY] = sys_win_destroy,
    [SYS_WIN_FILL] = sys_win_fill,
    [SYS_WIN_DRAW_RECT] = sys_win_draw_rect,
    [SYS_WIN_DRAW_TEXT] = sys_win_draw_text,
    [SYS_WIN_POLL_EVENT] = sys_win_poll_event,
    [SYS_WIN_MOVE] = sys_win_move,
    [SYS_IPC_SEND] = sys_ipc_send,
    [SYS_IPC_RECV] = sys_ipc_recv,
    [SYS_WM_REGISTER] = sys_wm_register,
    [SYS_FB_INFO] = sys_fb_info,
    [SYS_FB_PRESENT] = sys_fb_present,
    [SYS_FB_FONT] = sys_fb_font,
    [SYS_DUP2] = sys_dup2,
    [SYS_NET_FETCH] = sys_net_fetch,
    [SYS_RTC_GETTIME] = sys_rtc_gettime,
    [SYS_SOUND_INFO] = sys_sound_info,
    [SYS_SOUND_SET_VOLUME] = sys_sound_set_volume,
    [SYS_SOUND_PLAY_PCM] = sys_sound_play_pcm,
    [SYS_WM_UNREGISTER] = sys_wm_unregister,
    [SYS_SOUND_STOP] = sys_sound_stop,
    [SYS_PING] = sys_ping,
    [SYS_OS_VERSION] = sys_os_version,
    [SYS_NET_CONFIG_GET] = sys_net_config_get,
    [SYS_NET_CONFIG_SET] = sys_net_config_set,
    [SYS_DRIVER_CLAIM_DEVICE] = sys_driver_claim_device,
    [SYS_DRIVER_RELEASE_DEVICE] = sys_driver_release_device,
    [SYS_PCI_CONFIG_READ] = sys_pci_config_read,
    [SYS_PCI_CONFIG_WRITE] = sys_pci_config_write,
    [SYS_IO_IN] = sys_io_in,
    [SYS_IO_OUT] = sys_io_out,
    [SYS_MMAP_MMIO] = sys_mmap_mmio,
    [SYS_DMA_ALLOC] = sys_dma_alloc,
    [SYS_IRQ_REGISTER] = sys_irq_register,
    [SYS_IRQ_WAIT] = sys_irq_wait,
    [SYS_SHM_CREATE] = sys_shm_create,
    [SYS_SHM_ATTACH] = sys_shm_attach,
    [SYS_SOUNDD_REGISTER] = sys_soundd_register,
    [SYS_NETD_REGISTER] = sys_netd_register,
    [SYS_SOUND_CONSUME] = sys_sound_consume,
    [SYS_DRIVER_CLAIM_DEVICE_BY_CLASS] = sys_driver_claim_device_by_class,
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

    // SYS_SOUND_PLAY_PCM/SYS_SOUND_CONSUME — см. комментарий у g_sound_ring
    // выше про то, почему не статический массив.
    g_sound_ring = (int16_t *)kmalloc((size_t)SOUND_RING_FRAMES * 2 * sizeof(int16_t));
    if (g_sound_ring) memset(g_sound_ring, 0, (size_t)SOUND_RING_FRAMES * 2 * sizeof(int16_t));
    
    // Число не выводится из syscall.h автоматически (нет единого SYS_COUNT —
    // номера SYS_* просто #define'ы) — бумить вручную при добавлении нового
    // syscall'а.
    DLOG("[SYSCALL] 75 system calls registered\n");
}

// ========== ДИСПАТЧЕР ==========

uint64_t syscall_handler(uint64_t syscall_num, uint64_t arg1, uint64_t arg2,
                         uint64_t arg3, uint64_t arg4, uint64_t arg5,
                         uint64_t frame_ptr) {
    uint64_t result;

    // SYS_FORK — особый случай: нужен указатель на весь сохранённый кадр
    // регистров, не только 5 обычных аргументов.
    if (syscall_num == SYS_FORK) {
        result = process_fork(frame_ptr);
    }
    // SYS_SIGRETURN — тот же повод: process_sigreturn() переписывает
    // сохранённый кадр напрямую.
    else if (syscall_num == SYS_SIGRETURN) {
        result = process_sigreturn(frame_ptr);
    }
    else if (syscall_num >= 256 || !syscall_table[syscall_num]) {
        printf("[SYSCALL] Unknown: %u\n", (uint32_t)syscall_num);
        result = (uint64_t)-1;
    }
    else {
        result = syscall_table[syscall_num](arg1, arg2, arg3, arg4, arg5);
    }

    // Доставка отложенного сигнала здесь, а не в switch_to_process():
    // frame_ptr тут всегда настоящая ring3-точка возврата этого syscall'а,
    // в отличие от process_t.context, который может застать процесс где
    // угодно в середине кернела (подмена context напрямую падает в page
    // fault, см. process_sigreturn()).
    process_deliver_pending_signal(frame_ptr);

    return result;
}
