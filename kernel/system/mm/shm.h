#pragma once

// shm.c — реестр разделяемых (MAP_SHARED) mmap-областей. v0.8-мост,
// пункт 4: SYS_MMAP(MAP_SHARED) был объявлен в ABI (syscall.h), но флаг
// нигде не проверялся — любой mmap() вёл себя как MAP_PRIVATE.
//
// Сегодня область переживает fork() (физические страницы алиасятся, а не
// копируются, см. PAGE_MMAP_SHARED в paging.h) — это и есть минимальное,
// POSIX-корректное определение MAP_SHARED в отличие от MAP_PRIVATE
// (copy-on-fork). Полноценный именованный shm (для НЕродственных
// процессов, как потребовалось бы будущему компоновщику окон делиться
// буфером с произвольным клиентом) сюда НЕ входит — для этого нужен ещё
// слой "имя -> id" поверх этого реестра, которого пока явно не просили.
#include "lib/types.h"

#define MAX_SHARED_REGIONS      16
#define MAX_SHARED_REGION_PAGES 256 // 1MB на область - с запасом для бридж-целей

void shm_init(void);

// Регистрирует новую область из УЖЕ выделенных и обнулённых физических
// страниц (sys_mmap() сам их выделяет, как и для обычного MAP_PRIVATE).
// refcount стартует с 1 (сам вызывающий процесс). Возвращает id (>=0) или
// -1 (нет свободных слотов/слишком много страниц для одной области).
int shm_create(const uint64_t *pages, uint64_t num_pages);

// +1 к refcount — зовётся clone_address_space_deep() (process.c) на
// каждую страницу, которую она алиасит (а не копирует) при fork().
void shm_add_ref(int id);

// -1 к refcount; на 0 — освобождает все физические страницы области через
// pmm_free_page() и очищает слот. Зовётся unmap_page()/
// free_user_address_space() (paging.c) вместо прямого pmm_free_page() для
// страниц с PAGE_MMAP_SHARED.
void shm_release(int id);

// Линейный поиск: какой области принадлежит физическая страница phys (или
// -1, если ни одной) — PTE несёт только булев флаг PAGE_MMAP_SHARED, не
// сам id (в доступных OS-битах PTE банально не хватило бы места), так что
// id приходится искать по месту. MAX_SHARED_REGIONS*MAX_SHARED_REGION_PAGES
// — не больше ~4K сравнений, не горячий путь (только fork()/munmap() на
// shared-регион).
int shm_find_region_by_page(uint64_t phys);
