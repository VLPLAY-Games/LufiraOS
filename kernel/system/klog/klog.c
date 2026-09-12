#include "klog.h"
#include "lib/types.h"
#include "lib/stdarg.h"
#include "drivers/console/console.h"
#include "fs/lufirafs/lufirafs.h"

#define KLOG_PATH "/logs/system.log"
#define KLOG_LINE_MAX 160

extern lufirafs_t lufirafs;
extern int lufirafs_mounted;

static void append_str(char *out, uint32_t *pos, uint32_t max, const char *s) {
    if (!s) return;
    while (*s && *pos < max - 1) out[(*pos)++] = *s++;
}

// Урезанная копия форматтера printf() из console.c (%s/%c/%d/%u/%p/%x/%l*),
// но пишет в буфер вместо экрана — нужна отдельная реализация, так как
// печатающий printf() работает через put_char() посимвольно.
static void klog_format(char *out, uint32_t max, const char *format, va_list args) {
    uint32_t pos = 0;
    char numbuf[32];

    while (*format && pos < max - 1) {
        if (*format != '%') {
            out[pos++] = *format++;
            continue;
        }

        format++;
        switch (*format) {
            case '%': out[pos++] = '%'; break;
            case 's': append_str(out, &pos, max, va_arg(args, char*)); break;
            case 'c': out[pos++] = (char)va_arg(args, int); break;
            case 'd': itoa(va_arg(args, int64_t), numbuf, 10); append_str(out, &pos, max, numbuf); break;
            case 'u': utoa(va_arg(args, uint64_t), numbuf, 10); append_str(out, &pos, max, numbuf); break;
            case 'p':
                append_str(out, &pos, max, "0x");
                utoa(va_arg(args, uint64_t), numbuf, 16);
                append_str(out, &pos, max, numbuf);
                break;
            case 'x':
                utoa(va_arg(args, uint64_t), numbuf, 16);
                append_str(out, &pos, max, numbuf);
                break;
            case 'l':
                format++;
                if (*format == 'l') format++;
                if (*format == 'x') { utoa(va_arg(args, uint64_t), numbuf, 16); append_str(out, &pos, max, numbuf); }
                else if (*format == 'u') { utoa(va_arg(args, uint64_t), numbuf, 10); append_str(out, &pos, max, numbuf); }
                else if (*format == 'd') { itoa(va_arg(args, int64_t), numbuf, 10); append_str(out, &pos, max, numbuf); }
                else if (*format == 's') { append_str(out, &pos, max, va_arg(args, char*)); }
                break;
            default: break;
        }
        format++;
    }

    out[pos] = '\0';
}

void klog_init(void) {
    if (!lufirafs_mounted) return;

    uint32_t ino;
    if (lufirafs_lookup(&lufirafs, LUFIRAFS_ROOT_INODE, KLOG_PATH, &ino) == 0) return;

    uint32_t parent;
    char name[LUFIRAFS_MAX_NAME + 1];
    if (lufirafs_resolve_parent(&lufirafs, LUFIRAFS_ROOT_INODE, KLOG_PATH, &parent, name) != 0) return;

    uint32_t out_ino;
    lufirafs_create(&lufirafs, parent, name, LUFIRAFS_MODE_FILE, &out_ino);
}

void klog(const char *format, ...) {
    if (!lufirafs_mounted) return;

    char line[KLOG_LINE_MAX];
    va_list args;
    va_start(args, format);
    klog_format(line, sizeof(line), format, args);
    va_end(args);

    uint32_t len = 0;
    while (line[len]) len++;
    if (len < sizeof(line) - 1) line[len++] = '\n';

    uint32_t ino;
    if (lufirafs_lookup(&lufirafs, LUFIRAFS_ROOT_INODE, KLOG_PATH, &ino) != 0) return;

    lufirafs_inode_t inode;
    if (lufirafs_read_inode(&lufirafs, ino, &inode) != 0) return;

    lufirafs_write(&lufirafs, ino, inode.size, line, len);
    lufirafs_sync(&lufirafs);
}
