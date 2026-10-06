// dynlink.c — v0.8-мост, пункт 8. Архитектура и границы см. в dynlink.h.
//
// ПРОВЕРЕНО живым readelf/objdump-разбором (см. комментарии ниже у
// R_X86_64_COPY и у GOT-заполнения) ДО написания этого файла — toolchain
// здесь обычный Ubuntu gcc/binutils (gcc 13, GNU ld 2.42), без каких-либо
// самодельных ELF-расширений:
//   - gcc по умолчанию кладёт вызовы через .plt.sec (endbr64; jmp
//     *GOT-слот) — НЕ через классический lazy .plt (push; jmp PLT0), так
//     что заранее заполненный GOT отрабатывает напрямую, резолвер не
//     нужен вовсе (см. подробное объяснение в dynlink.h).
//   - прямая (не через указатель) ссылка non-PIE исполняемого файла на
//     ДАННЫЕ из .so транслируется ld'ом в R_X86_64_COPY, а не
//     R_X86_64_GLOB_DAT — поддержан отдельно, хотя сегодняшний libc.so
//     не экспортирует ни одной именно ТАКОЙ переменной (только функции +
//     возможные будущие).
//   - --hash-style=sysv (обязателен при сборке libc.so, см. build.py) даёт
//     классический DT_HASH, чьё ВТОРОЕ 32-битное слово (nchain) равно
//     ровно числу записей .dynsym — так выясняется количество экспортных
//     символов без обхода секционных заголовков (которых рантайм-загрузчик
//     в принципе не обязан видеть, только программные).

#include "dynlink.h"
#include "system/mm/pmm.h"
#include "system/mm/paging.h"
#include "system/mm/heap.h"
#include "system/mm/shm.h"
#include "system/process/process.h"
#include "fs/lufirafs/lufirafs.h"
#include "drivers/console/console.h"
#include "lib/stddef.h"
#include "lib/string.h"

extern lufirafs_t lufirafs;

#define DYNLINK_MAX_SEGMENTS 16

typedef struct {
    uint64_t vaddr_offset; // смещение от DYNLINK_LIBC_BASE (исходный vaddr сегмента - у ET_DYN он "с нуля")
    uint64_t memsz;        // округлено до страниц
    int writable;          // PF_W - приватная копия на процесс, иначе общая
    int executable;        // PF_X - без PAGE_NX
    uint64_t *phys_pages;  // kmalloc'd массив, по одной физ. странице на страницу сегмента
    uint64_t num_pages;
    int shm_id;            // shm.c id для !writable сегментов, иначе -1
} dynlink_segment_t;

typedef struct {
    int ready;
    dynlink_segment_t segments[DYNLINK_MAX_SEGMENTS];
    int seg_count;

    // Файл libc.so целиком хранится НАВСЕГДА (никогда не kfree()) -
    // dynsym/dynstr ниже это просто указатели ВНУТРЬ него, а не отдельные
    // копии.
    const uint8_t *file_buf;
    uint64_t file_size;

    const elf64_sym_t *dynsym;
    uint32_t dynsym_count;
    const char *dynstr;
    uint64_t dynstr_size;
} libc_cache_t;

static libc_cache_t g_libc;

// Откатывает ЛЮБУЮ частично проделанную работу dynlink_load_libc_cache()
// на любом из её путей отказа (нет /lib/libc.so, OOM на какой-то из
// страниц, переполнен shm.c) — НАЙДЕНО при аудите по просьбе пользователя
// ("audit shm.c for other per-page refcount mismatches"): без этой
// функции g_libc (статический, живёт между вызовами) после ПЕРВОЙ
// неудачной попытки оставался в частично заполненном состоянии
// (g_libc.seg_count > 0, но g_libc.ready всё ещё 0) — ЛЮБОЙ следующий
// exec() динамического бинарника заново звал бы dynlink_load_libc_cache()
// (g_libc.ready всё ещё ложно) и начинал писать сегменты СНОВА начиная с
// уже ненулевого g_libc.seg_count, затирая/портя указатели на уже
// "удержанные" (но никогда не освобождённые) физические страницы и shm-
// регионы предыдущей попытки — т.е. не просто редкая утечка, а порча
// состояния при первом же повторном exec() после единственного сбоя.
// Для сегментов, УЖЕ зарегистрированных в shm.c (seg->shm_id >= 0) —
// именно num_pages вызовов shm_release() корректно уводит refcount с
// num_pages (см. комментарий у shm_create() в shm.h) до нуля одним
// чистым проходом, тем же путём, что и обычный teardown процесса; для
// ещё не зарегистрированных — страницы освобождаются напрямую, т.к. их
// ещё никто, кроме этой самой функции, не видел.
static void dynlink_cache_rollback(void) {
    for (int s = 0; s < g_libc.seg_count; s++) {
        dynlink_segment_t *seg = &g_libc.segments[s];
        if (!seg->phys_pages) continue;

        if (seg->shm_id >= 0) {
            for (uint64_t p = 0; p < seg->num_pages; p++) shm_release(seg->shm_id);
        } else {
            for (uint64_t p = 0; p < seg->num_pages; p++) {
                if (seg->phys_pages[p]) pmm_free_page(seg->phys_pages[p]);
            }
        }
        kfree(seg->phys_pages);
        seg->phys_pages = NULL;
        seg->shm_id = -1;
    }
    g_libc.seg_count = 0;
    g_libc.dynsym = NULL;
    g_libc.dynsym_count = 0;
    g_libc.dynstr = NULL;
    g_libc.dynstr_size = 0;
}

// Переводит виртуальный адрес (как он значится в ЭТОМ ЖЕ elf_data/ph/
// phnum) в указатель внутрь elf_data — файл это один сплошной kmalloc'd
// буфер, так что никаких забот о границах физических страниц тут нет (в
// отличие от уже ЗАМАПЛЕННОЙ в процесс памяти, см. dynlink_apply_
// relocations() ниже, где они есть). Работает одинаково что для vaddr
// исполняемого файла (абсолютные, т.к. он не PIE), что для vaddr libc.so
// (от нуля, т.к. она ET_DYN) — обе системы координат "свои" для
// переданного elf_data/ph.
static const void *vaddr_to_file_ptr(const void *elf_data, const elf64_program_header_t *ph,
                                     int phnum, uint64_t vaddr) {
    for (int i = 0; i < phnum; i++) {
        if (ph[i].type != PT_LOAD) continue;
        if (vaddr >= ph[i].vaddr && vaddr < ph[i].vaddr + ph[i].filesz) {
            return (const uint8_t*)elf_data + ph[i].offset + (vaddr - ph[i].vaddr);
        }
    }
    return NULL;
}

// Грузит /lib/libc.so с диска и кэширует НАВСЕГДА (первый вызов — реальная
// загрузка, остальные — мгновенный no-op). Та же низкоуровневая lufirafs-
// первичка (lufirafs_lookup/read_inode/read), что уже использует
// spawn_shell_process() (kernel.c) для /bin/shell.elf — current_process/
// VFS-таблица fd не обязаны существовать в момент первого вызова (это
// может случиться на самом первом exec() с PT_DYNAMIC, откуда угодно).
static int dynlink_load_libc_cache(void) {
    if (g_libc.ready) return 0;

    uint32_t ino;
    if (lufirafs_lookup(&lufirafs, lufirafs.sb.root_inode, "/lib/libc.so", &ino) != 0) {
        printf("[DYNLINK] /lib/libc.so not found\n");
        return -1;
    }

    lufirafs_inode_t inode;
    if (lufirafs_read_inode(&lufirafs, ino, &inode) != 0 || inode.size == 0) {
        printf("[DYNLINK] /lib/libc.so unreadable\n");
        return -1;
    }

    uint8_t *buf = (uint8_t*)kmalloc(inode.size);
    if (!buf) { printf("[DYNLINK] out of memory loading libc.so\n"); return -1; }
    if (lufirafs_read(&lufirafs, ino, 0, buf, inode.size) != (int)inode.size) {
        printf("[DYNLINK] short read on /lib/libc.so\n");
        kfree(buf); // g_libc.seg_count ещё 0 здесь - rollback() не нужен
        return -1;
    }

    const elf64_header_t *header = (const elf64_header_t*)buf;
    if (elf_validate(header) != 0 || header->type != ET_DYN) {
        printf("[DYNLINK] /lib/libc.so is not a valid ET_DYN shared object\n");
        kfree(buf);
        return -1;
    }

    const elf64_program_header_t *ph =
        (const elf64_program_header_t*)(buf + header->phoff);

    uint64_t dyn_vaddr = 0;
    int have_dynamic = 0;

    for (int i = 0; i < header->phnum && g_libc.seg_count < DYNLINK_MAX_SEGMENTS; i++) {
        if (ph[i].type == PT_DYNAMIC) { dyn_vaddr = ph[i].vaddr; have_dynamic = 1; continue; }
        if (ph[i].type != PT_LOAD) continue;

        dynlink_segment_t *seg = &g_libc.segments[g_libc.seg_count];
        uint64_t seg_start = ph[i].vaddr & ~(PAGE_SIZE - 1);
        uint64_t seg_end = (ph[i].vaddr + ph[i].memsz + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        uint64_t npages = (seg_end - seg_start) / PAGE_SIZE;

        seg->vaddr_offset = seg_start;
        seg->memsz = seg_end - seg_start;
        seg->writable = (ph[i].flags & PF_W) ? 1 : 0;
        seg->executable = (ph[i].flags & PF_X) ? 1 : 0;
        seg->num_pages = npages;
        seg->shm_id = -1;
        seg->phys_pages = (uint64_t*)kmalloc(npages * sizeof(uint64_t));
        if (!seg->phys_pages) {
            printf("[DYNLINK] out of memory (libc.so segment table)\n");
            // Этот сегмент ещё не попал в g_libc.seg_count (инкремент — в
            // конце тела цикла) - откатывать для него нечего, сам
            // kmalloc() для phys_pages не удался, ничего не выделено.
            dynlink_cache_rollback(); // освобождает ВСЕ предыдущие, уже завершённые сегменты
            kfree(buf);
            return -1;
        }

        uint64_t allocated = 0;
        int oom = 0;
        for (uint64_t p = 0; p < npages; p++) {
            uint64_t phys = pmm_alloc_page();
            if (!phys) { oom = 1; break; }
            memset(phys_to_virt(phys), 0, PAGE_SIZE);
            seg->phys_pages[p] = phys;
            allocated++;
        }
        if (oom) {
            printf("[DYNLINK] out of memory (libc.so pages)\n");
            // Этот сегмент ТОЖЕ ещё не в g_libc.seg_count - освобождаем его
            // собственные, уже успевшие выделиться страницы (0..allocated)
            // здесь вручную (rollback() ниже про НЕГО не знает вообще,
            // видит только уже завершённые предыдущие сегменты).
            for (uint64_t p = 0; p < allocated; p++) pmm_free_page(seg->phys_pages[p]);
            kfree(seg->phys_pages);
            seg->phys_pages = NULL;
            dynlink_cache_rollback();
            kfree(buf);
            return -1;
        }

        // ZERO уже сделан выше (общий для всего сегмента) - здесь только
        // COPY, постранично, т.к. начало/конец сегмента не обязаны
        // совпадать с границами страниц (тот же приём, что в
        // elf_load_to_process(), elf.c).
        if (ph[i].filesz > 0) {
            uint64_t dst_vaddr = ph[i].vaddr;
            uint64_t remaining = ph[i].filesz;
            const uint8_t *src = buf + ph[i].offset;
            while (remaining > 0) {
                uint64_t page_va = dst_vaddr & ~(PAGE_SIZE - 1);
                uint64_t page_off = dst_vaddr - page_va;
                uint64_t chunk = PAGE_SIZE - page_off;
                if (chunk > remaining) chunk = remaining;
                uint64_t page_idx = (page_va - seg_start) / PAGE_SIZE;
                memcpy((uint8_t*)phys_to_virt(seg->phys_pages[page_idx]) + page_off, src, chunk);
                dst_vaddr += chunk;
                src += chunk;
                remaining -= chunk;
            }
        }

        g_libc.seg_count++;
    }

    // DT_SYMTAB/DT_STRTAB (экспортные символы - нужны dynlink_resolve()
    // ниже) + DT_RELA (R_X86_64_RELATIVE - собственная самоссылка libc.so
    // на свою же, пока неизвестную заранее базу DYNLINK_LIBC_BASE).
    // Сегодняшний string/malloc/printf/stdlib не порождают ни одной такой
    // релокации (проверено живой сборкой, см. комментарий в dynlink.h) -
    // обрабатываем всё равно, на будущее, это дёшево.
    if (have_dynamic) {
        const elf64_dyn_t *dyn =
            (const elf64_dyn_t*)vaddr_to_file_ptr(buf, ph, header->phnum, dyn_vaddr);
        uint64_t symtab_v = 0, strtab_v = 0, rela_v = 0, relasz = 0, hash_v = 0, strsz = 0;
        if (dyn) {
            for (int i = 0; dyn[i].d_tag != DT_NULL; i++) {
                switch (dyn[i].d_tag) {
                    case DT_SYMTAB: symtab_v = dyn[i].d_val; break;
                    case DT_STRTAB: strtab_v = dyn[i].d_val; break;
                    case DT_RELA:   rela_v   = dyn[i].d_val; break;
                    case DT_RELASZ: relasz   = dyn[i].d_val; break;
                    case DT_HASH:   hash_v   = dyn[i].d_val; break;
                    case DT_STRSZ:  strsz    = dyn[i].d_val; break;
                    default: break;
                }
            }
        }

        if (hash_v && symtab_v && strtab_v) {
            const uint32_t *hash =
                (const uint32_t*)vaddr_to_file_ptr(buf, ph, header->phnum, hash_v);
            // Классический SysV .hash: [0]=nbucket, [1]=nchain, далее
            // bucket[nbucket], chain[nchain] - нам нужен только nchain
            // (== общее число записей .dynsym), саму хэш-таблицу для
            // поиска не используем (dynlink_resolve() - линейный перебор,
            // символов здесь десятки, не тысячи).
            uint32_t nchain = hash ? hash[1] : 0;

            g_libc.dynsym_count = nchain;
            g_libc.dynsym = (const elf64_sym_t*)vaddr_to_file_ptr(buf, ph, header->phnum, symtab_v);
            g_libc.dynstr = (const char*)vaddr_to_file_ptr(buf, ph, header->phnum, strtab_v);
            g_libc.dynstr_size = strsz;
        }

        if (rela_v && relasz) {
            const elf64_rela_t *relas =
                (const elf64_rela_t*)vaddr_to_file_ptr(buf, ph, header->phnum, rela_v);
            uint64_t count = relasz / sizeof(elf64_rela_t);
            for (uint64_t i = 0; relas && i < count; i++) {
                if (ELF64_R_TYPE(relas[i].r_info) != R_X86_64_RELATIVE) continue;

                uint64_t target_vaddr = relas[i].r_offset;
                uint64_t value = DYNLINK_LIBC_BASE + (uint64_t)relas[i].r_addend;

                for (int s = 0; s < g_libc.seg_count; s++) {
                    dynlink_segment_t *seg = &g_libc.segments[s];
                    if (target_vaddr < seg->vaddr_offset ||
                        target_vaddr >= seg->vaddr_offset + seg->memsz)
                        continue;
                    uint64_t rel_off = target_vaddr - seg->vaddr_offset;
                    uint64_t page_idx = rel_off / PAGE_SIZE;
                    uint64_t page_off = rel_off % PAGE_SIZE;
                    *(uint64_t*)((uint8_t*)phys_to_virt(seg->phys_pages[page_idx]) + page_off) = value;
                    break;
                }
            }
        }
    }

    // !writable сегменты (.text и любые чисто читаемые, вроде .rodata) -
    // регистрируем как общую область в shm.c: КАЖДЫЙ будущий процесс
    // будет ссылаться на ЭТИ ЖЕ физические страницы (dynlink_map_libc_
    // into_process() ниже зовёт shm_add_ref(), не копирует) - в этом и
    // есть экономия памяти настоящей динамической линковки. Жёсткий отказ
    // при нехватке слотов (а не молчаливый fallback без PAGE_MMAP_SHARED):
    // немаркированная "общая" страница была бы ошибочно pmm_free_page()'а
    // при выходе ПЕРВОГО же процесса, пока другие ещё её используют.
    for (int s = 0; s < g_libc.seg_count; s++) {
        dynlink_segment_t *seg = &g_libc.segments[s];
        if (seg->writable) continue;
        seg->shm_id = shm_create(seg->phys_pages, seg->num_pages);
        if (seg->shm_id < 0) {
            printf("[DYNLINK] shm registry full - cannot cache libc.so safely\n");
            // Все g_libc.seg_count сегментов (включая этот самый, провально
            // не получивший shm_id) УЖЕ полностью выделены - rollback()
            // корректно освободит и уже зарегистрированные в shm.c (через
            // shm_release() по числу страниц), и ещё не зарегистрированные
            // (этот и любые после него - прямым pmm_free_page()).
            dynlink_cache_rollback();
            kfree(buf);
            return -1;
        }
    }

    g_libc.file_buf = buf;
    g_libc.file_size = inode.size;
    g_libc.ready = 1;

    printf("[DYNLINK] /lib/libc.so cached: %d segment(s), %u exported symbol(s)\n",
           g_libc.seg_count, g_libc.dynsym_count);
    return 0;
}

// Линейный перебор .dynsym (десятки записей - не горячий путь, звучит
// только на exec() динамического бинарника, не на каждый вызов функции
// внутри него). Индекс 0 - всегда зарезервированная пустая запись
// (SHN_UNDEF-заглушка стандарта ELF), пропускаем.
static int dynlink_resolve(const char *name, uint64_t *out_addr, uint64_t *out_size) {
    if (!g_libc.ready || !g_libc.dynsym || !g_libc.dynstr) return -1;

    for (uint32_t i = 1; i < g_libc.dynsym_count; i++) {
        const elf64_sym_t *sym = &g_libc.dynsym[i];
        if (sym->st_shndx == 0) continue; // сам не определён здесь
        if (sym->st_name >= g_libc.dynstr_size) continue;
        if (strcmp(g_libc.dynstr + sym->st_name, name) == 0) {
            *out_addr = DYNLINK_LIBC_BASE + sym->st_value;
            if (out_size) *out_size = sym->st_size;
            return 0;
        }
    }
    return -1;
}

// Маппит кэшированную libc.so в адресное пространство proc: !writable
// сегменты - общие физические страницы (shm_add_ref() вместо копии),
// writable (данные/bss) - СВЕЖАЯ приватная копия (malloc() внутри libc.so
// должен иметь своё собственное состояние кучи на процесс, а не общее с
// другими процессами).
static int dynlink_map_libc_into_process(process_t *proc) {
    for (int s = 0; s < g_libc.seg_count; s++) {
        dynlink_segment_t *seg = &g_libc.segments[s];
        uint64_t base_flags = PAGE_USER;
        if (seg->writable) base_flags |= PAGE_WRITE;
        if (!seg->executable) base_flags |= PAGE_NX;

        for (uint64_t p = 0; p < seg->num_pages; p++) {
            uint64_t virt = DYNLINK_LIBC_BASE + seg->vaddr_offset + p * PAGE_SIZE;

            if (seg->writable) {
                uint64_t phys = pmm_alloc_page();
                if (!phys) {
                    printf("[DYNLINK] out of memory mapping libc.so data\n");
                    return -1;
                }
                memcpy(phys_to_virt(phys), phys_to_virt(seg->phys_pages[p]), PAGE_SIZE);
                if (map_page_in_pml4(proc->page_table, virt, phys, base_flags) != 0) {
                    pmm_free_page(phys);
                    printf("[DYNLINK] map failed (libc.so data)\n");
                    return -1;
                }
            } else {
                if (map_page_in_pml4(proc->page_table, virt, seg->phys_pages[p],
                                      base_flags | PAGE_MMAP_SHARED) != 0) {
                    printf("[DYNLINK] map failed (libc.so text)\n");
                    return -1;
                }
                shm_add_ref(seg->shm_id);
            }
        }
    }
    return 0;
}

// Применяет один .rela.dyn ИЛИ .rela.plt массив (оба имеют одинаковый
// формат Elf64_Rela - разделение на "данные"/"функции" условно и не
// влияет на обработку) к уже замапленной памяти proc. r_offset для
// non-PIE исполняемого файла - уже финальный VA (не нужно перебазировать,
// в отличие от R_X86_64_RELATIVE внутри самой libc.so выше).
static int dynlink_apply_relocations(process_t *proc,
                                     const elf64_sym_t *exe_dynsym, const char *exe_dynstr,
                                     const elf64_rela_t *relas, uint64_t count) {
    for (uint64_t i = 0; i < count; i++) {
        uint32_t type = ELF64_R_TYPE(relas[i].r_info);
        uint32_t symidx = ELF64_R_SYM(relas[i].r_info);
        const char *name = exe_dynstr + exe_dynsym[symidx].st_name;

        if (type == R_X86_64_RELATIVE) {
            // Не должно появляться в НЕ-PIE исполняемом файле (нет
            // собственной базы для перебазирования) - пропускаем с
            // предупреждением, не падаем всей линковкой из-за одной
            // странной записи.
            printf("[DYNLINK] WARNING: unexpected R_X86_64_RELATIVE in a non-PIE executable, skipped\n");
            continue;
        }

        uint64_t sym_addr = 0, sym_size = 0;
        if (dynlink_resolve(name, &sym_addr, &sym_size) != 0) {
            printf("[DYNLINK] undefined symbol: %s\n", name);
            return -1;
        }

        switch (type) {
            case R_X86_64_JUMP_SLOT:
            case R_X86_64_GLOB_DAT: {
                uint64_t phys = get_physical_address_in_pml4(proc->page_table, relas[i].r_offset);
                if (!phys) {
                    printf("[DYNLINK] relocation target 0x%lx not mapped\n", relas[i].r_offset);
                    return -1;
                }
                *(uint64_t*)phys_to_virt(phys) = sym_addr;
                break;
            }
            case R_X86_64_COPY: {
                // Прямая (не через GOT) ссылка исполняемого файла на
                // ДАННЫЕ из библиотеки - размер может быть больше 8 байт
                // и пересечь границу страницы, копируем постранично с
                // обеих сторон (физические страницы процесса не обязаны
                // быть смежными между собой).
                uint64_t remaining = sym_size;
                uint64_t dst_vaddr = relas[i].r_offset;
                uint64_t src_vaddr = sym_addr;
                while (remaining > 0) {
                    uint64_t page_off = dst_vaddr & (PAGE_SIZE - 1);
                    uint64_t chunk = PAGE_SIZE - page_off;
                    if (chunk > remaining) chunk = remaining;

                    uint64_t dst_phys = get_physical_address_in_pml4(proc->page_table, dst_vaddr);
                    uint64_t src_phys = get_physical_address_in_pml4(proc->page_table, src_vaddr);
                    if (!dst_phys || !src_phys) {
                        printf("[DYNLINK] COPY relocation target/source not mapped\n");
                        return -1;
                    }
                    memcpy(phys_to_virt(dst_phys), phys_to_virt(src_phys), chunk);
                    dst_vaddr += chunk;
                    src_vaddr += chunk;
                    remaining -= chunk;
                }
                break;
            }
            default:
                printf("[DYNLINK] WARNING: unsupported relocation type %u for '%s', skipped\n", type, name);
                break;
        }
    }
    return 0;
}

int dynlink_process(process_t *proc, const void *elf_data,
                     const elf64_program_header_t *ph, int phnum) {
    uint64_t dyn_vaddr = 0;
    int have_dynamic = 0;
    for (int i = 0; i < phnum; i++) {
        if (ph[i].type == PT_DYNAMIC) { dyn_vaddr = ph[i].vaddr; have_dynamic = 1; break; }
    }
    if (!have_dynamic) return 0; // обычный статический бинарник - полная обратная совместимость

    const elf64_dyn_t *dyn = (const elf64_dyn_t*)vaddr_to_file_ptr(elf_data, ph, phnum, dyn_vaddr);
    if (!dyn) { printf("[DYNLINK] PT_DYNAMIC not readable\n"); return -1; }

    // Этот toolchain (lufira-packages/build.py) никогда не порождает
    // больше ОДНОЙ зависимости - всегда libc.so (единственная .so во всей
    // системе, см. dynlink.h) - поэтому здесь не проверяется ИМЯ
    // DT_NEEDED по строке, только сам факт хотя бы одной записи. Это
    // осознанное сужение, не упущение: добавление второй настоящей
    // библиотеки потребовало бы реального разбора списка имён.
    int needs_libc = 0;
    uint64_t symtab_v = 0, strtab_v = 0, rela_v = 0, relasz = 0, jmprel_v = 0, pltrelsz = 0;
    for (int i = 0; dyn[i].d_tag != DT_NULL; i++) {
        switch (dyn[i].d_tag) {
            case DT_NEEDED:   needs_libc = 1; break;
            case DT_SYMTAB:   symtab_v = dyn[i].d_val; break;
            case DT_STRTAB:   strtab_v = dyn[i].d_val; break;
            case DT_RELA:     rela_v = dyn[i].d_val; break;
            case DT_RELASZ:   relasz = dyn[i].d_val; break;
            case DT_JMPREL:   jmprel_v = dyn[i].d_val; break;
            case DT_PLTRELSZ: pltrelsz = dyn[i].d_val; break;
            default: break;
        }
    }

    if (!needs_libc) return 0; // PT_DYNAMIC без DT_NEEDED - нечего резолвить

    if (dynlink_load_libc_cache() != 0) return -1;
    if (dynlink_map_libc_into_process(proc) != 0) return -1;

    const elf64_sym_t *exe_dynsym =
        symtab_v ? (const elf64_sym_t*)vaddr_to_file_ptr(elf_data, ph, phnum, symtab_v) : NULL;
    const char *exe_dynstr =
        strtab_v ? (const char*)vaddr_to_file_ptr(elf_data, ph, phnum, strtab_v) : NULL;
    if (!exe_dynsym || !exe_dynstr) {
        printf("[DYNLINK] executable has no .dynsym/.dynstr\n");
        return -1;
    }

    if (rela_v && relasz) {
        const elf64_rela_t *relas = (const elf64_rela_t*)vaddr_to_file_ptr(elf_data, ph, phnum, rela_v);
        if (!relas) { printf("[DYNLINK] .rela.dyn not readable\n"); return -1; }
        if (dynlink_apply_relocations(proc, exe_dynsym, exe_dynstr, relas,
                                      relasz / sizeof(elf64_rela_t)) != 0)
            return -1;
    }

    if (jmprel_v && pltrelsz) {
        const elf64_rela_t *relas = (const elf64_rela_t*)vaddr_to_file_ptr(elf_data, ph, phnum, jmprel_v);
        if (!relas) { printf("[DYNLINK] .rela.plt not readable\n"); return -1; }
        if (dynlink_apply_relocations(proc, exe_dynsym, exe_dynstr, relas,
                                      pltrelsz / sizeof(elf64_rela_t)) != 0)
            return -1;
    }

    return 0;
}
