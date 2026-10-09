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

static void mark_dirty(exfat_fs_t *fs, uint32_t byte_off, uint32_t len) {
    if (!fs->dirty_map || len == 0) return;
    uint32_t first = byte_off / 512;
    uint32_t last = (byte_off + len - 1) / 512;
    for (uint32_t lba = first; lba <= last && (lba >> 3) < fs->dirty_map_size; lba++)
        fs->dirty_map[lba >> 3] |= (1 << (lba & 7));
}

static inline uint8_t *cluster_ptr(exfat_fs_t *fs, uint32_t cluster) {
    return fs->image + fs->cluster_heap_offset_bytes + (uint64_t)(cluster - 2) * fs->cluster_size;
}
static inline uint8_t *fat_entry_ptr(exfat_fs_t *fs, uint32_t cluster) {
    return fs->image + fs->fat_offset_bytes + (uint64_t)cluster * 4;
}
static uint32_t get_fat(exfat_fs_t *fs, uint32_t cluster) { return rd32(fat_entry_ptr(fs, cluster)); }
static void set_fat(exfat_fs_t *fs, uint32_t cluster, uint32_t val) {
    wr32(fat_entry_ptr(fs, cluster), val);
    mark_dirty(fs, (uint32_t)(fat_entry_ptr(fs, cluster) - fs->image), 4);
}

static uint32_t alloc_cluster(exfat_fs_t *fs) {
    if (!fs->bitmap) return 0;
    for (uint32_t i = 0; i < fs->bitmap_bits; i++) {
        if (!(fs->bitmap[i >> 3] & (1 << (i & 7)))) {
            fs->bitmap[i >> 3] |= (1 << (i & 7));
            mark_dirty(fs, (uint32_t)(&fs->bitmap[i >> 3] - fs->image), 1);
            uint32_t cluster = i + 2;
            memset(cluster_ptr(fs, cluster), 0, fs->cluster_size);
            mark_dirty(fs, (uint32_t)(cluster_ptr(fs, cluster) - fs->image), fs->cluster_size);
            return cluster;
        }
    }
    return 0;
}

static void free_cluster(exfat_fs_t *fs, uint32_t cluster) {
    uint32_t i = cluster - 2;
    fs->bitmap[i >> 3] &= ~(1 << (i & 7));
    mark_dirty(fs, (uint32_t)(&fs->bitmap[i >> 3] - fs->image), 1);
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
// новый кластер — используется только для роста КАТАЛОГОВ (у них к этому
// моменту first_cluster уже гарантированно ненулевой, см. exfat_create()).
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

static void get_meta(exfat_fs_t *fs, exfat_handle_t h, uint32_t *first_cluster, uint64_t *size, int *no_fat_chain, int *is_dir) {
    if (h == EXFAT_ROOT_HANDLE) {
        *first_cluster = fs->root_cluster;
        *size = 0;
        *no_fat_chain = 0;
        *is_dir = 1;
        return;
    }
    uint8_t *se = fs->image + h;
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

typedef int (*dir_visit_cb_t)(void *ctx, uint32_t stream_off, const char *name, int name_len, int is_dir);

// Обходит набор (File 0x85 + Stream 0xC0 + Name 0xC1 ×N) по кластерной
// цепочке каталога; cb вызывается один раз на КАЖДЫЙ набор InUse (0x85 с
// битом 0x80) — EntryType==0x00 означает конец каталога (гарантия спеки:
// неиспользуемый хвост всегда занулён), набор с снятым 0x80 (удалённый) —
// просто пропускается, не вызывая cb.
static int walk_dir(exfat_fs_t *fs, uint32_t first_cluster, int no_fat_chain, dir_visit_cb_t cb, void *ctx) {
    uint32_t cidx = 0;
    for (;;) {
        uint32_t cluster = cluster_for_index(fs, first_cluster, no_fat_chain, cidx);
        if (cluster == 0) return 0;
        uint8_t *base = cluster_ptr(fs, cluster);
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
                uint32_t stream_off = (uint32_t)(stream - fs->image);
                if (cb(ctx, stream_off, name, got, is_dir)) return 1;
                off += 32u * (1 + sec_count);
            } else {
                off += 32;
            }
        }
        cidx++;
    }
}

typedef struct { const char *name; int name_len; uint32_t found_stream_off; int found; } find_ctx_t;

static int find_cb(void *vctx, uint32_t stream_off, const char *name, int name_len, int is_dir) {
    (void)is_dir;
    find_ctx_t *ctx = (find_ctx_t *)vctx;
    if (name_len != ctx->name_len) return 0;
    for (int i = 0; i < name_len; i++) {
        char a = name[i], b = ctx->name[i];
        if (a >= 'a' && a <= 'z') a -= 32;
        if (b >= 'a' && b <= 'z') b -= 32;
        if (a != b) return 0;
    }
    ctx->found_stream_off = stream_off;
    ctx->found = 1;
    return 1;
}

typedef struct { int found; } any_ctx_t;
static int any_cb(void *vctx, uint32_t stream_off, const char *name, int name_len, int is_dir) {
    (void)stream_off; (void)name; (void)name_len; (void)is_dir;
    ((any_ctx_t *)vctx)->found = 1;
    return 1;
}

static int find_free_run(exfat_fs_t *fs, uint32_t first_cluster, int need_slots, uint32_t *out_cluster, uint32_t *out_offset) {
    uint32_t cidx = 0;
    for (;;) {
        uint32_t cluster = cluster_for_index(fs, first_cluster, 0, cidx);
        if (cluster == 0) return -1;
        uint8_t *base = cluster_ptr(fs, cluster);
        uint32_t off = 0;
        while (off < fs->cluster_size) {
            if (base[off] == 0x00) {
                if (off + 32u * (uint32_t)need_slots <= fs->cluster_size) {
                    *out_cluster = cluster; *out_offset = off;
                    return 0;
                }
                return -1;
            }
            off += 32;
        }
        cidx++;
    }
}

/* ============================ Инициализация ============================ */

int exfat_init(exfat_fs_t *fs, uint8_t *image, uint32_t image_size) {
    memset(fs, 0, sizeof(*fs));
    if (image_size < 512) return -1;
    fs->image = image;
    fs->image_size = image_size;

    for (int i = 0; i < 8; i++) if (image[3 + i] != "EXFAT   "[i]) return -1;
    if (rd16(image + 510) != 0xAA55) return -1;

    uint8_t bps_shift = image[108];
    uint8_t spc_shift = image[109];
    if (bps_shift > 16 || spc_shift > 16) return -1;
    fs->bytes_per_sector = 1u << bps_shift;
    fs->sectors_per_cluster = 1u << spc_shift;
    fs->cluster_size = fs->bytes_per_sector * fs->sectors_per_cluster;
    if (fs->cluster_size == 0) return -1;

    fs->fat_offset_bytes = rd32(image + 80) * fs->bytes_per_sector;
    fs->fat_length_bytes = rd32(image + 84) * fs->bytes_per_sector;
    fs->cluster_heap_offset_bytes = rd32(image + 88) * fs->bytes_per_sector;
    fs->cluster_count = rd32(image + 92);
    fs->root_cluster = rd32(image + 96);
    if (fs->root_cluster < 2 || fs->cluster_count == 0) return -1;
    if ((uint64_t)fs->cluster_heap_offset_bytes + (uint64_t)fs->cluster_count * fs->cluster_size > image_size) return -1;

    uint32_t map_size = (image_size / 512 + 7) / 8;
    fs->dirty_map = (uint8_t *)kmalloc(map_size);
    if (!fs->dirty_map) return -1;
    memset(fs->dirty_map, 0, map_size);
    fs->dirty_map_size = map_size;

    // Ищем Allocation Bitmap (0x81) среди записей корня — единственный
    // источник истины о занятости кластеров (см. разбор в exfat.h).
    uint32_t cidx = 0;
    for (;;) {
        uint32_t cluster = cluster_for_index(fs, fs->root_cluster, 0, cidx);
        if (cluster == 0) break;
        uint8_t *base = cluster_ptr(fs, cluster);
        int stop = 0;
        for (uint32_t off = 0; off < fs->cluster_size; off += 32) {
            uint8_t et = base[off];
            if (et == 0x00) { stop = 1; break; }
            if (et == 0x81) {
                uint32_t bc = rd32(base + off + 20);
                if (bc >= 2) {
                    fs->bitmap_cluster = bc;
                    fs->bitmap = cluster_ptr(fs, bc);
                    fs->bitmap_bits = fs->cluster_count;
                }
            }
        }
        if (stop) break;
        cidx++;
    }

    return 0;
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
        walk_dir(fs, fc, nofat, find_cb, &fcx);
        if (!fcx.found) return -1;
        cur = fcx.found_stream_off;
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
        walk_dir(fs, fc, nofat, find_cb, &fcx);
        if (!fcx.found) return -1;
        cur = fcx.found_stream_off;
        p = rest;
    }
}

typedef struct { int index; int cur; char *name_out; exfat_handle_t *h_out; int *is_dir_out; int found; } readdir_ctx_t;

static int readdir_cb(void *vctx, uint32_t stream_off, const char *name, int name_len, int is_dir) {
    readdir_ctx_t *ctx = (readdir_ctx_t *)vctx;
    if (ctx->cur == ctx->index) {
        memcpy(ctx->name_out, name, (size_t)name_len);
        ctx->name_out[name_len] = '\0';
        *ctx->h_out = stream_off;
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
    walk_dir(fs, fc, nofat, readdir_cb, &ctx);
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
        if (!cluster) memset(out + done, 0, chunk);
        else memcpy(out + done, cluster_ptr(fs, cluster) + in_off, chunk);
        done += chunk;
    }
    return (int)done;
}

int exfat_write_file(exfat_fs_t *fs, exfat_handle_t h, uint64_t offset, const void *buf, uint32_t count) {
    if (h == EXFAT_ROOT_HANDLE) return -1;
    uint8_t *se = fs->image + h;
    if (se[1] & 0x02) return -1; // NoFatChain — чужой контейнер, не растим (см. exfat.h)

    uint32_t fc = rd32(se + 20);
    const uint8_t *in = (const uint8_t *)buf;
    uint32_t done = 0;

    while (done < count) {
        uint64_t file_off = offset + done;
        uint32_t idx = (uint32_t)(file_off / fs->cluster_size);
        uint32_t in_off = (uint32_t)(file_off % fs->cluster_size);
        uint32_t chunk = fs->cluster_size - in_off;
        if (chunk > count - done) chunk = count - done;

        uint32_t cluster = cluster_for_index_alloc(fs, &fc, idx);
        if (!cluster) break;
        memcpy(cluster_ptr(fs, cluster) + in_off, in + done, chunk);
        mark_dirty(fs, (uint32_t)(cluster_ptr(fs, cluster) - fs->image) + in_off, chunk);
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
        mark_dirty(fs, (uint32_t)(fe - fs->image), 32u * (uint32_t)(1 + fe[1]));
    }
    return done > 0 ? (int)done : -1;
}

void exfat_truncate(exfat_fs_t *fs, exfat_handle_t h) {
    if (h == EXFAT_ROOT_HANDLE) return;
    uint8_t *se = fs->image + h;
    uint32_t fc = rd32(se + 20);
    int nofat = (se[1] & 0x02) != 0;

    if (fc >= 2) {
        if (nofat) {
            uint64_t size = rd64(se + 24);
            uint32_t n = (uint32_t)((size + fs->cluster_size - 1) / fs->cluster_size);
            for (uint32_t i = 0; i < n; i++) free_cluster(fs, fc + i);
        } else {
            uint32_t c = fc;
            while (c >= 2 && c < EXFAT_FAT_BAD) {
                uint32_t next = get_fat(fs, c);
                free_cluster(fs, c);
                set_fat(fs, c, 0);
                c = next;
            }
        }
    }

    wr32(se + 20, 0);
    wr64(se + 24, 0);
    wr64(se + 8, 0);
    se[1] &= ~0x02;
    recompute_checksum(se - 32);
    mark_dirty(fs, (uint32_t)(se - 32 - fs->image), 32);
    mark_dirty(fs, (uint32_t)(se - fs->image), 32);
}

/* ============================ Создание / удаление ========================= */

int exfat_create(exfat_fs_t *fs, exfat_handle_t parent, const char *name, int is_dir, exfat_handle_t *out) {
    int name_len = (int)strlen(name);
    if (name_len == 0 || name_len > 255) return -1;

    uint32_t pfc; uint64_t psize; int p_nofat, p_isdir;
    get_meta(fs, parent, &pfc, &psize, &p_nofat, &p_isdir);
    if (!p_isdir) return -1;

    find_ctx_t fcx = { name, name_len, 0, 0 };
    walk_dir(fs, pfc, p_nofat, find_cb, &fcx);
    if (fcx.found) return -1;

    int name_entries = (name_len + 14) / 15;
    int sec_count = 1 + name_entries;
    int need_slots = 1 + sec_count;

    uint32_t cluster, offset;
    if (find_free_run(fs, pfc, need_slots, &cluster, &offset) != 0) {
        uint32_t nc = extend_chain(fs, pfc);
        if (!nc) return -1;
        cluster = nc; offset = 0;
    }

    uint8_t *base = cluster_ptr(fs, cluster);
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
        uint16_t c = name_u16[i];
        if (c >= 'a' && c <= 'z') c = (uint16_t)(c - 32);
        hash = (uint16_t)(((hash << 15) | (hash >> 1)) + (c & 0xFF));
        hash = (uint16_t)(((hash << 15) | (hash >> 1)) + (c >> 8));
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
    mark_dirty(fs, (uint32_t)(fe - fs->image), 32u * (uint32_t)need_slots);

    *out = (uint32_t)(se - fs->image);
    return 0;
}

int exfat_unlink(exfat_fs_t *fs, exfat_handle_t parent, const char *name) {
    int name_len = (int)strlen(name);
    uint32_t pfc; uint64_t psize; int p_nofat, p_isdir;
    get_meta(fs, parent, &pfc, &psize, &p_nofat, &p_isdir);
    if (!p_isdir) return -1;

    find_ctx_t fcx = { name, name_len, 0, 0 };
    walk_dir(fs, pfc, p_nofat, find_cb, &fcx);
    if (!fcx.found) return -1;

    uint8_t *se = fs->image + fcx.found_stream_off;
    uint8_t *fe = se - 32;
    uint8_t sec_count = fe[1];
    uint16_t attrs = rd16(fe + 4);
    int is_dir = (attrs & 0x10) != 0;
    uint32_t first_cluster = rd32(se + 20);
    int no_fat_chain = (se[1] & 0x02) != 0;

    if (is_dir && first_cluster >= 2) {
        any_ctx_t actx = { 0 };
        walk_dir(fs, first_cluster, no_fat_chain, any_cb, &actx);
        if (actx.found) return -1; // не пусто
    }

    if (first_cluster >= 2) {
        if (no_fat_chain) {
            uint64_t size = rd64(se + 24);
            uint32_t n = (uint32_t)((size + fs->cluster_size - 1) / fs->cluster_size);
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

    for (int i = 0; i < 1 + sec_count; i++) fe[i * 32] &= ~EXFAT_ENTRY_INUSE;
    mark_dirty(fs, (uint32_t)(fe - fs->image), 32u * (uint32_t)(1 + sec_count));
    return 0;
}
