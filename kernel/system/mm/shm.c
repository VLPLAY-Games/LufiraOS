#include "shm.h"
#include "pmm.h"
#include "lib/stddef.h"

typedef struct {
    int in_use;
    int refcount;
    uint64_t num_pages;
    uint64_t pages[MAX_SHARED_REGION_PAGES];
} shared_region_t;

static shared_region_t g_regions[MAX_SHARED_REGIONS];

void shm_init(void) {
    for (int i = 0; i < MAX_SHARED_REGIONS; i++) {
        g_regions[i].in_use = 0;
        g_regions[i].refcount = 0;
        g_regions[i].num_pages = 0;
    }
}

int shm_create(const uint64_t *pages, uint64_t num_pages) {
    if (num_pages == 0 || num_pages > MAX_SHARED_REGION_PAGES) return -1;

    int slot = -1;
    for (int i = 0; i < MAX_SHARED_REGIONS; i++) {
        if (!g_regions[i].in_use) { slot = i; break; }
    }
    if (slot < 0) return -1;

    // НАЙДЕННЫЙ БАГ (во время работы над динамической линковкой, которой
    // нужны честные МНОГОстраничные общие области — libc.so размером в
    // несколько страниц): refcount здесь раньше всегда стартовал с 1,
    // независимо от num_pages — но shm_add_ref()/shm_release() зовутся
    // ПОСТРАНИЧНО (см. их комментарии и оба места вызова:
    // clone_address_space_deep() в process.c, unmap_page()/
    // free_user_address_space() в paging.c — каждая СТРАНИЦА области даёт
    // отдельный +1/-1). Для num_pages==1 (все области до динамической
    // линковки) разницы не было — 1 страница, 1 ref. Но для области из
    // N>1 страниц: создающий процесс кладёт себе N страниц, однако
    // refcount=1 — при его выходе ПЕРВЫЙ же из N вызовов shm_release()
    // (по одному на страницу) обнулял refcount и освобождал ВСЕ N
    // физических страниц разом, а остальные N-1 вызовов в том же цикле
    // находили область уже очищенной (shm_find_region_by_page() -> -1) и
    // били pmm_free_page() по уже освобождённым страницам — двойное
    // освобождение, порча битовой карты pmm. Правильный инвариант: один
    // ref на каждую страницу, которой сейчас кто-то реально владеет —
    // ровно это и считают shm_add_ref()/shm_release(), так что начальное
    // значение должно быть num_pages (сколько ссылок кладёт сам
    // создающий процесс), а не 1.
    shared_region_t *r = &g_regions[slot];
    r->in_use = 1;
    r->refcount = (int)num_pages;
    r->num_pages = num_pages;
    for (uint64_t i = 0; i < num_pages; i++) r->pages[i] = pages[i];

    return slot;
}

void shm_add_ref(int id) {
    if (id < 0 || id >= MAX_SHARED_REGIONS || !g_regions[id].in_use) return;
    g_regions[id].refcount++;
}

void shm_release(int id) {
    if (id < 0 || id >= MAX_SHARED_REGIONS || !g_regions[id].in_use) return;

    shared_region_t *r = &g_regions[id];
    r->refcount--;
    if (r->refcount > 0) return;

    // Последний процесс отпустил область — теперь действительно освобождаем
    // физическую память, а не просто снимаем маппинг из одного адресного
    // пространства.
    for (uint64_t i = 0; i < r->num_pages; i++) {
        pmm_free_page(r->pages[i]);
    }
    r->in_use = 0;
    r->num_pages = 0;
}

int shm_find_region_by_page(uint64_t phys) {
    for (int i = 0; i < MAX_SHARED_REGIONS; i++) {
        if (!g_regions[i].in_use) continue;
        shared_region_t *r = &g_regions[i];
        for (uint64_t p = 0; p < r->num_pages; p++) {
            if (r->pages[p] == phys) return i;
        }
    }
    return -1;
}
