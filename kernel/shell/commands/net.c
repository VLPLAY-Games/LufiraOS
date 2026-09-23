// Сетевые команды — ifconfig/ping/wget. Драйвер: kernel/drivers/net/rtl8139.c,
// стек: kernel/net/ (eth/arp/ip/icmp/tcp). wget работает только с
// IP-адресами (без DNS) и минимальным блокирующим TCP-клиентом — см. план.
#include "../commands.h"
#include "../shell.h"
#include "drivers/console/console.h"
#include "drivers/net/rtl8139.h"
#include "net/net.h"
#include "net/icmp.h"
#include "net/tcp.h"
#include "fs/lufirafs/lufirafs.h"
#include "system/process/process.h"
#include "system/timer/pit.h"
#include "lib/string.h"

extern lufirafs_t lufirafs;

static int append_str(char *dst, int pos, int cap, const char *s) {
    while (*s && pos < cap - 1) dst[pos++] = *s++;
    return pos;
}

// ifconfig [ip] [netmask] [gateway] — без аргументов показывает текущую
// настройку + MAC карты; с аргументами — меняет её (см. net_set_config()).
void command_ifconfig(const char *args) {
    const net_config_t *cfg = net_get_config();

    if (!args || !*args) {
        char ip_str[16], mask_str[16], gw_str[16], mac_str[18];
        ip_addr_to_str(cfg->our_ip, ip_str);
        ip_addr_to_str(cfg->netmask, mask_str);
        ip_addr_to_str(cfg->gateway, gw_str);

        printf("\n");
        if (rtl8139_found()) {
            mac_addr_to_str(rtl8139_get_mac(), mac_str);
            printf("rtl0: link up, mac %s\n", mac_str);
        } else {
            printf("rtl0: no network device found\n");
        }
        printf("  inet %s  netmask %s  gateway %s\n", ip_str, mask_str, gw_str);
        return;
    }

    const char *p = skip_spaces(args);
    uint32_t ip, mask, gw;

    if (ip_str_to_addr(p, &ip) != 0) {
        printf("\nUsage: ifconfig [ip] [netmask] [gateway]\n");
        return;
    }
    p = skip_spaces(p + token_length(p));
    if (*p == '\0' || ip_str_to_addr(p, &mask) != 0) {
        printf("\nUsage: ifconfig <ip> <netmask> <gateway>\n");
        return;
    }
    p = skip_spaces(p + token_length(p));
    if (*p == '\0' || ip_str_to_addr(p, &gw) != 0) {
        printf("\nUsage: ifconfig <ip> <netmask> <gateway>\n");
        return;
    }

    net_set_config(ip, mask, gw);
    printf("\nifconfig: updated\n");
}

// ping <ip> [count] — count Echo Request'ов (по умолчанию 4), печатает RTT
// каждого и итоговую статистику потерь.
void command_ping(const char *args) {
    if (!args || !*args) {
        printf("\nUsage: ping <ip> [count]\n");
        return;
    }
    if (!rtl8139_found()) {
        printf("\nping: no network device found\n");
        return;
    }

    const char *p = skip_spaces(args);
    uint32_t ip;
    if (ip_str_to_addr(p, &ip) != 0) {
        printf("\nping: invalid IP address\n");
        return;
    }
    p = skip_spaces(p + token_length(p));

    int count = 4;
    if (*p != '\0') {
        int n = atoi(p);
        if (n > 0) count = n;
    }

    char ip_str[16];
    ip_addr_to_str(ip, ip_str);
    printf("\nPING %s\n", ip_str);

    static uint16_t ping_id_counter = 0;
    uint16_t ping_id = ++ping_id_counter;

    int sent = 0, received = 0;
    for (int seq = 1; seq <= count; seq++) {
        sent++;
        if (icmp_ping_send(ip, ping_id, (uint16_t)seq) != 0) {
            printf("icmp_seq=%d: send failed\n", seq);
            continue;
        }
        uint32_t rtt_ms;
        if (icmp_ping_wait(1000, &rtt_ms) == 0) {
            received++;
            printf("icmp_seq=%d: time=%ums\n", seq, rtt_ms);
        } else {
            printf("icmp_seq=%d: timeout\n", seq);
        }
    }

    int loss_pct = sent > 0 ? ((sent - received) * 100) / sent : 0;
    printf("\n%d packets transmitted, %d received, %d%% packet loss\n", sent, received, loss_pct);
}

static int find_header_end(const uint8_t *buf, int len) {
    for (int i = 0; i + 4 <= len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') return i;
    }
    return -1;
}

// wget <ip> <path> [output-filename] — HTTP/1.0 GET через минимальный
// блокирующий TCP-клиент (kernel/net/tcp.c), тело ответа (без заголовков)
// пишется в файл по мере поступления. Аргументы — из СЫРОГО input_buffer
// (см. execute_command() в shell.c): путь/имя файла регистрозависимы.
void command_wget(const char *args) {
    if (!args || !*args) {
        printf("\nUsage: wget <ip> <path> [output-filename]\n");
        return;
    }
    if (!rtl8139_found()) {
        printf("\nwget: no network device found\n");
        return;
    }

    const char *p = skip_spaces(args);
    uint32_t ip;
    if (ip_str_to_addr(p, &ip) != 0) {
        printf("\nwget: invalid IP address\n");
        return;
    }
    p = skip_spaces(p + token_length(p));
    if (*p == '\0') {
        printf("\nUsage: wget <ip> <path> [output-filename]\n");
        return;
    }

    char path[256];
    int path_len = token_length(p);
    if (path_len >= (int)sizeof(path)) path_len = (int)sizeof(path) - 1;
    for (int i = 0; i < path_len; i++) path[i] = p[i];
    path[path_len] = '\0';
    p = skip_spaces(p + token_length(p));

    char out_name[64];
    if (*p != '\0') {
        int n = token_length(p);
        if (n >= (int)sizeof(out_name)) n = (int)sizeof(out_name) - 1;
        for (int i = 0; i < n; i++) out_name[i] = p[i];
        out_name[n] = '\0';
    } else {
        const char *last_slash = NULL;
        for (const char *s = path; *s; s++) if (*s == '/') last_slash = s;
        const char *leaf = (last_slash && *(last_slash + 1) != '\0') ? last_slash + 1 : "index.html";
        int n = 0;
        while (leaf[n] && n < (int)sizeof(out_name) - 1) { out_name[n] = leaf[n]; n++; }
        out_name[n] = '\0';
    }

    char ip_str[16];
    ip_addr_to_str(ip, ip_str);
    printf("\nConnecting to %s:80...\n", ip_str);

    if (tcp_connect(ip, 80) != 0) {
        printf("wget: connection failed\n");
        return;
    }

    char request[512];
    int rp = 0;
    rp = append_str(request, rp, sizeof(request), "GET ");
    rp = append_str(request, rp, sizeof(request), path);
    rp = append_str(request, rp, sizeof(request), " HTTP/1.0\r\nHost: ");
    rp = append_str(request, rp, sizeof(request), ip_str);
    rp = append_str(request, rp, sizeof(request), "\r\nConnection: close\r\n\r\n");

    if (tcp_send(request, (uint16_t)rp) != 0) {
        printf("wget: request send failed\n");
        tcp_close();
        return;
    }

    uint32_t ino;
    int exists = (lufirafs_lookup(&lufirafs, cwd_inode, out_name, &ino) == 0);
    if (!exists) {
        if (lufirafs_create(&lufirafs, cwd_inode, out_name, LUFIRAFS_MODE_FILE,
                             current_process->uid, current_process->gid,
                             LUFIRAFS_DEFAULT_FILE_PERM, &ino) != 0) {
            printf("wget: failed to create %s\n", out_name);
            tcp_close();
            return;
        }
    }
    lufirafs_truncate(&lufirafs, ino, 0);

    printf("Downloading to %s...\n", out_name);

    uint32_t offset = 0;
    int header_done = 0;
    uint8_t carry[3];
    int carry_len = 0;

    uint64_t deadline = pit_get_ticks() + 1000; // ~10с без новых данных = обрыв
    while (pit_get_ticks() < deadline) {
        uint8_t *chunk;
        uint16_t chunk_len;

        if (tcp_recv_poll(&chunk, &chunk_len)) {
            deadline = pit_get_ticks() + 1000;

            if (!header_done) {
                uint8_t combined[3 + 1460];
                int clen = 0;
                for (int i = 0; i < carry_len; i++) combined[clen++] = carry[i];
                for (int i = 0; i < chunk_len; i++) combined[clen++] = chunk[i];

                int idx = find_header_end(combined, clen);
                if (idx >= 0) {
                    header_done = 1;
                    int body_start = idx + 4;
                    int body_len = clen - body_start;
                    if (body_len > 0) {
                        lufirafs_write(&lufirafs, ino, offset, combined + body_start, (uint32_t)body_len);
                        offset += (uint32_t)body_len;
                    }
                } else {
                    carry_len = clen < 3 ? clen : 3;
                    for (int i = 0; i < carry_len; i++) carry[i] = combined[(uint32_t)clen - carry_len + i];
                }
            } else {
                lufirafs_write(&lufirafs, ino, offset, chunk, chunk_len);
                offset += chunk_len;
            }

            tcp_ack_consumed();
        } else if (tcp_is_remote_closed()) {
            break;
        } else {
            pit_wait_ms(1);
        }
    }

    lufirafs_sync(&lufirafs);
    tcp_close();

    printf("wget: saved %u bytes to %s\n", offset, out_name);
}
