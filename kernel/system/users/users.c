#include "users.h"
#include "fs/lufirafs/lufirafs.h"
#include "system/mm/heap.h"
#include "system/timer/pit.h"
#include "lib/string.h"
#include "lib/sha256.h"

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

// SHA-256(salt (4 байта, big-endian) || password) — заменяет прежний
// двойной FNV-1a (некриптографический, уязвим к целевому подбору пароля по
// (соль,хэш) из /etc/passwd). Соль всё так же best-effort (generate_salt()
// ниже — pit_get_ticks(), не настоящая энтропия), её роль ограничена тем,
// чтобы одна готовая радужная таблица не покрывала всех пользователей сразу
// — не замена честной KDF (PBKDF2/bcrypt/argon2) против целевого перебора
// конкретной пары (соль,хэш), но несопоставимо сильнее FNV-1a.
static void salted_password_hash(uint32_t salt, const char *password, uint8_t out[32]) {
    uint8_t salt_be[4] = {
        (uint8_t)(salt >> 24), (uint8_t)(salt >> 16), (uint8_t)(salt >> 8), (uint8_t)salt
    };
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, salt_be, sizeof(salt_be));
    sha256_update(&ctx, password, strlen(password));
    sha256_final(&ctx, out);
}

// Лучшее, что доступно без настоящего источника энтропии — тики PIT,
// перемешанные с чем-то, что меняется от вызова к вызову (номер попытки).
// Явно НЕ криптографическая соль, просто защита от идентичных солей у двух
// пользователей, заведённых в один и тот же тик.
static uint32_t generate_salt(void) {
    static uint32_t counter = 0;
    counter++;
    return (uint32_t)pit_get_ticks() ^ (counter * 0x9e3779b9u) ^ 0xA5A5A5A5u;
}

static void format_hex8(uint32_t v, char *out) {
    static const char digits[] = "0123456789abcdef";
    for (int i = 7; i >= 0; i--) {
        out[i] = digits[v & 0xF];
        v >>= 4;
    }
    out[8] = '\0';
}

// SHA-256-дайджест (32 байта) -> 64 hex-символа — hex_to_int()
// (lib/string.c) тут не годится, он копит результат в один int. out
// должен вмещать 2*n_bytes+1 байт.
static void bytes_to_hex(const uint8_t *data, int n_bytes, char *out) {
    static const char digits[] = "0123456789abcdef";
    for (int i = 0; i < n_bytes; i++) {
        out[i * 2]     = digits[(data[i] >> 4) & 0xF];
        out[i * 2 + 1] = digits[data[i] & 0xF];
    }
    out[n_bytes * 2] = '\0';
}

// Обратное — парсит до n_bytes байт из hex-строки hex (как раз /etc/passwd
// после parse_passwd()'s split_fields()). Короче/мусорная строка — остаток
// out зануляется, не читаем за пределы hex.
static void hex_to_bytes(const char *hex, uint8_t *out, int n_bytes) {
    for (int i = 0; i < n_bytes; i++) {
        int hi = 0, lo = 0;
        char c1 = hex[i * 2];
        char c2 = c1 ? hex[i * 2 + 1] : '\0';
        if (c1 >= '0' && c1 <= '9') hi = c1 - '0';
        else if (c1 >= 'a' && c1 <= 'f') hi = c1 - 'a' + 10;
        else if (c1 >= 'A' && c1 <= 'F') hi = c1 - 'A' + 10;
        else { memset(out + i, 0, n_bytes - i); return; }
        if (c2 >= '0' && c2 <= '9') lo = c2 - '0';
        else if (c2 >= 'a' && c2 <= 'f') lo = c2 - 'a' + 10;
        else if (c2 >= 'A' && c2 <= 'F') lo = c2 - 'A' + 10;
        else { memset(out + i, 0, n_bytes - i); return; }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
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
            // username:uid:gid:salt_hex:hash_hex:home
            char *fields[6];
            int n = split_fields(line, fields, 6);
            if (n >= 5) {
                user_entry_t *u = &g_users[count];
                copy_bounded(u->username, fields[0], sizeof(u->username));
                u->uid = (uint32_t)atoi(fields[1]);
                u->gid = (uint32_t)atoi(fields[2]);
                u->password_salt = (uint32_t)hex_to_int(fields[3]);
                hex_to_bytes(fields[4], u->password_hash, sizeof(u->password_hash));
                if (n >= 6) {
                    copy_bounded(u->home, fields[5], sizeof(u->home));
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
    uint8_t computed[32];
    salted_password_hash(u.password_salt, password ? password : "", computed);
    return memcmp(computed, u.password_hash, sizeof(computed)) == 0;
}

// Строит "username:uid:gid:salt_hex:hash_hex:home\n" в line (capacity cap),
// как parse_passwd() потом ожидает его прочитать. Возвращает длину строки
// (без завершающего '\0', который тем не менее тоже пишется).
static int build_user_line(const user_entry_t *u, char *line, int cap) {
    char uid_buf[12], gid_buf[12], salt_hex[9], hash_hex[65];
    format_dec(u->uid, uid_buf);
    format_dec(u->gid, gid_buf);
    format_hex8(u->password_salt, salt_hex);
    bytes_to_hex(u->password_hash, sizeof(u->password_hash), hash_hex);

    const char *parts[9] = {
        u->username, ":", uid_buf, ":", gid_buf, ":", salt_hex, ":", hash_hex
    };
    int pos = 0;
    for (int i = 0; i < 9; i++) {
        const char *s = parts[i];
        while (*s && pos < cap - 1) line[pos++] = *s++;
    }
    if (pos < cap - 1) line[pos++] = ':';
    {
        const char *s = (u->home[0]) ? u->home : "/";
        while (*s && pos < cap - 1) line[pos++] = *s++;
    }
    if (pos < cap - 1) line[pos++] = '\n';
    line[pos] = '\0';
    return pos;
}

// Дописывает новую строку в конец /etc/passwd и в g_users[] — не
// перечитывает файл целиком, новая запись видна сразу.
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

    user_entry_t u;
    memset(&u, 0, sizeof(u));
    copy_bounded(u.username, name, sizeof(u.username));
    u.uid = uid;
    u.gid = gid;
    u.password_salt = generate_salt();
    salted_password_hash(u.password_salt, password ? password : "", u.password_hash);
    copy_bounded(u.home, (home && *home) ? home : "/", sizeof(u.home));
    u.in_use = 1;

    char line[256]; // hash_hex теперь 64 символа (SHA-256), не 8 — см. salted_password_hash()
    int len = build_user_line(&u, line, sizeof(line));

    if (lufirafs_write(&lufirafs, ino, inode.size, line, len) < 0) return -1;
    lufirafs_sync(&lufirafs);

    g_users[slot] = u;
    return 0;
}

// Перезаписывает /etc/passwd целиком из g_users[] — единственный способ
// ИЗМЕНИТЬ уже существующую строку (users_add() выше только дописывает в
// конец, годится для новых пользователей, но не для смены пароля).
static int rewrite_passwd_file(void) {
    uint32_t ino;
    if (lufirafs_lookup(&lufirafs, lufirafs.sb.root_inode, PASSWD_PATH, &ino) != 0) return -1;
    if (lufirafs_truncate(&lufirafs, ino, 0) != 0) return -1;

    uint32_t offset = 0;
    for (int i = 0; i < MAX_USERS; i++) {
        if (!g_users[i].in_use) continue;
        char line[256]; // hash_hex теперь 64 символа (SHA-256), не 8 — см. salted_password_hash()
        int len = build_user_line(&g_users[i], line, sizeof(line));
        if (lufirafs_write(&lufirafs, ino, offset, line, len) < 0) return -1;
        offset += (uint32_t)len;
    }

    lufirafs_sync(&lufirafs);
    return 0;
}

int users_set_password(const char *name, const char *new_password) {
    for (int i = 0; i < MAX_USERS; i++) {
        if (!g_users[i].in_use || strcmp(g_users[i].username, name) != 0) continue;
        g_users[i].password_salt = generate_salt();
        salted_password_hash(g_users[i].password_salt, new_password ? new_password : "", g_users[i].password_hash);
        return rewrite_passwd_file();
    }
    return -1;
}

// Хэш заводского пароля root'а ("toor") из tools/seed/passwd, пересчитанный
// с ЕГО ЖЕ солью — прямое сравнение "хэш совпадает с текущим" было бы
// некорректно после смены соли без смены пароля (на практике не бывает,
// users_set_password() меняет оба сразу, но так надёжнее).
int users_root_has_default_password(void) {
    user_entry_t root;
    if (users_lookup_by_uid(0, &root) != 0) return 0;
    uint8_t computed[32];
    salted_password_hash(root.password_salt, "toor", computed);
    return memcmp(computed, root.password_hash, sizeof(computed)) == 0;
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
