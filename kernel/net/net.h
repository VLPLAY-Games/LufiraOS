#pragma once

#include "lib/types.h"

/*
 * Общие типы/утилиты сетевого стека (kernel/net/) и точка входа
 * (net_init()/net_poll()). Стек — Ethernet/ARP/IPv4/ICMP/TCP поверх
 * kernel/drivers/net/rtl8139.c, без DNS и без UDP (см. план: wget работает
 * только с IP-адресами, IP настраивается статически через ifconfig).
 *
 * IP-адреса везде в этом стеке хранятся как uint32_t В ХОСТОВОМ порядке
 * байт (то есть a.b.c.d == (a<<24)|(b<<16)|(c<<8)|d) — сеть/провода видят
 * только через htonl()/ntohl() на границе сборки/разбора заголовков.
 */

static inline uint16_t htons(uint16_t v) { return (uint16_t)((v << 8) | (v >> 8)); }
static inline uint16_t ntohs(uint16_t v) { return htons(v); }
static inline uint32_t htonl(uint32_t v) {
    return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
           ((v & 0x00FF0000u) >> 8)  | ((v & 0xFF000000u) >> 24);
}
static inline uint32_t ntohl(uint32_t v) { return htonl(v); }

// Классический интернет-чексум (16-бит одно-дополнительная сумма,
// проинвертированная). Считает БОЛЬШИМ порядком байт внутри пары — перед
// записью результата в поле заголовка нужен htons() (см. использование в
// ip.c/icmp.c/tcp.c).
uint16_t net_checksum(const void *data, uint32_t len);

// "a.b.c.d" -> host-order uint32_t. 0 = успех, -1 = неверный формат.
// Останавливается на конце строки ИЛИ первом пробеле (вызывающая сторона
// часто передаёт остаток строки команды целиком).
int ip_str_to_addr(const char *s, uint32_t *out_ip);

// out должен вмещать минимум 16 байт ("255.255.255.255\0").
void ip_addr_to_str(uint32_t ip, char *out);

// out должен вмещать минимум 18 байт ("xx:xx:xx:xx:xx:xx\0").
void mac_addr_to_str(const uint8_t mac[6], char *out);

typedef struct {
    uint32_t our_ip;
    uint32_t netmask;
    uint32_t gateway;
} net_config_t;

// Инициализация: поднимает RTL8139 (см. rtl8139.c), при успехе — статический
// дефолт под подсеть QEMU user-mode (SLIRP): 10.0.2.15/255.255.255.0,
// шлюз 10.0.2.2 (см. план). Безопасно вызывать, даже если карта не найдена.
void net_init(void);

// Вызывается раз в тик PIT из timer_irq_handler() (pit.c), сразу после
// usb_poll(). Опрашивает NIC и прогоняет пришедшие кадры через весь стек
// (eth_receive() -> arp/ip -> icmp/tcp).
void net_poll(void);

void net_set_config(uint32_t ip, uint32_t netmask, uint32_t gateway);
const net_config_t *net_get_config(void);
