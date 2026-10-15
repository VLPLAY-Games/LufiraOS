#include "exfat.h"
#include "system/mm/heap.h"
#include "lib/stddef.h"
#include "lib/string.h"

#define EXFAT_FAT_EOF 0xFFFFFFFFu
#define EXFAT_FAT_BAD 0xFFFFFFF7u
#define EXFAT_ENTRY_INUSE 0x80

static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static inline uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32); }
static inline void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static inline void wr64(uint8_t *p, uint64_t v) { wr32(p, (uint32_t)v); wr32(p + 4, (uint32_t)(v >> 32)); }

/* ======== Метаданные (reserved-регион + таблица FAT) — резидентны в
 * fs->image целиком. dirty_map покрывает ТОЛЬКО эту область (image_sectors),
 * не весь диск — см. разбор в exfat.h. ======== */
static void mark_dirty(exfat_fs_t *fs, uint32_t byte_off, uint32_t len) {
    if (!fs->dirty_map || len == 0) return;
    uint64_t meta_bytes = (uint64_t)fs->image_sectors * 512;
    if (byte_off >= meta_bytes) return;
    uint32_t first = byte_off / 512;
    uint32_t last = (uint32_t)((byte_off + len - 1) / 512);
    if (last >= fs->image_sectors) last = fs->image_sectors - 1;
    for (uint32_t lba = first; lba <= last; lba++)
        fs->dirty_map[lba >> 3] |= (1 << (lba & 7));
}

static inline uint8_t *fat_entry_ptr(exfat_fs_t *fs, uint32_t cluster) {
    return fs->image + fs->fat_offset_bytes + (uint64_t)cluster * 4;
}
static uint32_t get_fat(exfat_fs_t *fs, uint32_t cluster) { return rd32(fat_entry_ptr(fs, cluster)); }
static void set_fat(exfat_fs_t *fs, uint32_t cluster, uint32_t val) {
    wr32(fat_entry_ptr(fs, cluster), val);
    mark_dirty(fs, (uint32_t)(fat_entry_ptr(fs, cluster) - fs->image), 4);
}

/* ======== Доступ к кластерам ДАННЫХ (содержимое файлов) — НАПРЯМУЮ на
 * устройство, без постоянного кеширования. Это и есть "не грузить флешку
 * целиком в ОЗУ", см. разбор в exfat.h. ======== */
static inline uint64_t cluster_byte_offset(exfat_fs_t *fs, uint32_t cluster) {
    return (uint64_t)fs->cluster_heap_offset_bytes + (uint64_t)(cluster - 2) * fs->cluster_size;
}
static int read_cluster_raw(exfat_fs_t *fs, uint32_t cluster, void *buf) {
    if (cluster < 2) return -1;
    uint64_t off = cluster_byte_offset(fs, cluster);
    return fs->dev.read_blocks(fs->dev.ctx, (uint32_t)(off / 512), fs->cluster_size / 512, buf);
}
static int write_cluster_raw(exfat_fs_t *fs, uint32_t cluster, const void *buf) {
    if (cluster < 2) return -1;
    uint64_t off = cluster_byte_offset(fs, cluster);
    return fs->dev.write_blocks(fs->dev.ctx, (uint32_t)(off / 512), fs->cluster_size / 512, buf);
}

static uint32_t alloc_cluster(exfat_fs_t *fs) {
    if (!fs->bitmap) return 0;
    for (uint32_t i = 0; i < fs->bitmap_bits; i++) {
        if (!(fs->bitmap[i >> 3] & (1 << (i & 7)))) {
            fs->bitmap[i >> 3] |= (1 << (i & 7));
            uint32_t cluster = i + 2;
            memset(fs->scratch_cluster, 0, fs->cluster_size);
            if (write_cluster_raw(fs, cluster, fs->scratch_cluster) != 0) {
                fs->bitmap[i >> 3] &= ~(1 << (i & 7)); // откат — реально не выделили
                return 0;
            }
            return cluster;
        }
    }
    return 0;
}

static void free_cluster(exfat_fs_t *fs, uint32_t cluster) {
    uint32_t i = cluster - 2;
    fs->bitmap[i >> 3] &= ~(1 << (i & 7));
}

// Номер кластера по индексу внутри цепочки (0-based) — для NoFatChain это
// просто арифметика (кластеры подряд), иначе обычный проход по FAT. 0, если
// цепочка короче (конец достигнут раньше index).
static uint32_t cluster_for_index(exfat_fs_t *fs, uint32_t first_cluster, int no_fat_chain, uint32_t index) {
    if (first_cluster < 2) return 0;
    if (no_fat_chain) return first_cluster + index;
    uint32_t c = first_cluster;
    for (uint32_t i = 0; i < index; i++) {
        c = get_fat(fs, c);
        if (c < 2 || c >= EXFAT_FAT_BAD) return 0;
    }
    return c;
}

// То же самое, но выделяет/подключает недостающие кластеры по требованию —
// ТОЛЬКО для цепочек в режиме FAT-chain (вызывающий обязан был убедиться,
// что NoFatChain не стоит — см. exfat_write_file()). *first_cluster_io
// обновляется, если цепочка была пуста (первое выделение).
static uint32_t cluster_for_index_alloc(exfat_fs_t *fs, uint32_t *first_cluster_io, uint32_t index) {
    if (*first_cluster_io < 2) {
        uint32_t c = alloc_cluster(fs);
        if (!c) return 0;
        set_fat(fs, c, EXFAT_FAT_EOF);
        *first_cluster_io = c;
    }
    uint32_t c = *first_cluster_io;
    for (uint32_t i = 0; i < index; i++) {
        uint32_t next = get_fat(fs, c);
        if (next >= 2 && next < EXFAT_FAT_BAD) { c = next; continue; }
        uint32_t nc = alloc_cluster(fs);
        if (!nc) return 0;
        set_fat(fs, nc, EXFAT_FAT_EOF);
        set_fat(fs, c, nc);
        c = nc;
    }
    return c;
}

// Следует по (обязательно FAT-chain) цепочке до конца и подключает ОДИН
// новый кластер — используется только для роста КАТАЛОГОВ.
static uint32_t extend_chain(exfat_fs_t *fs, uint32_t first_cluster) {
    uint32_t c = first_cluster;
    for (;;) {
        uint32_t next = get_fat(fs, c);
        if (next < 2 || next >= EXFAT_FAT_BAD) break;
        c = next;
    }
    uint32_t nc = alloc_cluster(fs);
    if (!nc) return 0;
    set_fat(fs, nc, EXFAT_FAT_EOF);
    set_fat(fs, c, nc);
    return nc;
}

/* ======== Реестр кэшей каталогов — см. подробный разбор архитектуры в
 * exfat.h. Ключ — first_cluster; "открытый" handle никогда не хранит сырой
 * указатель, только (ключ, смещение), так что вытеснение слота безопасно —
 * следующий доступ просто перезагрузит его заново. ======== */

static void dir_cache_flush_slot(exfat_fs_t *fs, exfat_dir_cache_t *c) {
    if (!c->in_use || !c->dirty) return;
    for (uint32_t i = 0; i < c->chain_len; i++)
        write_cluster_raw(fs, c->chain[i], c->image + (uint64_t)i * fs->cluster_size);
    c->dirty = 0;
}

static void dir_cache_evict_slot(exfat_fs_t *fs, exfat_dir_cache_t *c) {
    dir_cache_flush_slot(fs, c);
    if (c->image) kfree(c->image);
    c->image = NULL;
    c->in_use = 0;
    c->chain_len = 0;
}

// Освобождает слот БЕЗ сброса на диск — для случая, когда цепочка каталога
// только что выросла (extend_chain()) и кэш просто устарел, а не "грязный"
// (мы его не мутировали, значит на диске он уже совпадает с тем, что было).
static void dir_cache_invalidate(exfat_fs_t *fs, uint32_t first_cluster) {
    (void)fs;
    for (int i = 0; i < EXFAT_DIR_CACHE_SLOTS; i++) {
        exfat_dir_cache_t *c = &fs->dir_cache[i];
        if (c->in_use && c->first_cluster == first_cluster) {
            if (c->image) kfree(c->image);
            c->image = NULL;
            c->in_use = 0;
            c->chain_len = 0;
            return;
        }
    }
}

// Находит (или загружает заново) кэш каталога с заданным first_cluster.
// dir_size нужен ТОЛЬКО для NoFatChain-случая (там нет явного конца цепочки
// в FAT — границу задаёт размер каталога, известный из его Stream Extension
// в РОДИТЕЛЬСКОМ каталоге, см. вызывающих). NULL при нехватке памяти/ошибке
// чтения, либо если цепочка длиннее EXFAT_DIR_CACHE_MAX_CLUSTERS.
static exfat_dir_cache_t *dir_cache_get(exfat_fs_t *fs, uint32_t first_cluster, int no_fat_chain, uint64_t dir_size) {
    if (first_cluster < 2) return NULL;

    for (int i = 0; i < EXFAT_DIR_CACHE_SLOTS; i++) {
        if (fs->dir_cache[i].in_use && fs->dir_cache[i].first_cluster == first_cluster) {
            fs->dir_cache[i].lru_tick = ++fs->dir_cache_clock;
            return &fs->dir_cache[i];
        }
    }

    int slot = -1;
    for (int i = 0; i < EXFAT_DIR_CACHE_SLOTS; i++) {
        if (!fs->dir_cache[i].in_use) { slot = i; break; }
    }
    if (slot < 0) {
        uint32_t oldest = 0xFFFFFFFFu;
        for (int i = 0; i < EXFAT_DIR_CACHE_SLOTS; i++) {
            if (fs->dir_cache[i].lru_tick < oldest) { oldest = fs->dir_cache[i].lru_tick; slot = i; }
        }
        dir_cache_evict_slot(fs, &fs->dir_cache[slot]);
    }

    exfat_dir_cache_t *c = &fs->dir_cache[slot];
    memset(c, 0, sizeof(*c));
    c->first_cluster = first_cluster;
    c->no_fat_chain = no_fat_chain;

    uint32_t count = 0;
    if (no_fat_chain) {
        uint32_t need = (uint32_t)((dir_size + fs->cluster_size - 1) / fs->cluster_size);
        if (need == 0) need = 1;
        if (need > EXFAT_DIR_CACHE_MAX_CLUSTERS) return NULL;
        for (uint32_t i = 0; i < need; i++) c->chain[count++] = first_cluster + i;
    } else {
        uint32_t cluster = first_cluster;
        while (cluster >= 2 && count < EXFAT_DIR_CACHE_MAX_CLUSTERS) {
            c->chain[count++] = cluster;
            uint32_t next = get_fat(fs, cluster);
            if (next < 2 || next >= EXFAT_FAT_BAD) break;
            cluster = next;
        }
    }
    if (count == 0) return NULL;

    c->image = (uint8_t *)kmalloc((size_t)count * fs->cluster_size);
    if (!c->image) return NULL;

    for (uint32_t i = 0; i < count; i++) {
        if (read_cluster_raw(fs, c->chain[i], c->image + (uint64_t)i * fs->cluster_size) != 0) {
            kfree(c->image);
            c->image = NULL;
            return NULL;
        }
    }
    c->chain_len = count;
    c->in_use = 1;
    c->dirty = 0;
    c->lru_tick = ++fs->dir_cache_clock;
    return c;
}

// Указатель на данные конкретного кластера ВНУТРИ уже загруженного кэша —
// линейный поиск по chain[] (короткий, EXFAT_DIR_CACHE_MAX_CLUSTERS с
// запасом мал).
static uint8_t *dir_cache_cluster_ptr(exfat_dir_cache_t *c, uint32_t cluster_size, uint32_t cluster) {
    for (uint32_t i = 0; i < c->chain_len; i++)
        if (c->chain[i] == cluster) return c->image + (uint64_t)i * cluster_size;
    return NULL;
}

static void get_meta(exfat_fs_t *fs, exfat_handle_t h, uint32_t *first_cluster, uint64_t *size, int *no_fat_chain, int *is_dir) {
    if (h == EXFAT_ROOT_HANDLE) {
        *first_cluster = fs->root_cluster;
        *size = 0;
        *no_fat_chain = 0;
        *is_dir = 1;
        return;
    }
    uint32_t dir_cluster = (uint32_t)(h >> 32);
    uint32_t se_off = (uint32_t)h;

    // См. разбор границы в exfat.h: каталог-держатель этой записи почти
    // всегда ещё/уже в кэше (только что был тронут walk_dir()/find_cb()) —
    // dir_size=0 здесь корректен для обычного FAT-chain случая (подавляющее
    // большинство) и неточен только для НЕ-созданных-этим-драйвером
    // NoFatChain-каталогов, случайно вытесненных из кэша между вызовами.
    exfat_dir_cache_t *c = dir_cache_get(fs, dir_cluster, 0, 0);
    if (!c) { *first_cluster = 0; *size = 0; *no_fat_chain = 0; *is_dir = 0; return; }

    uint8_t *se = c->image + se_off;
    *first_cluster = rd32(se + 20);
    *size = rd64(se + 24);
    *no_fat_chain = (se[1] & 0x02) != 0;
    uint16_t attrs = rd16(se - 32 + 4); // Primary (0x85) entry — ровно 32 байта перед Stream Extension
    *is_dir = (attrs & 0x10) != 0;
}

static void recompute_checksum(uint8_t *fe) {
    uint8_t sec_count = fe[1];
    int total_bytes = 32 * (1 + sec_count);
    uint16_t sum = 0;
    for (int i = 0; i < total_bytes; i++) {
        if (i == 2 || i == 3) continue;
        sum = (uint16_t)(((sum << 15) | (sum >> 1)) + fe[i]);
    }
    wr16(fe + 2, sum);
}

/* ========================= Directory entries ========================= */

typedef int (*dir_visit_cb_t)(void *ctx, exfat_handle_t h, const char *name, int name_len, int is_dir);

// Обходит набор (File 0x85 + Stream 0xC0 + Name 0xC1 ×N) по уже загруженному
// кэшу каталога (dir_cache_get()); cb вызывается один раз на КАЖДЫЙ набор
// InUse (0x85 с битом 0x80) — EntryType==0x00 означает конец каталога
// (гарантия спеки: неиспользуемый хвост всегда занулён), набор со снятым
// 0x80 (удалённый) — просто пропускается, не вызывая cb.
static int walk_dir(exfat_fs_t *fs, uint32_t first_cluster, int no_fat_chain, uint64_t dir_size, dir_visit_cb_t cb, void *ctx) {
    exfat_dir_cache_t *c = dir_cache_get(fs, first_cluster, no_fat_chain, dir_size);
    if (!c) return 0;

    for (uint32_t cidx = 0; cidx < c->chain_len; cidx++) {
        uint8_t *base = c->image + (uint64_t)cidx * fs->cluster_size;
        uint32_t off = 0;
        while (off < fs->cluster_size) {
            uint8_t et = base[off];
            if (et == 0x00) return 0;
            if (et == (0x05 | EXFAT_ENTRY_INUSE)) { // 0x85
                uint8_t sec_count = base[off + 1];
                if (off + 32u * (1 + sec_count) > fs->cluster_size) { off += 32; continue; }
                uint8_t *stream = base + off + 32;
                if (stream[0] != 0xC0) { off += 32; continue; }

                int name_len = stream[3];
                char name[256];
                int got = 0;
                for (int s = 1; s < sec_count && got < name_len && got < 255; s++) {
                    uint8_t *ne = base + off + 32 * (1 + s);
                    if (ne[0] != 0xC1) break;
                    for (int k = 0; k < 15 && got < name_len && got < 255; k++) {
                        uint16_t ch = rd16(ne + 2 + 2 * k);
                        name[got++] = (ch > 0 && ch < 128) ? (char)ch : '?';
                    }
                }
                name[got] = '\0';

                uint16_t attrs = rd16(base + off + 4);
                int is_dir = (attrs & 0x10) != 0;
                exfat_handle_t h = ((exfat_handle_t)first_cluster << 32) | (uint32_t)(stream - c->image);
                if (cb(ctx, h, name, got, is_dir)) return 1;
                off += 32u * (1 + sec_count);
            } else {
                off += 32;
            }
        }
    }
    return 0;
}

typedef struct { const char *name; int name_len; exfat_handle_t found_handle; int found; } find_ctx_t;

static int find_cb(void *vctx, exfat_handle_t h, const char *name, int name_len, int is_dir) {
    (void)is_dir;
    find_ctx_t *ctx = (find_ctx_t *)vctx;
    if (name_len != ctx->name_len) return 0;
    for (int i = 0; i < name_len; i++) {
        char a = name[i], b = ctx->name[i];
        if (a >= 'a' && a <= 'z') a -= 32;
        if (b >= 'a' && b <= 'z') b -= 32;
        if (a != b) return 0;
    }
    ctx->found_handle = h;
    ctx->found = 1;
    return 1;
}

typedef struct { int found; } any_ctx_t;
static int any_cb(void *vctx, exfat_handle_t h, const char *name, int name_len, int is_dir) {
    (void)h; (void)name; (void)name_len; (void)is_dir;
    ((any_ctx_t *)vctx)->found = 1;
    return 1;
}

static int find_free_run(exfat_fs_t *fs, uint32_t first_cluster, int no_fat_chain, uint64_t dir_size,
                          int need_slots, uint32_t *out_cluster, uint32_t *out_offset) {
    exfat_dir_cache_t *c = dir_cache_get(fs, first_cluster, no_fat_chain, dir_size);
    if (!c) return -1;
    for (uint32_t cidx = 0; cidx < c->chain_len; cidx++) {
        uint8_t *base = c->image + (uint64_t)cidx * fs->cluster_size;
        uint32_t off = 0;
        while (off < fs->cluster_size) {
            if (base[off] == 0x00) {
                if (off + 32u * (uint32_t)need_slots <= fs->cluster_size) {
                    *out_cluster = c->chain[cidx]; *out_offset = off;
                    return 0;
                }
                return -1;
            }
            off += 32;
        }
    }
    return -1;
}

/* ============================ Инициализация ============================ */

void exfat_deinit(exfat_fs_t *fs) {
    if (!fs) return;
    if (fs->image) { kfree(fs->image); fs->image = NULL; }
    if (fs->dirty_map) { kfree(fs->dirty_map); fs->dirty_map = NULL; }
    if (fs->bitmap) { kfree(fs->bitmap); fs->bitmap = NULL; }
    if (fs->scratch_cluster) { kfree(fs->scratch_cluster); fs->scratch_cluster = NULL; }
    for (int i = 0; i < EXFAT_DIR_CACHE_SLOTS; i++) {
        if (fs->dir_cache[i].image) kfree(fs->dir_cache[i].image);
        fs->dir_cache[i].image = NULL;
        fs->dir_cache[i].in_use = 0;
    }
}

// v0.9, фаза 2 — РЕДИЗАЙН: раньше exfat_init() требовал образ ФС целиком в
// ОЗУ одним kmalloc()'ом (image, image_size) — отсюда жёсткий потолок у
// вызывающих ("устройство не больше 8MB", половина кучи ядра). Теперь
// exfat_init() сам читает устройство через fs->dev блоками: в ОЗУ остаются
// только метаданные (boot-регион + таблица FAT, EXFAT_META_MAX_BYTES) и
// резидентный кэш Allocation Bitmap (EXFAT_BITMAP_MAX_BYTES) — см. их
// разбор в exfat.h. Содержимое файлов и обычные каталоги (кроме как через
// ограниченный LRU-реестр dir_cache) никогда не копируются в ОЗУ целиком.
int exfat_init(exfat_fs_t *fs, const exfat_blockdev_t *dev, uint32_t total_sectors) {
    if (!fs || !dev || !dev->read_blocks || !dev->write_blocks) return -1;
    if (total_sectors == 0) return -1;

    memset(fs, 0, sizeof(*fs));
    fs->dev = *dev;

    uint8_t boot[512];
    if (fs->dev.read_blocks(fs->dev.ctx, 0, 1, boot) != 0) return -1;

    for (int i = 0; i < 8; i++) if (boot[3 + i] != "EXFAT   "[i]) return -1;
    if (rd16(boot + 510) != 0xAA55) return -1;

    uint8_t bps_shift = boot[108];
    uint8_t spc_shift = boot[109];
    // bps_shift < 9 означало бы сектор меньше 512 байт — спека exFAT и
    // реальные устройства такого не допускают, а наша блочная абстракция
    // (512-байтные LBA, см. exfat.h) этого не переживёт.
    if (bps_shift < 9 || bps_shift > 16 || spc_shift > 16) return -1;
    fs->bytes_per_sector = 1u << bps_shift;
    fs->sectors_per_cluster = 1u << spc_shift;
    fs->cluster_size = fs->bytes_per_sector * fs->sectors_per_cluster;
    if (fs->cluster_size == 0 || fs->cluster_size > EXFAT_CLUSTER_MAX_BYTES) return -2;

    fs->fat_offset_bytes = rd32(boot + 80) * fs->bytes_per_sector;
    fs->fat_length_bytes = rd32(boot + 84) * fs->bytes_per_sector;
    fs->cluster_heap_offset_bytes = rd32(boot + 88) * fs->bytes_per_sector;
    fs->cluster_count = rd32(boot + 92);
    fs->root_cluster = rd32(boot + 96);
    if (fs->root_cluster < 2 || fs->cluster_count == 0) return -1;

    uint64_t total_bytes = (uint64_t)total_sectors * 512;
    if ((uint64_t)fs->cluster_heap_offset_bytes + (uint64_t)fs->cluster_count * fs->cluster_size > total_bytes) return -2;
    // Таблица FAT обязана целиком лежать ДО кучи кластеров — иначе
    // fat_entry_ptr() читал/писал бы за пределами fs->image (который теперь
    // выделен РОВНО под cluster_heap_offset_bytes, не под весь диск, как
    // раньше) — см. разбор в exfat.h про то, что это и есть смысл редизайна.
    if ((uint64_t)fs->fat_offset_bytes + (uint64_t)fs->fat_length_bytes > fs->cluster_heap_offset_bytes) return -2;
    if (fs->cluster_heap_offset_bytes % 512 != 0) return -2; // не укладывается в 512-байтные LBA устройства

    fs->total_sectors = total_sectors;
    fs->image_sectors = fs->cluster_heap_offset_bytes / 512;
    if ((uint64_t)fs->image_sectors * 512 > EXFAT_META_MAX_BYTES) return -3;

    fs->image = (uint8_t *)kmalloc((size_t)fs->image_sectors * 512);
    if (!fs->image) return -4;
    if (fs->dev.read_blocks(fs->dev.ctx, 0, fs->image_sectors, fs->image) != 0) {
        exfat_deinit(fs);
        return -5;
    }

    uint32_t map_size = (fs->image_sectors + 7) / 8;
    fs->dirty_map = (uint8_t *)kmalloc(map_size);
    if (!fs->dirty_map) { exfat_deinit(fs); return -4; }
    memset(fs->dirty_map, 0, map_size);
    fs->dirty_map_size = map_size;

    fs->scratch_cluster = (uint8_t *)kmalloc(fs->cluster_size);
    if (!fs->scratch_cluster) { exfat_deinit(fs); return -4; }

    // Ищем Allocation Bitmap (0x81) среди записей корня — единственный
    // источник истины о занятости кластеров (см. разбор в exfat.h). Корень
    // у exFAT всегда обычная FAT-цепочка (no_fat_chain=0), размер роли не
    // играет в этой ветке dir_cache_get().
    exfat_dir_cache_t *rc = dir_cache_get(fs, fs->root_cluster, 0, 0);
    if (!rc) { exfat_deinit(fs); return -5; }

    for (uint32_t cidx = 0; cidx < rc->chain_len; cidx++) {
        uint8_t *base = rc->image + (uint64_t)cidx * fs->cluster_size;
        int stop = 0;
        for (uint32_t off = 0; off < fs->cluster_size; off += 32) {
            uint8_t et = base[off];
            if (et == 0x00) { stop = 1; break; }
            if (et == 0x81) {
                uint32_t bc = rd32(base + off + 20);
                if (bc >= 2) fs->bitmap_cluster = bc;
            }
        }
        if (stop) break;
    }

    if (fs->bitmap_cluster >= 2) {
        uint32_t bitmap_bytes_needed = (fs->cluster_count + 7) / 8;
        if (bitmap_bytes_needed > EXFAT_BITMAP_MAX_BYTES) { exfat_deinit(fs); return -3; }

        uint32_t need_clusters = (bitmap_bytes_needed + fs->cluster_size - 1) / fs->cluster_size;
        if (need_clusters == 0) need_clusters = 1;

        uint32_t count = 0;
        uint32_t cluster = fs->bitmap_cluster;
        while (cluster >= 2 && count < need_clusters && count < EXFAT_DIR_CACHE_MAX_CLUSTERS) {
            fs->bitmap_chain[count++] = cluster;
            uint32_t next = get_fat(fs, cluster);
            if (next < 2 || next >= EXFAT_FAT_BAD) break;
            cluster = next;
        }
        if (count == 0) { exfat_deinit(fs); return -5; }

        fs->bitmap = (uint8_t *)kmalloc((size_t)count * fs->cluster_size);
        if (!fs->bitmap) { exfat_deinit(fs); return -4; }

        for (uint32_t i = 0; i < count; i++) {
            if (read_cluster_raw(fs, fs->bitmap_chain[i], fs->bitmap + (uint64_t)i * fs->cluster_size) != 0) {
                exfat_deinit(fs);
                return -5;
            }
        }
        fs->bitmap_chain_len = count;
        fs->bitmap_bits = fs->cluster_count;
    }

    return 0;
}

/* ======== Синхронизация с устройством ========
 * Метаданные (fs->image) — отложенная dirty-bitmap синхронизация, только по
 * image_sectors. Allocation Bitmap — целиком и безусловно (маленький, см.
 * EXFAT_BITMAP_MAX_BYTES). Кэш каталогов — мутаторы (exfat_create()/
 * exfat_unlink()/exfat_truncate()/exfat_write_file()) уже сбрасывают
 * затронутый слот сразу сами (REAL-TIME запись, см. exfat.h) — цикл ниже
 * просто подстраховка на случай, если что-то осталось "грязным". */
void exfat_sync(exfat_fs_t *fs) {
    if (!fs) return;

    if (fs->dirty_map) {
        uint32_t total = fs->image_sectors;
        uint32_t lba = 0;
        while (lba < total) {
            if (!(fs->dirty_map[lba >> 3] & (1 << (lba & 7)))) { lba++; continue; }
            uint32_t run_start = lba, run_len = 0;
            while (lba < total && (fs->dirty_map[lba >> 3] & (1 << (lba & 7)))) { run_len++; lba++; }
            fs->dev.write_blocks(fs->dev.ctx, run_start, run_len, fs->image + (uint64_t)run_start * 512);
        }
        memset(fs->dirty_map, 0, fs->dirty_map_size);
    }

    if (fs->bitmap && fs->bitmap_chain_len > 0) {
        for (uint32_t i = 0; i < fs->bitmap_chain_len; i++)
            write_cluster_raw(fs, fs->bitmap_chain[i], fs->bitmap + (uint64_t)i * fs->cluster_size);
    }

    for (int i = 0; i < EXFAT_DIR_CACHE_SLOTS; i++)
        dir_cache_flush_slot(fs, &fs->dir_cache[i]);
}

/* ============================ Путь / inode-API =========================== */

int exfat_is_dir(exfat_fs_t *fs, exfat_handle_t h) {
    uint32_t fc; uint64_t size; int nofat, isdir;
    get_meta(fs, h, &fc, &size, &nofat, &isdir);
    return isdir;
}

uint64_t exfat_size(exfat_fs_t *fs, exfat_handle_t h) {
    uint32_t fc; uint64_t size; int nofat, isdir;
    get_meta(fs, h, &fc, &size, &nofat, &isdir);
    return size;
}

int exfat_lookup_path(exfat_fs_t *fs, const char *path, exfat_handle_t *out) {
    exfat_handle_t cur = EXFAT_ROOT_HANDLE;
    const char *p = path;
    while (*p == '/') p++;
    while (*p) {
        int len = 0;
        const char *comp = p;
        while (comp[len] && comp[len] != '/') len++;

        uint32_t fc; uint64_t size; int nofat, isdir;
        get_meta(fs, cur, &fc, &size, &nofat, &isdir);
        if (!isdir) return -1;

        find_ctx_t fcx = { comp, len, 0, 0 };
        walk_dir(fs, fc, nofat, size, find_cb, &fcx);
        if (!fcx.found) return -1;
        cur = fcx.found_handle;
        p = comp + len;
        while (*p == '/') p++;
    }
    *out = cur;
    return 0;
}

int exfat_resolve_parent(exfat_fs_t *fs, const char *path, exfat_handle_t *out_parent, char *leaf_out, int leaf_cap) {
    const char *p = path;
    while (*p == '/') p++;
    if (!*p) return -1;

    exfat_handle_t cur = EXFAT_ROOT_HANDLE;
    for (;;) {
        int len = 0;
        const char *comp = p;
        while (comp[len] && comp[len] != '/') len++;
        const char *rest = comp + len;
        while (*rest == '/') rest++;

        if (!*rest) {
            *out_parent = cur;
            int n = (len < leaf_cap - 1) ? len : leaf_cap - 1;
            memcpy(leaf_out, comp, n);
            leaf_out[n] = '\0';
            return 0;
        }

        uint32_t fc; uint64_t size; int nofat, isdir;
        get_meta(fs, cur, &fc, &size, &nofat, &isdir);
        if (!isdir) return -1;

        find_ctx_t fcx = { comp, len, 0, 0 };
        walk_dir(fs, fc, nofat, size, find_cb, &fcx);
        if (!fcx.found) return -1;
        cur = fcx.found_handle;
        p = rest;
    }
}

typedef struct { int index; int cur; char *name_out; exfat_handle_t *h_out; int *is_dir_out; int found; } readdir_ctx_t;

static int readdir_cb(void *vctx, exfat_handle_t h, const char *name, int name_len, int is_dir) {
    readdir_ctx_t *ctx = (readdir_ctx_t *)vctx;
    if (ctx->cur == ctx->index) {
        memcpy(ctx->name_out, name, (size_t)name_len);
        ctx->name_out[name_len] = '\0';
        *ctx->h_out = h;
        *ctx->is_dir_out = is_dir;
        ctx->found = 1;
        return 1;
    }
    ctx->cur++;
    return 0;
}

int exfat_readdir(exfat_fs_t *fs, exfat_handle_t dir, int index, char *name_out, exfat_handle_t *out, int *is_dir_out) {
    uint32_t fc; uint64_t size; int nofat, isdir;
    get_meta(fs, dir, &fc, &size, &nofat, &isdir);
    if (!isdir) return -1;
    readdir_ctx_t ctx = { index, 0, name_out, out, is_dir_out, 0 };
    walk_dir(fs, fc, nofat, size, readdir_cb, &ctx);
    return ctx.found ? 1 : 0;
}

/* ============================ Чтение / запись ============================ */

int exfat_read_file(exfat_fs_t *fs, exfat_handle_t h, uint64_t offset, void *buf, uint32_t count) {
    uint32_t fc; uint64_t size; int nofat, isdir;
    get_meta(fs, h, &fc, &size, &nofat, &isdir);
    if (offset >= size) return 0;
    if (offset + count > size) count = (uint32_t)(size - offset);

    uint8_t *out = (uint8_t *)buf;
    uint32_t done = 0;
    while (done < count) {
        uint64_t file_off = offset + done;
        uint32_t idx = (uint32_t)(file_off / fs->cluster_size);
        uint32_t in_off = (uint32_t)(file_off % fs->cluster_size);
        uint32_t chunk = fs->cluster_size - in_off;
        if (chunk > count - done) chunk = count - done;

        uint32_t cluster = cluster_for_index(fs, fc, nofat, idx);
        if (!cluster) {
            memset(out + done, 0, chunk);
        } else if (chunk == fs->cluster_size) {
            // Полный кластер — читаем прямо в буфер вызывающего, без
            // промежуточной копии.
            if (read_cluster_raw(fs, cluster, out + done) != 0) memset(out + done, 0, chunk);
        } else {
            if (read_cluster_raw(fs, cluster, fs->scratch_cluster) == 0)
                memcpy(out + done, fs->scratch_cluster + in_off, chunk);
            else
                memset(out + done, 0, chunk);
        }
        done += chunk;
    }
    return (int)done;
}

int exfat_write_file(exfat_fs_t *fs, exfat_handle_t h, uint64_t offset, const void *buf, uint32_t count) {
    if (h == EXFAT_ROOT_HANDLE) return -1;
    uint32_t dir_cluster = (uint32_t)(h >> 32);
    uint32_t se_off = (uint32_t)h;

    exfat_dir_cache_t *c = dir_cache_get(fs, dir_cluster, 0, 0);
    if (!c) return -1;
    uint8_t *se = c->image + se_off;
    if (se[1] & 0x02) return -1; // NoFatChain — чужой контейнер, не растим (см. exfat.h)

    uint32_t fc = rd32(se + 20);
    const uint8_t *in = (const uint8_t *)buf;
    uint32_t done = 0;

    // cluster_for_index_alloc()/read_cluster_raw()/write_cluster_raw() ниже
    // трогают только таблицу FAT (резидентная, отдельно от dir_cache) и
    // устройство напрямую — ни разу не вызывают dir_cache_get(), так что
    // c/se гарантированно остаются валидными всю дальнейшую функцию.
    while (done < count) {
        uint64_t file_off = offset + done;
        uint32_t idx = (uint32_t)(file_off / fs->cluster_size);
        uint32_t in_off = (uint32_t)(file_off % fs->cluster_size);
        uint32_t chunk = fs->cluster_size - in_off;
        if (chunk > count - done) chunk = count - done;

        uint32_t cluster = cluster_for_index_alloc(fs, &fc, idx);
        if (!cluster) break;

        if (chunk == fs->cluster_size) {
            if (write_cluster_raw(fs, cluster, in + done) != 0) break;
        } else {
            // Частичная запись — дочитываем остальное содержимое кластера,
            // чтобы не затереть соседние байты (как fat_append_file() у FAT).
            if (read_cluster_raw(fs, cluster, fs->scratch_cluster) != 0) break;
            memcpy(fs->scratch_cluster + in_off, in + done, chunk);
            if (write_cluster_raw(fs, cluster, fs->scratch_cluster) != 0) break;
        }
        done += chunk;
    }

    if (done > 0) {
        wr32(se + 20, fc);
        uint64_t size = rd64(se + 24);
        if (offset + done > size) {
            wr64(se + 24, offset + done);
            wr64(se + 8, offset + done);
        }
        uint8_t *fe = se - 32;
        recompute_checksum(fe);
        c->dirty = 1;
        dir_cache_flush_slot(fs, c);
    }
    return done > 0 ? (int)done : -1;
}

void exfat_truncate(exfat_fs_t *fs, exfat_handle_t h) {
    if (h == EXFAT_ROOT_HANDLE) return;
    uint32_t dir_cluster = (uint32_t)(h >> 32);
    uint32_t se_off = (uint32_t)h;

    exfat_dir_cache_t *c = dir_cache_get(fs, dir_cluster, 0, 0);
    if (!c) return;
    uint8_t *se = c->image + se_off;
    uint32_t fc = rd32(se + 20);
    int nofat = (se[1] & 0x02) != 0;

    if (fc >= 2) {
        if (nofat) {
            uint64_t size = rd64(se + 24);
            uint32_t n = (uint32_t)((size + fs->cluster_size - 1) / fs->cluster_size);
            for (uint32_t i = 0; i < n; i++) free_cluster(fs, fc + i);
        } else {
            uint32_t cc = fc;
            while (cc >= 2 && cc < EXFAT_FAT_BAD) {
                uint32_t next = get_fat(fs, cc);
                free_cluster(fs, cc);
                set_fat(fs, cc, 0);
                cc = next;
            }
        }
    }

    wr32(se + 20, 0);
    wr64(se + 24, 0);
    wr64(se + 8, 0);
    se[1] &= ~0x02;
    recompute_checksum(se - 32);
    c->dirty = 1;
    dir_cache_flush_slot(fs, c);
}

/* ============================ Создание / удаление ========================= */

int exfat_create(exfat_fs_t *fs, exfat_handle_t parent, const char *name, int is_dir, exfat_handle_t *out) {
    int name_len = (int)strlen(name);
    if (name_len == 0 || name_len > 255) return -1;

    uint32_t pfc; uint64_t psize; int p_nofat, p_isdir;
    get_meta(fs, parent, &pfc, &psize, &p_nofat, &p_isdir);
    if (!p_isdir) return -1;

    find_ctx_t fcx = { name, name_len, 0, 0 };
    walk_dir(fs, pfc, p_nofat, psize, find_cb, &fcx);
    if (fcx.found) return -1;

    int name_entries = (name_len + 14) / 15;
    int sec_count = 1 + name_entries;
    int need_slots = 1 + sec_count;

    uint32_t cluster, offset;
    if (find_free_run(fs, pfc, p_nofat, psize, need_slots, &cluster, &offset) != 0) {
        uint32_t new_cluster = extend_chain(fs, pfc);
        if (!new_cluster) return -1;
        // Цепочка родителя выросла — кэш устарел (не "грязный", мы его не
        // мутировали, просто неполный), перезагружаем заново ниже.
        dir_cache_invalidate(fs, pfc);
        cluster = new_cluster;
        offset = 0;
    }

    exfat_dir_cache_t *pc = dir_cache_get(fs, pfc, p_nofat, psize);
    if (!pc) return -1;
    uint8_t *base = dir_cache_cluster_ptr(pc, fs->cluster_size, cluster);
    if (!base) return -1;
    uint8_t *fe = base + offset;
    memset(fe, 0, 32u * (uint32_t)need_slots);
    fe[0] = 0x85;
    fe[1] = (uint8_t)sec_count;
    wr16(fe + 4, is_dir ? 0x0010 : 0x0020);

    uint8_t *se = fe + 32;
    se[0] = 0xC0;
    se[1] = 0x01; // AllocationPossible; NoFatChain не ставим — см. exfat.h
    se[3] = (uint8_t)name_len;

    uint16_t name_u16[255];
    for (int i = 0; i < name_len; i++) name_u16[i] = (uint8_t)name[i];
    for (int s = 0; s < name_entries; s++) {
        uint8_t *ne = se + 32 * (1 + s);
        ne[0] = 0xC1;
        for (int k = 0; k < 15; k++) {
            int idx = s * 15 + k;
            wr16(ne + 2 + 2 * k, idx < name_len ? name_u16[idx] : 0);
        }
    }

    uint16_t hash = 0;
    for (int i = 0; i < name_len; i++) {
        uint16_t ch = name_u16[i];
        if (ch >= 'a' && ch <= 'z') ch = (uint16_t)(ch - 32);
        hash = (uint16_t)(((hash << 15) | (hash >> 1)) + (ch & 0xFF));
        hash = (uint16_t)(((hash << 15) | (hash >> 1)) + (ch >> 8));
    }
    wr16(se + 4, hash);

    if (is_dir) {
        uint32_t c = alloc_cluster(fs);
        if (!c) { fe[0] &= ~EXFAT_ENTRY_INUSE; return -1; }
        set_fat(fs, c, EXFAT_FAT_EOF);
        wr32(se + 20, c);
        wr64(se + 24, fs->cluster_size);
        wr64(se + 8, fs->cluster_size);
    }

    recompute_checksum(fe);
    pc->dirty = 1;
    dir_cache_flush_slot(fs, pc);

    *out = ((exfat_handle_t)pfc << 32) | (uint32_t)(se - pc->image);
    return 0;
}

int exfat_unlink(exfat_fs_t *fs, exfat_handle_t parent, const char *name) {
    int name_len = (int)strlen(name);
    uint32_t pfc; uint64_t psize; int p_nofat, p_isdir;
    get_meta(fs, parent, &pfc, &psize, &p_nofat, &p_isdir);
    if (!p_isdir) return -1;

    find_ctx_t fcx = { name, name_len, 0, 0 };
    walk_dir(fs, pfc, p_nofat, psize, find_cb, &fcx);
    if (!fcx.found) return -1;

    exfat_handle_t h = fcx.found_handle;
    uint32_t se_off = (uint32_t)h;

    exfat_dir_cache_t *pc = dir_cache_get(fs, pfc, p_nofat, psize); // только что был в кэше (walk_dir)
    if (!pc) return -1;
    uint8_t *se = pc->image + se_off;
    uint8_t *fe = se - 32;
    uint8_t sec_count = fe[1];
    uint16_t attrs = rd16(fe + 4);
    int is_dir = (attrs & 0x10) != 0;
    uint32_t first_cluster = rd32(se + 20);
    int no_fat_chain = (se[1] & 0x02) != 0;
    uint64_t target_size = rd64(se + 24);

    if (is_dir && first_cluster >= 2) {
        any_ctx_t actx = { 0 };
        // Обходит ДРУГОЙ каталог (удаляемый, не родителя) — может вытеснить
        // слот родителя из кэша, если все EXFAT_DIR_CACHE_SLOTS заняты
        // чем-то ещё более свежим; перечитываем pc/se/fe ниже заново на
        // случай именно этого.
        walk_dir(fs, first_cluster, no_fat_chain, target_size, any_cb, &actx);
        if (actx.found) return -1; // не пусто
    }

    if (first_cluster >= 2) {
        if (no_fat_chain) {
            uint32_t n = (uint32_t)((target_size + fs->cluster_size - 1) / fs->cluster_size);
            for (uint32_t i = 0; i < n; i++) free_cluster(fs, first_cluster + i);
        } else {
            uint32_t c = first_cluster;
            while (c >= 2 && c < EXFAT_FAT_BAD) {
                uint32_t next = get_fat(fs, c);
                free_cluster(fs, c);
                set_fat(fs, c, 0);
                c = next;
            }
        }
    }

    pc = dir_cache_get(fs, pfc, p_nofat, psize);
    if (!pc) return -1;
    se = pc->image + se_off;
    fe = se - 32;
    for (int i = 0; i < 1 + sec_count; i++) fe[i * 32] &= ~EXFAT_ENTRY_INUSE;
    pc->dirty = 1;
    dir_cache_flush_slot(fs, pc);
    return 0;
}
