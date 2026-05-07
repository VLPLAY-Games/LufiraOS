#include "commands.h"
#include "../drivers/console.h"
#include "../shell/shell.h"
#include "../fs/fat.h"

extern fat_fs_t fatfs;
extern char cwd_path[256];
extern uint32_t cwd_first_cluster;

static const char* skip_spaces(const char* s) {
    while (*s == ' ' || *s == '\t') s++;
    return s;
}
static int token_length(const char* s) {
    int len = 0;
    while (s[len] != '\0' && s[len] != ' ' && s[len] != '\t') len++;
    return len;
}
static int token_equals(const char* s, const char* word) {
    int i = 0;
    while (word[i] != '\0' && s[i] == word[i]) i++;
    return word[i] == '\0' && (s[i] == '\0' || s[i] == ' ' || s[i] == '\t');
}
static int interrupts_enabled(void) {
    uint64_t rflags;
    asm volatile ("pushfq; pop %0" : "=r"(rflags));
    return (int)((rflags >> 9) & 1ULL);
}
int atoi(const char* str) {
    int result = 0, sign = 1;
    if (*str == '-') { sign = -1; str++; }
    while (*str >= '0' && *str <= '9') { result = result * 10 + (*str - '0'); str++; }
    return sign * result;
}
int hex_to_int(const char* hex) {
    int result = 0;
    while (*hex) {
        char c = *hex;
        int digit = 0;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        else break;
        result = result * 16 + digit;
        hex++;
    }
    return result;
}

void command_help(void) {
    printf("\nAvailable commands:\n");
    printf(" help - Show this help\n");
    printf(" clear - Clear screen\n");
    printf(" reboot - Reboot system\n");
    printf(" shutdown - Shutdown system\n");
    printf(" version - Show kernel version\n");
    printf(" echo - Echo text back\n");
    printf(" history - Show command history\n");
    printf(" status - Show interrupt/CPU status\n");
    printf(" trap - Trigger test exceptions\n");
    printf(" color - Set console colors or reset\n");
    printf(" colors - Show available colors\n");
    printf(" fg <color> - Set foreground color\n");
    printf(" bg <color> - Set background color\n");
    printf("\nFile system commands:\n");
    printf(" pwd - Print current directory\n");
    printf(" cd <dir> - Change directory\n");
    printf(" ls [-l] - List directory contents\n");
    printf(" mkdir <name> - Create directory\n");
    printf(" rm <name> - Remove file or empty directory\n");
    printf(" cat <file> - Display file content\n");
    printf(" touch <filename> - Create empty file\n");
}
void command_clear(void) { clear_screen(); show_prompt(); }
void command_reboot(void) {
    printf("\nRebooting system...\n");
    __asm__ volatile ("outb %0, %1" : : "a"((uint8_t)0xFE), "Nd"((uint16_t)0x64));
    __asm__ volatile ("outw %0, %1" : : "a"((uint16_t)0x2000), "Nd"((uint16_t)0x604));
    printf("Reboot failed. Please restart manually.\n");
}
void command_shutdown(void) {
    printf("\nShutting down system...\n");
    __asm__ volatile ("outw %0, %1" : : "a"((uint16_t)0x2000), "Nd"((uint16_t)0x604));
    __asm__ volatile ("outw %0, %1" : : "a"((uint16_t)0x3400), "Nd"((uint16_t)0x604));
    __asm__ volatile ("outw %0, %1" : : "a"((uint16_t)0x2000), "Nd"((uint16_t)0xB004));
    printf("Shutdown command sent. System may require manual power off.\n");
}
void command_version(void) {
    printf("\nLufiraOS Kernel v0.2\nBuilt: %s %s\nArchitecture: x86_64\n", __DATE__, __TIME__);
}
void command_status(void) {
    printf("\nSYSTEM STATUS:\n");
    printf("--------------\n");
    printf(" Interrupt Flag: %s\n", interrupts_enabled() ? "SET" : "CLEAR");
    printf(" Interrupts: %s\n", interrupts_enabled() ? "ENABLED" : "DISABLED");
    printf(" CPU Test: trap int3 / ud2 / pf\n");
}
void command_colors(void) {
    printf("\nColor Commands:\nUsage examples:\n");
    printf(" color 1F - Blue background, White text\n");
    printf(" color FF0000 - Red text on current background\n");
    printf(" color 0000FF 00FF00 - Blue text on Green background\n");
    printf(" color reset - Reset to default colors\n");
    printf(" fg 4 - Red text\n");
    printf(" bg 1 - Blue background\n\n");
    print_color_table_16();
}
void command_color(void) {
    char* args = (char*)skip_spaces(input_buffer + 6);
    if (*args == '\0') {
        printf("\nUsage: color <fg> [bg]\n");
        return;
    }
    if (token_equals(args, "reset")) {
        reset_colors(); printf("\nColors reset to default\n");
        return;
    }
    int len = token_length(args);
    if (len == 1 || len == 2) {
        int fg_index = hex_to_int(args);
        int bg_index = COLOR_BLACK;
        const char* second = skip_spaces(args + len);
        if (*second != '\0') { int len2 = token_length(second); if (len2 == 1 || len2 == 2) bg_index = hex_to_int(second); }
        if (fg_index >= 0 && fg_index <= 255 && bg_index >= 0 && bg_index <= 255) {
            set_color_by_index((ConsoleColor)fg_index, (ConsoleColor)bg_index);
            printf("\nColors set: Text=%d (%s), Background=%d (%s)\n", fg_index, get_color_name((ConsoleColor)fg_index), bg_index, get_color_name((ConsoleColor)bg_index));
        } else printf("\nError: Color indices must be between 0-255\n");
    } else if (len == 6) {
        uint32_t fg_rgb = (uint32_t)hex_to_int(args);
        uint32_t bg_rgb = 0x000000;
        const char* second = skip_spaces(args + 6);
        if (*second != '\0') { int len2 = token_length(second); if (len2 == 6) bg_rgb = (uint32_t)hex_to_int(second); }
        set_color_by_rgb(fg_rgb, bg_rgb);
        ConsoleColor closest_fg = find_closest_color(fg_rgb), closest_bg = find_closest_color(bg_rgb);
        printf("\nColors set: Text=#%06X (~%s), Background=#%06X (~%s)\n", fg_rgb, get_color_name(closest_fg), bg_rgb, get_color_name(closest_bg));
    } else printf("\nError: Invalid color format\n");
}
void command_fg(void) {
    char* args = (char*)skip_spaces(input_buffer + 3);
    if (*args == '\0') { printf("\nUsage: fg <color>\n"); return; }
    int len = token_length(args);
    if (len == 1 || len == 2) {
        int color_index = hex_to_int(args);
        if (color_index >= 0 && color_index <= 255) {
            set_foreground_color((ConsoleColor)color_index);
            printf("\nForeground color set to %d (%s)\n", color_index, get_color_name((ConsoleColor)color_index));
        } else printf("\nError: Color index must be between 0-255\n");
    } else if (len == 6) {
        uint32_t rgb = (uint32_t)hex_to_int(args);
        set_foreground_rgb(rgb);
        ConsoleColor closest = find_closest_color(rgb);
        printf("\nForeground color set to #%06X (~%s)\n", rgb, get_color_name(closest));
    } else printf("\nError: Invalid color format\n");
}
void command_bg(void) {
    char* args = (char*)skip_spaces(input_buffer + 3);
    if (*args == '\0') { printf("\nUsage: bg <color>\n"); return; }
    int len = token_length(args);
    if (len == 1 || len == 2) {
        int color_index = hex_to_int(args);
        if (color_index >= 0 && color_index <= 255) {
            set_background_color((ConsoleColor)color_index);
            printf("\nBackground color set to %d (%s)\n", color_index, get_color_name((ConsoleColor)color_index));
        } else printf("\nError: Color index must be between 0-255\n");
    } else if (len == 6) {
        uint32_t rgb = (uint32_t)hex_to_int(args);
        set_background_rgb(rgb);
        ConsoleColor closest = find_closest_color(rgb);
        printf("\nBackground color set to #%06X (~%s)\n", rgb, get_color_name(closest));
    } else printf("\nError: Invalid color format\n");
}
void command_trap(void) {
    char* args = (char*)skip_spaces(input_buffer + 5);
    if (*args == '\0') { printf("\nUsage: trap <int3|ud2|pf|cli|sti|hlt>\n"); return; }
    if (token_equals(args, "int3")) { printf("\nTriggering breakpoint...\n"); asm volatile ("int3"); }
    else if (token_equals(args, "ud2")) { printf("\nTriggering invalid opcode...\n"); asm volatile ("ud2"); }
    else if (token_equals(args, "pf")) { printf("\nTriggering page fault...\n"); volatile uint64_t* bad = (volatile uint64_t*)0x0; *bad = 0xDEADBEEF; }
    else if (token_equals(args, "cli")) { asm volatile ("cli"); printf("\nInterrupt Flag cleared.\n"); }
    else if (token_equals(args, "sti")) { asm volatile ("sti"); printf("\nInterrupt Flag set.\n(All IRQs are masked by PIC)\n"); }
    else if (token_equals(args, "hlt")) { printf("\nHalting CPU.\n"); asm volatile ("hlt"); }
    else printf("\nUnknown trap: %s\n", args);
}

// ---------- НОВЫЕ КОМАНДЫ ФАЙЛОВОЙ СИСТЕМЫ ----------

static int is_dot_or_dotdot(const char* name) {
    if (name[0] == '.' && name[1] == '\0') return 1;
    if (name[0] == '.' && name[1] == '.' && name[2] == '\0') return 1;
    return 0;
}

void command_cd(const char* path) {
    if (!path || !*path) return;

    /* handle ".." */
    if (strcmp(path, "..") == 0) {
        if (cwd_first_cluster == 0 && fatfs.fat_type != 32) {
            /* already root (FAT12/16) */
            return;
        }
        /* read dotdot entry from current directory */
        fat_dir_t dir;
        fat_opendir(&fatfs, cwd_first_cluster, &dir);
        fat_dir_entry_t entry;
        while (fat_readdir(&dir, &entry)) {
            if (entry.name[0] == '.' && entry.name[1] == '.' && entry.name[2] == ' ') {
                uint32_t parent = read_le16((const uint8_t*)&entry.first_cluster_low);
                cwd_first_cluster = parent;
                /* update path string (strip last component) */
                int len = 0; while (cwd_path[len]) len++;
                if (len > 1) {
                    cwd_path[len-1] = '\0';        // remove trailing '/'
                    char *slash = NULL;
                    for (int i = 0; cwd_path[i]; i++)
                        if (cwd_path[i] == '/') slash = &cwd_path[i];
                    if (slash) *slash = '\0';
                    else cwd_path[0] = '/', cwd_path[1] = '\0';
                }
                fat_closedir(&dir);
                return;
            }
        }
        fat_closedir(&dir);
        printf("\ncd: .. not found\n");
        return;
    }

    /* ordinary cd */
    fat_dir_entry_t ent;
    if (fat_find_entry(&fatfs, cwd_first_cluster, path, &ent) == 0) {
        if (!(ent.attr & 0x10)) {
            printf("\ncd: not a directory: %s\n", path);
            return;
        }
        uint32_t new_cluster = read_le16((const uint8_t*)&ent.first_cluster_low);
        cwd_first_cluster = new_cluster;
        /* append name to cwd_path */
        int len = 0; while (cwd_path[len]) len++;
        int plen = 0; while (path[plen]) plen++;
        if (len + 1 + plen < 255) {
            if (cwd_path[0] != '/' || cwd_path[1] != '\0') // not just "/"
                cwd_path[len++] = '/';
            for (int i = 0; path[i]; i++) cwd_path[len++] = path[i];
            cwd_path[len] = '\0';
        }
    } else {
        printf("\ncd: no such directory: %s\n", path);
    }
}

void command_ls(const char* flags) {
    int long_fmt = 0;
    if (flags && flags[0] == '-' && flags[1] == 'l') long_fmt = 1;
    fat_dir_t dir;
    if (fat_opendir(&fatfs, cwd_first_cluster, &dir) != 0) {
        printf("\nCannot open directory\n");
        return;
    }
    printf("\n");
    fat_dir_entry_t entry;
    int count = 0;
    while (fat_readdir(&dir, &entry)) {
        char name[13]; int pos = 0;
        for (int j = 0; j < 8 && entry.name[j] != ' '; j++)
            name[pos++] = (char)entry.name[j];
        if (entry.name[8] != ' ') {
            name[pos++] = '.';
            for (int j = 8; j < 11 && entry.name[j] != ' '; j++)
                name[pos++] = (char)entry.name[j];
        }
        name[pos] = '\0';
        if (long_fmt) {
            uint32_t size = read_le32((const uint8_t*)&entry.file_size);
            char type = (entry.attr & 0x10) ? 'd' : '-';
            printf("%c ", type);
            printf("%u ", size);
            printf("%s\n", name);
        } else {
            printf("%s  ", name);
            if (++count % 4 == 0) printf("\n");
        }
    }
    if (!long_fmt && count % 4 != 0) printf("\n");
    fat_closedir(&dir);
}

void command_mkdir(const char* name) {
    if (!name || !*name) return;
    int res = fat_mkdir(&fatfs, cwd_first_cluster, name);
    if (res == 0) printf("\nDirectory created: %s\n", name);
    else printf("\nmkdir failed (error %d)\n", res);
}

void command_rm(const char* name) {
    if (!name || !*name) return;
    int res = fat_rm(&fatfs, cwd_first_cluster, name);
    if (res == 0) printf("\nRemoved: %s\n", name);
    else printf("\nrm failed (error %d)\n", res);
}

void command_touch(const char* name) {
    if (!name || !*name) {
        printf("\nUsage: touch <filename>\n");
        return;
    }
    int res = fat_create_file(&fatfs, cwd_first_cluster, name);
    if (res == 0) printf("\nFile created: %s\n", name);
    else printf("\ntouch failed (error %d)\n", res);
}
