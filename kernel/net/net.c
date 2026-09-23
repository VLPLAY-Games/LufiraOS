#include "net.h"
#include "drivers/net/rtl8139.h"
#include "system/klog/klog.h"

uint16_t net_checksum(const void *data, uint32_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t sum = 0;

    while (len > 1) {
        sum += (uint32_t)(((uint32_t)p[0] << 8) | p[1]);
        p += 2;
        len -= 2;
    }
    if (len == 1) {
        sum += (uint32_t)((uint32_t)p[0] << 8);
    }

    while (sum >> 16) sum = (sum & 0xFFFFu) + (sum >> 16);

    return (uint16_t)~sum;
}

int ip_str_to_addr(const char *s, uint32_t *out_ip) {
    uint32_t octets[4];
    const char *p = s;

    for (int i = 0; i < 4; i++) {
        if (*p < '0' || *p > '9') return -1;

        uint32_t val = 0;
        int digits = 0;
        while (*p >= '0' && *p <= '9') {
            val = val * 10 + (uint32_t)(*p - '0');
            p++;
            digits++;
            if (digits > 3 || val > 255) return -1;
        }
        octets[i] = val;

        if (i < 3) {
            if (*p != '.') return -1;
            p++;
        }
    }

    if (*p != '\0' && *p != ' ') return -1;

    *out_ip = (octets[0] << 24) | (octets[1] << 16) | (octets[2] << 8) | octets[3];
    return 0;
}

static void append_u8_dec(char **pp, uint8_t v) {
    char tmp[4];
    int n = 0;
    if (v == 0) {
        tmp[n++] = '0';
    } else {
        while (v) { tmp[n++] = (char)('0' + v % 10); v = (uint8_t)(v / 10); }
    }
    while (n > 0) *(*pp)++ = tmp[--n];
}

void ip_addr_to_str(uint32_t ip, char *out) {
    char *p = out;
    append_u8_dec(&p, (uint8_t)(ip >> 24));
    *p++ = '.';
    append_u8_dec(&p, (uint8_t)(ip >> 16));
    *p++ = '.';
    append_u8_dec(&p, (uint8_t)(ip >> 8));
    *p++ = '.';
    append_u8_dec(&p, (uint8_t)ip);
    *p = '\0';
}

static char hex_digit_lower(uint8_t v) {
    return (char)(v < 10 ? ('0' + v) : ('a' + (v - 10)));
}

void mac_addr_to_str(const uint8_t mac[6], char *out) {
    char *p = out;
    for (int i = 0; i < 6; i++) {
        *p++ = hex_digit_lower((uint8_t)(mac[i] >> 4));
        *p++ = hex_digit_lower((uint8_t)(mac[i] & 0xF));
        if (i < 5) *p++ = ':';
    }
    *p = '\0';
}

// Дефолт под подсеть QEMU user-mode (SLIRP) — см. net.h/план.
static net_config_t g_net_config = {
    .our_ip  = (10u << 24) | (0u << 16) | (2u << 8) | 15u,  // 10.0.2.15
    .netmask = (255u << 24) | (255u << 16) | (255u << 8) | 0u, // 255.255.255.0
    .gateway = (10u << 24) | (0u << 16) | (2u << 8) | 2u,   // 10.0.2.2
};

void net_init(void) {
    rtl8139_init();
    if (rtl8139_found()) {
        klog("[NET] ready, ip=10.0.2.15/24 gw=10.0.2.2");
    }
}

void net_poll(void) {
    if (rtl8139_found()) rtl8139_poll();
}

void net_set_config(uint32_t ip, uint32_t netmask, uint32_t gateway) {
    g_net_config.our_ip = ip;
    g_net_config.netmask = netmask;
    g_net_config.gateway = gateway;
}

const net_config_t *net_get_config(void) {
    return &g_net_config;
}
