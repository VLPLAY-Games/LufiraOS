#pragma once

// shm.c — реестр разделяемых (MAP_SHARED) mmap-областей: область переживает
// fork() (физические страницы алиасятся, а не копируются, см.
// PAGE_MMAP_SHARED в paging.h) — минимальное, POSIX-корректное отличие
// MAP_SHARED от MAP_PRIVATE (copy-on-fork). Именованный shm для НЕродственных
// процессов сюда не входит — нужен отдельный слой "имя -> id" поверх реестра.
#include "lib/types.h"

#define MAX_SHARED_REGIONS      16
// v0.9.5: SYS_NET_FETCH (syscall.c) гонит тело HTTP-ответа через shm (netd
// пишет, ядро вычитывает через phys_to_virt()) — пакеты уже перевалили за
// 1MB (doom.lpg ~4.8MB), отсюда 4096 страниц = 16MB, а не прежний 1MB
// "с запасом для бридж-целей".
#define MAX_SHARED_REGION_PAGES 4096

void shm_init(void);

// Регистрирует новую область из УЖЕ выделенных и обнулённых физических
// страниц (sys_mmap() сам их выделяет). refcount стартует с num_pages, а не
// с 1: shm_add_ref()/shm_release() считают ПОСТРАНИЧНО, так что создающий
// процесс обязан сразу дать области ссылку на каждую свою страницу — иначе
// его собственный выход без единого fork() схлопнется в двойное
// освобождение. Возвращает id (>=0) или -1 (нет слотов/слишком много страниц).
int shm_create(const uint64_t *pages, uint64_t num_pages);

// +1 к refcount — зовётся clone_address_space_deep() (process.c) на
// каждую страницу, которую она алиасит (а не копирует) при fork().
void shm_add_ref(int id);

// -1 к refcount; на 0 — освобождает все физические страницы области через
// pmm_free_page() и очищает слот. Зовётся unmap_page()/
// free_user_address_space() (paging.c) вместо прямого pmm_free_page() для
// страниц с PAGE_MMAP_SHARED.
void shm_release(int id);

// Линейный поиск: какой области принадлежит физическая страница phys (-1,
// если ни одной) — PTE несёт только булев флаг PAGE_MMAP_SHARED, не сам id
// (не хватило бы OS-битов), поэтому id ищется по месту. До ~4K сравнений
// (MAX_SHARED_REGIONS*MAX_SHARED_REGION_PAGES), не горячий путь.
int shm_find_region_by_page(uint64_t phys);

// Именованный shm (SYS_SHM_CREATE/SYS_SHM_ATTACH, syscall.c) — неродственный
// процесс знает только id (передан ему заранее через control-plane IPC,
// mailbox.h/driver.h), не сам список страниц; нужен способ прочитать его
// обратно у реестра, чтобы замаппить те же физические страницы в СВОЁ
// адресное пространство. Копирует до max_pages страниц в pages_out,
// *num_pages_out — реальное число страниц у области. 0 успех, -1 неверный id.
int shm_get_pages(int id, uint64_t *pages_out, uint64_t max_pages, uint64_t *num_pages_out);

// Число страниц в области (0, если id неверен) — нужен вызывающему, чтобы
// kmalloc() буфер под shm_get_pages() РОВНО нужного размера, не на все
// MAX_SHARED_REGION_PAGES разом (см. sys_shm_attach(), syscall.c).
uint64_t shm_get_num_pages(int id);
