#pragma once

// shm.c — реестр разделяемых (MAP_SHARED) mmap-областей: область переживает
// fork() (физические страницы алиасятся, а не копируются, см.
// PAGE_MMAP_SHARED в paging.h) — минимальное, POSIX-корректное отличие
// MAP_SHARED от MAP_PRIVATE (copy-on-fork). Именованный shm для НЕродственных
// процессов сюда не входит — нужен отдельный слой "имя -> id" поверх реестра.
#include "lib/types.h"

#define MAX_SHARED_REGIONS      16
#define MAX_SHARED_REGION_PAGES 256 // 1MB на область - с запасом для бридж-целей

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
