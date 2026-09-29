#pragma once

#include "lib/types.h"

// Простая база пользователей/групп поверх текстовых файлов LufiraFS
// (/etc/passwd, /etc/group) — как классический ранний Unix: одна основная
// группа на пользователя, никакого членства в нескольких группах.
// Фиксированные массивы (как MAX_PROCESSES/MAX_MMAP_REGIONS в остальном
// ядре), парсятся один раз при загрузке (users_init(), после
// lufirafs_init(), до создания процесса шелла — см. kernel.c).

#define MAX_USERS  32
#define MAX_GROUPS 32

typedef struct {
    char username[32];
    uint32_t uid;
    uint32_t gid;
    // Соль + двойной FNV-1a (см. users.c) — некриптографический хэш,
    // достаточный только против готовых радужных таблиц по паролю самому
    // по себе (соль честно best-effort: pit_get_ticks(), не настоящая
    // энтропия), а не против целевого перебора.
    uint32_t password_salt;
    uint32_t password_hash;
    char home[64];
    int in_use;
} user_entry_t;

typedef struct {
    char groupname[32];
    uint32_t gid;
    int in_use;
} group_entry_t;

// Читает /etc/passwd и /etc/group в память. Без них (свежий/повреждённый
// диск) таблицы остаются пустыми — whoami/su просто не найдут никого,
// кроме встроенного отката на root (см. users_lookup_by_uid()).
void users_init(void);

int users_lookup_by_name(const char *name, user_entry_t *out);
int users_lookup_by_uid(uint32_t uid, user_entry_t *out);
int groups_lookup_by_name(const char *name, group_entry_t *out);
int groups_lookup_by_gid(uint32_t gid, group_entry_t *out);

// 1 = пароль верный, 0 = неверный или пользователь не найден.
int users_check_password(const char *name, const char *password);

// Дописывает нового пользователя/группу в /etc/passwd, /etc/group на диске
// И в память (виден сразу, без перезагрузки). password может быть "" —
// тогда сохраняется хэш пустой строки (пароль не требуется).
int users_add(const char *name, uint32_t uid, uint32_t gid, const char *password, const char *home);
int groups_add(const char *name, uint32_t gid);

// Меняет пароль уже существующего пользователя (новая соль+хэш, строка в
// /etc/passwd перезаписывается целиком — см. users_rewrite_passwd_file() в
// users.c). Возвращает 0 при успехе, -1 если пользователь не найден.
int users_set_password(const char *name, const char *new_password);

// 1, если хранимый хэш root совпадает с хэшем сid-заводского пароля из
// tools/seed/passwd ("toor") — используется kernel.c для предупреждения при
// загрузке. 0 если root сменил пароль или не найден.
int users_root_has_default_password(void);

// Первый свободный id, начиная с 1000 (0-999 зарезервированы за системными
// учётками, как root=0).
uint32_t users_next_free_uid(void);
uint32_t groups_next_free_gid(void);
