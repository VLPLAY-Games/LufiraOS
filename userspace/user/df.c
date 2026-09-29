// df.c — вынос команды "df" (kernel/shell/commands/filesystem.c,
// command_df()) в отдельную userspace-программу поверх нового
// SYS_STATFS (v0.7, план, этап 1).
//
// Сборка — как у free.c/hello.c.

#include <stdio.h>
#include <lufira/syscall.h>

int main(void) {
    struct lufira_statfs sfs;
    long r = sys_statfs(&sfs);
    if (r != 0) {
        printf("df: SYS_STATFS failed (%ld)\n", r);
        return 1;
    }

    unsigned long used_blocks = (unsigned long)(sfs.total_blocks - sfs.free_blocks);
    unsigned long total_kb = ((unsigned long)sfs.total_blocks * sfs.block_size) / 1024;
    unsigned long used_kb = (used_blocks * sfs.block_size) / 1024;
    unsigned long free_kb = ((unsigned long)sfs.free_blocks * sfs.block_size) / 1024;

    printf("Filesystem: LufiraFS (block size %lu bytes)\n", (unsigned long)sfs.block_size);
    printf("Total: %lu KB\n", total_kb);
    printf("Used:  %lu KB\n", used_kb);
    printf("Free:  %lu KB\n", free_kb);
    printf("Inodes: %lu total, %lu free\n",
           (unsigned long)sfs.inode_count, (unsigned long)sfs.free_inodes);
    return 0;
}
