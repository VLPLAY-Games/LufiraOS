// Команды пользователей/групп: whoami, chmod, chown, useradd, groupadd, su.
//
// su/useradd/chown берут username/password/groupname из СЫРОГО
// input_buffer (а не из лоуеркейснутого args), как cat/write уже делают
// для имён файлов — execute_command() лоуеркейсит всю строку целиком
// (shell.c), а пароли/имена пользователей регистрозависимы.
#include "../commands.h"
#include "drivers/console/console.h"
#include "../shell.h"
#include "fs/lufirafs/lufirafs.h"
#include "system/process/process.h"
#include "system/users/users.h"
#include "lib/string.h"

extern lufirafs_t lufirafs;

// Копирует один пробельно-разделённый токен, начиная с p, в dest (ёмкость
// dest_size), возвращает указатель на остаток строки ПОСЛЕ токена (уже
// пропустив разделяющие пробелы) — тот же паттерн разбора, что и в
// command_cp()/command_mv() (filesystem.c), только оформленный как хелпер.
static const char *take_token(const char *p, char *dest, int dest_size) {
    p = skip_spaces(p);
    int len = token_length(p);
    int cl = len < dest_size - 1 ? len : dest_size - 1;
    for (int i = 0; i < cl; i++) dest[i] = p[i];
    dest[cl] = '\0';
    return skip_spaces(p + len);
}

void command_whoami(void) {
    if (!current_process) return;

    user_entry_t u;
    group_entry_t g;
    int have_user = (users_lookup_by_uid(current_process->uid, &u) == 0);
    int have_group = (groups_lookup_by_gid(current_process->gid, &g) == 0);

    printf("\nuid=%u", current_process->uid);
    if (have_user) printf("(%s)", u.username);
    printf(" gid=%u", current_process->gid);
    if (have_group) printf("(%s)", g.groupname);
    printf("\n");
}

// chmod <octal-mode> <path> — владелец файла или root.
void command_chmod(const char *args) {
    if (!args || !*args) {
        printf("\nUsage: chmod <mode> <path>\n");
        return;
    }

    const char *p = skip_spaces(args);
    int mode_len = token_length(p);
    if (mode_len == 0) {
        printf("\nUsage: chmod <mode> <path>\n");
        return;
    }

    uint32_t mode = 0;
    for (int i = 0; i < mode_len; i++) {
        char c = p[i];
        if (c < '0' || c > '7') {
            printf("\nchmod: invalid mode (expected octal, e.g. 644)\n");
            return;
        }
        mode = mode * 8 + (uint32_t)(c - '0');
    }

    const char *path = skip_spaces(p + mode_len);
    if (*path == '\0') {
        printf("\nUsage: chmod <mode> <path>\n");
        return;
    }

    uint32_t ino;
    if (lufirafs_lookup(&lufirafs, cwd_inode, path, &ino) != 0) {
        printf("\nchmod: '%s' not found\n", path);
        return;
    }

    lufirafs_inode_t inode;
    if (lufirafs_read_inode(&lufirafs, ino, &inode) != 0) return;

    if (current_process->uid != 0 && current_process->uid != inode.uid) {
        printf("\nchmod: permission denied: %s\n", path);
        return;
    }

    inode.perm = mode & 0777u;
    lufirafs_write_inode(&lufirafs, ino, &inode);
    lufirafs_sync(&lufirafs);
    printf("\nchmod: updated %s\n", path);
}

// chown <user>[:group] <path> — только root (без POSIX-нюанса "owner может
// сменить группу на свою собственную" — см. план).
void command_chown(const char *args) {
    if (!args || !*args) {
        printf("\nUsage: chown <user>[:group] <path>\n");
        return;
    }

    char owner_spec[64];
    const char *rest = take_token(args, owner_spec, sizeof(owner_spec));
    if (owner_spec[0] == '\0' || *rest == '\0') {
        printf("\nUsage: chown <user>[:group] <path>\n");
        return;
    }

    if (current_process->uid != 0) {
        printf("\nchown: permission denied\n");
        return;
    }

    char path[256];
    take_token(rest, path, sizeof(path));

    char *colon = NULL;
    for (int i = 0; owner_spec[i]; i++) {
        if (owner_spec[i] == ':') { colon = &owner_spec[i]; break; }
    }

    uint32_t new_uid, new_gid;
    user_entry_t u;
    if (colon) {
        *colon = '\0';
        const char *group_part = colon + 1;
        if (users_lookup_by_name(owner_spec, &u) != 0) {
            printf("\nchown: unknown user: %s\n", owner_spec);
            return;
        }
        group_entry_t g;
        if (groups_lookup_by_name(group_part, &g) != 0) {
            printf("\nchown: unknown group: %s\n", group_part);
            return;
        }
        new_uid = u.uid;
        new_gid = g.gid;
    } else {
        if (users_lookup_by_name(owner_spec, &u) != 0) {
            printf("\nchown: unknown user: %s\n", owner_spec);
            return;
        }
        new_uid = u.uid;
        new_gid = u.gid; // основная группа пользователя по умолчанию
    }

    uint32_t ino;
    if (lufirafs_lookup(&lufirafs, cwd_inode, path, &ino) != 0) {
        printf("\nchown: '%s' not found\n", path);
        return;
    }

    lufirafs_inode_t inode;
    if (lufirafs_read_inode(&lufirafs, ino, &inode) != 0) return;
    inode.uid = new_uid;
    inode.gid = new_gid;
    lufirafs_write_inode(&lufirafs, ino, &inode);
    lufirafs_sync(&lufirafs);
    printf("\nchown: updated %s\n", path);
}

// Создаёт /home (если его ещё нет — общий родитель для всех домашних
// каталогов) и /home/<username> внутри него, владелец — сам новый
// пользователь (perm 0700 — приватный каталог, как в большинстве
// дистрибутивов по умолчанию). При любой неудаче тихо откатывается на "/"
// (совместимо со старым поведением — обычным ограниченным на 0755-корне
// новый пользователь просто ничего не сможет создавать, но хотя бы
// залогинится, а не останется вовсе без домашнего каталога).
static void ensure_home_dir(uint32_t uid, uint32_t gid, const char *username,
                             char *out_home, int out_home_size)
{
    uint32_t home_root_ino;
    if (lufirafs_lookup(&lufirafs, lufirafs.sb.root_inode, "/home", &home_root_ino) != 0) {
        if (lufirafs_create(&lufirafs, lufirafs.sb.root_inode, "home", LUFIRAFS_MODE_DIR,
                             0, 0, LUFIRAFS_DEFAULT_DIR_PERM, &home_root_ino) != 0)
        {
            if (out_home_size > 1) { out_home[0] = '/'; out_home[1] = '\0'; }
            else if (out_home_size == 1) { out_home[0] = '\0'; }
            return;
        }
        lufirafs_sync(&lufirafs);
    }

    uint32_t user_home_ino;
    if (lufirafs_lookup(&lufirafs, home_root_ino, username, &user_home_ino) != 0) {
        if (lufirafs_create(&lufirafs, home_root_ino, username, LUFIRAFS_MODE_DIR,
                             uid, gid, 0700, &user_home_ino) != 0)
        {
            if (out_home_size > 1) { out_home[0] = '/'; out_home[1] = '\0'; }
            else if (out_home_size == 1) { out_home[0] = '\0'; }
            return;
        }
        lufirafs_sync(&lufirafs);
    }

    int pos = 0;
    const char *prefix = "/home/";
    while (prefix[pos] && pos < out_home_size - 1) { out_home[pos] = prefix[pos]; pos++; }
    int i = 0;
    while (username[i] && pos < out_home_size - 1) { out_home[pos++] = username[i++]; }
    out_home[pos] = '\0';
}

// useradd <username> <password> [groupname] — без groupname создаёт новую
// группу с именем пользователя (как реальный Linux useradd по умолчанию).
void command_useradd(const char *args) {
    if (!args || !*args) {
        printf("\nUsage: useradd <username> <password> [group]\n");
        return;
    }

    char username[32], password[64], groupname[32];
    const char *rest = take_token(args, username, sizeof(username));
    rest = take_token(rest, password, sizeof(password));
    take_token(rest, groupname, sizeof(groupname));

    if (username[0] == '\0') {
        printf("\nUsage: useradd <username> <password> [group]\n");
        return;
    }

    if (users_lookup_by_name(username, NULL) == 0) {
        printf("\nuseradd: user '%s' already exists\n", username);
        return;
    }

    uint32_t gid;
    if (groupname[0] != '\0') {
        group_entry_t g;
        if (groups_lookup_by_name(groupname, &g) != 0) {
            printf("\nuseradd: unknown group: %s\n", groupname);
            return;
        }
        gid = g.gid;
    } else {
        gid = groups_next_free_gid();
        if (groups_add(username, gid) != 0) {
            printf("\nuseradd: failed to create default group\n");
            return;
        }
    }

    uint32_t uid = users_next_free_uid();

    char home[64];
    ensure_home_dir(uid, gid, username, home, sizeof(home));

    if (users_add(username, uid, gid, password, home) != 0) {
        printf("\nuseradd: failed to add user\n");
        return;
    }

    printf("\nuseradd: created user '%s' (uid=%u gid=%u home=%s)\n", username, uid, gid, home);
}

// groupadd <groupname>
void command_groupadd(const char *args) {
    if (!args || !*args) {
        printf("\nUsage: groupadd <groupname>\n");
        return;
    }

    char groupname[32];
    take_token(args, groupname, sizeof(groupname));
    if (groupname[0] == '\0') {
        printf("\nUsage: groupadd <groupname>\n");
        return;
    }

    if (groups_lookup_by_name(groupname, NULL) == 0) {
        printf("\ngroupadd: group '%s' already exists\n", groupname);
        return;
    }

    uint32_t gid = groups_next_free_gid();
    if (groups_add(groupname, gid) != 0) {
        printf("\ngroupadd: failed\n");
        return;
    }

    printf("\ngroupadd: created group '%s' (gid=%u)\n", groupname, gid);
}

// su <username> [password] — как cd мутирует cwd_inode напрямую, su мутирует
// current_process->uid/gid напрямую (никакого отдельного syscall/сессии —
// шелл САМ И ЕСТЬ current_process). root может su в кого угодно без пароля;
// иначе пароль обязателен и проверяется против ЦЕЛЕВОГО пользователя.
// Видимый ввод пароля (не маскируется) — сознательное упрощение, см. план.
void command_su(const char *args) {
    if (!args || !*args) {
        printf("\nUsage: su <username> [password]\n");
        return;
    }

    char username[32], password[64];
    const char *rest = take_token(args, username, sizeof(username));
    take_token(rest, password, sizeof(password));

    if (username[0] == '\0') {
        printf("\nUsage: su <username> [password]\n");
        return;
    }

    user_entry_t u;
    if (users_lookup_by_name(username, &u) != 0) {
        printf("\nsu: unknown user: %s\n", username);
        return;
    }

    if (current_process->uid != 0 && !users_check_password(username, password)) {
        printf("\nsu: authentication failure\n");
        return;
    }

    current_process->uid = u.uid;
    current_process->gid = u.gid;
    printf("\n");
}
