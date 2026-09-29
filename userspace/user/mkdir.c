// mkdir.c — вынос "mkdir" (kernel/shell/commands/filesystem.c,
// command_mkdir()) в отдельную userspace-программу (v0.7 план, этап 5,
// под-этап 1). Прямой перенос без обходных путей: SYS_MKDIR (в отличие от
// SYS_OPEN/SYS_EXEC) уже резолвит путь относительно cwd_inode процесса
// (см. комментарий в kernel/system/syscall/syscall.c), так что никакого
// SYS_GETCWD-склеивания тут не нужно.
//
// SYS_MKDIR теряет часть детализации ошибок, которая была у kernel-native
// команды (та печатала разные сообщения для "уже существует"/"нет inode"/
// "нет места" — сырой код lufirafs_create()); syscall.c сворачивает всё,
// кроме -ENOENT (недоступный родитель) и -EACCES, в один общий -1, так что
// точнее "failed to create" здесь сказать нечего — это ограничение самого
// ABI SYS_MKDIR, а не недосмотр этого порта.

#include <stdio.h>
#include <lufira/syscall.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: mkdir <name>\n");
        return 1;
    }

    long r = sys_mkdir(argv[1], 0755);
    if (r == 0) {
        printf("Directory created: %s\n", argv[1]);
        return 0;
    }
    if (r == -ENOENT) printf("mkdir: no such parent directory: %s\n", argv[1]);
    else if (r == -EACCES) printf("mkdir: permission denied: %s\n", argv[1]);
    else printf("mkdir: failed to create '%s'\n", argv[1]);
    return 1;
}
