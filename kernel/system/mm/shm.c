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

    // refcount стартует с num_pages, а не с 1: shm_add_ref()/shm_release()
    // считают ПОСТРАНИЧНО (см. shm.h), так что создающий процесс обязан сразу
    // дать области по ссылке на каждую свою страницу.
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

int shm_get_pages(int id, uint64_t *pages_out, uint64_t max_pages, uint64_t *num_pages_out) {
    if (id < 0 || id >= MAX_SHARED_REGIONS || !g_regions[id].in_use) return -1;

    shared_region_t *r = &g_regions[id];
    uint64_t n = r->num_pages;
    if (n > max_pages) n = max_pages;
    for (uint64_t i = 0; i < n; i++) pages_out[i] = r->pages[i];
    if (num_pages_out) *num_pages_out = r->num_pages;
    return 0;
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
