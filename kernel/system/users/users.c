#include "users.h"
#include "fs/lufirafs/lufirafs.h"
#include "system/mm/heap.h"
#include "lib/string.h"

#define PASSWD_PATH "/etc/passwd"
#define GROUP_PATH  "/etc/group"

extern lufirafs_t lufirafs;

static user_entry_t g_users[MAX_USERS];
static group_entry_t g_groups[MAX_GROUPS];

// strncpy в этом freestanding lib/string.h нет (см. тот же комментарий в
// lufirafs.c) — max_len тут ЁМКОСТЬ БУФЕРА, последний байт всегда под '\0'.
static void copy_bounded(char *dest, const char *src, int max_len) {
    int i = 0;
    while (i < max_len - 1 && src[i]) { dest[i] = src[i]; i++; }
    dest[i] = '\0';
}

// FNV-1a 32-бит — простой некриптографический хэш, тут ничего сильнее не
// требуется (см. план: "хобби-ОС, никакой crypto-библиотеки тут нет").
static uint32_t fnv1a_hash(const char *s) {
    uint32_t h = 0x811c9dc5u;
    while (*s) {
        h ^= (uint8_t)(*s++);
        h *= 0x01000193u;
    }
    return h;
}

static void format_hex8(uint32_t v, char *out) {
    static const char digits[] = "0123456789abcdef";
    for (int i = 7; i >= 0; i--) {
        out[i] = digits[v & 0xF];
        v >>= 4;
    }
    out[8] = '\0';
}

// uint -> десятичная строка, без libc (snprintf тут нет).
static void format_dec(uint32_t v, char *out) {
    char tmp[12];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    out[n] = '\0';
}

// Режет line на поля по ':', мутируя саму строку (заменяет ':' на '\0'),
// как split_path/аналоги в остальном шелле — своего strtok в этом
// freestanding lib/string.h нет.
static int split_fields(char *line, char *fields[], int max_fields) {
    int count = 0;
    char *p = line;
    fields[count++] = p;
    while (*p && count < max_fields) {
        if (*p == ':') {
            *p = '\0';
            p++;
            fields[count++] = p;
        } else {
            p++;
        }
    }
    return count;
}

// Читает файл целиком в свежевыделенный NUL-терминированный буфер.
// Возвращает NULL, если файла нет/ошибка чтения (например, если /etc ещё
// не размечен — users_init() тогда просто оставит таблицы пустыми).
static char *read_whole_file(const char *path, uint32_t *out_size) {
    uint32_t ino;
    if (lufirafs_lookup(&lufirafs, lufirafs.sb.root_inode, path, &ino) != 0) return NULL;

    lufirafs_inode_t inode;
    if (lufirafs_read_inode(&lufirafs, ino, &inode) != 0) return NULL;

    char *buf = (char *)kmalloc(inode.size + 1);
    if (!buf) return NULL;

    int br = lufirafs_read(&lufirafs, ino, 0, buf, inode.size);
    if (br < 0) { kfree(buf); return NULL; }

    buf[br] = '\0';
    if (out_size) *out_size = (uint32_t)br;
    return buf;
}

static void parse_passwd(void) {
    memset(g_users, 0, sizeof(g_users));

    char *content = read_whole_file(PASSWD_PATH, NULL);
    if (!content) return;

    int count = 0;
    char *line = content;
    while (*line && count < MAX_USERS) {
        char *nl = line;
        while (*nl && *nl != '\n') nl++;
        int had_nl = (*nl == '\n');
        *nl = '\0';

        if (*line != '\0' && *line != '#') {
            char *fields[5];
            int n = split_fields(line, fields, 5);
            if (n >= 4) {
                user_entry_t *u = &g_users[count];
                copy_bounded(u->username, fields[0], sizeof(u->username));
                u->uid = (uint32_t)atoi(fields[1]);
                u->gid = (uint32_t)atoi(fields[2]);
                u->password_hash = (uint32_t)hex_to_int(fields[3]);
                if (n >= 5) {
                    copy_bounded(u->home, fields[4], sizeof(u->home));
                } else {
                    u->home[0] = '/';
                    u->home[1] = '\0';
                }
                u->in_use = 1;
                count++;
            }
        }

        line = had_nl ? nl + 1 : nl;
    }

    kfree(content);
}

static void parse_group(void) {
    memset(g_groups, 0, sizeof(g_groups));

    char *content = read_whole_file(GROUP_PATH, NULL);
    if (!content) return;

    int count = 0;
    char *line = content;
    while (*line && count < MAX_GROUPS) {
        char *nl = line;
        while (*nl && *nl != '\n') nl++;
        int had_nl = (*nl == '\n');
        *nl = '\0';

        if (*line != '\0' && *line != '#') {
            char *fields[2];
            int n = split_fields(line, fields, 2);
            if (n >= 2) {
                group_entry_t *g = &g_groups[count];
                copy_bounded(g->groupname, fields[0], sizeof(g->groupname));
                g->gid = (uint32_t)atoi(fields[1]);
                g->in_use = 1;
                count++;
            }
        }

        line = had_nl ? nl + 1 : nl;
    }

    kfree(content);
}

void users_init(void) {
    parse_passwd();
    parse_group();
}

int users_lookup_by_name(const char *name, user_entry_t *out) {
    if (!name) return -1;
    for (int i = 0; i < MAX_USERS; i++) {
        if (g_users[i].in_use && strcmp(g_users[i].username, name) == 0) {
            if (out) *out = g_users[i];
            return 0;
        }
    }
    return -1;
}

int users_lookup_by_uid(uint32_t uid, user_entry_t *out) {
    for (int i = 0; i < MAX_USERS; i++) {
        if (g_users[i].in_use && g_users[i].uid == uid) {
            if (out) *out = g_users[i];
            return 0;
        }
    }
    return -1;
}

int groups_lookup_by_name(const char *name, group_entry_t *out) {
    if (!name) return -1;
    for (int i = 0; i < MAX_GROUPS; i++) {
        if (g_groups[i].in_use && strcmp(g_groups[i].groupname, name) == 0) {
            if (out) *out = g_groups[i];
            return 0;
        }
    }
    return -1;
}

int groups_lookup_by_gid(uint32_t gid, group_entry_t *out) {
    for (int i = 0; i < MAX_GROUPS; i++) {
        if (g_groups[i].in_use && g_groups[i].gid == gid) {
            if (out) *out = g_groups[i];
            return 0;
        }
    }
    return -1;
}

int users_check_password(const char *name, const char *password) {
    user_entry_t u;
    if (users_lookup_by_name(name, &u) != 0) return 0;
    return fnv1a_hash(password ? password : "") == u.password_hash;
}

// Дописывает "username:uid:gid:hash:home\n" в конец /etc/passwd и в
// g_users[] — не перечитывает файл целиком, новая запись видна сразу.
int users_add(const char *name, uint32_t uid, uint32_t gid, const char *password, const char *home) {
    if (!name || !*name) return -1;
    if (users_lookup_by_name(name, NULL) == 0) return -1; // уже существует

    int slot = -1;
    for (int i = 0; i < MAX_USERS; i++) {
        if (!g_users[i].in_use) { slot = i; break; }
    }
    if (slot < 0) return -1;

    uint32_t ino;
    if (lufirafs_lookup(&lufirafs, lufirafs.sb.root_inode, PASSWD_PATH, &ino) != 0) return -1;
    lufirafs_inode_t inode;
    if (lufirafs_read_inode(&lufirafs, ino, &inode) != 0) return -1;

    char hash_hex[9];
    format_hex8(fnv1a_hash(password ? password : ""), hash_hex);

    char line[192];
    int pos = 0;
    const char *parts[6];
    int part_count = 0;
    // Собираем строку вручную (нет snprintf в этом freestanding libc) —
    // как command_edit()/klog_format() в остальном ядре.
    char uid_buf[12], gid_buf[12];
    format_dec(uid, uid_buf);
    format_dec(gid, gid_buf);

    parts[part_count++] = name;
    parts[part_count++] = ":";
    parts[part_count++] = uid_buf;
    parts[part_count++] = ":";
    parts[part_count++] = gid_buf;
    parts[part_count++] = ":";

    for (int i = 0; i < part_count; i++) {
        const char *s = parts[i];
        while (*s && pos < (int)sizeof(line) - 1) line[pos++] = *s++;
    }
    {
        const char *s = hash_hex;
        while (*s && pos < (int)sizeof(line) - 1) line[pos++] = *s++;
    }
    if (pos < (int)sizeof(line) - 1) line[pos++] = ':';
    {
        const char *s = (home && *home) ? home : "/";
        while (*s && pos < (int)sizeof(line) - 1) line[pos++] = *s++;
    }
    if (pos < (int)sizeof(line) - 1) line[pos++] = '\n';
    line[pos] = '\0';

    if (lufirafs_write(&lufirafs, ino, inode.size, line, pos) < 0) return -1;
    lufirafs_sync(&lufirafs);

    user_entry_t *u = &g_users[slot];
    copy_bounded(u->username, name, sizeof(u->username));
    u->uid = uid;
    u->gid = gid;
    u->password_hash = fnv1a_hash(password ? password : "");
    copy_bounded(u->home, (home && *home) ? home : "/", sizeof(u->home));
    u->in_use = 1;

    return 0;
}

int groups_add(const char *name, uint32_t gid) {
    if (!name || !*name) return -1;
    if (groups_lookup_by_name(name, NULL) == 0) return -1;

    int slot = -1;
    for (int i = 0; i < MAX_GROUPS; i++) {
        if (!g_groups[i].in_use) { slot = i; break; }
    }
    if (slot < 0) return -1;

    uint32_t ino;
    if (lufirafs_lookup(&lufirafs, lufirafs.sb.root_inode, GROUP_PATH, &ino) != 0) return -1;
    lufirafs_inode_t inode;
    if (lufirafs_read_inode(&lufirafs, ino, &inode) != 0) return -1;

    char gid_buf[12];
    format_dec(gid, gid_buf);

    char line[80];
    int pos = 0;
    const char *s = name;
    while (*s && pos < (int)sizeof(line) - 1) line[pos++] = *s++;
    if (pos < (int)sizeof(line) - 1) line[pos++] = ':';
    s = gid_buf;
    while (*s && pos < (int)sizeof(line) - 1) line[pos++] = *s++;
    if (pos < (int)sizeof(line) - 1) line[pos++] = '\n';
    line[pos] = '\0';

    if (lufirafs_write(&lufirafs, ino, inode.size, line, pos) < 0) return -1;
    lufirafs_sync(&lufirafs);

    group_entry_t *g = &g_groups[slot];
    copy_bounded(g->groupname, name, sizeof(g->groupname));
    g->gid = gid;
    g->in_use = 1;

    return 0;
}

uint32_t users_next_free_uid(void) {
    uint32_t candidate = 1000;
    for (;;) {
        if (users_lookup_by_uid(candidate, NULL) != 0) return candidate;
        candidate++;
    }
}

uint32_t groups_next_free_gid(void) {
    uint32_t candidate = 1000;
    for (;;) {
        if (groups_lookup_by_gid(candidate, NULL) != 0) return candidate;
        candidate++;
    }
}
