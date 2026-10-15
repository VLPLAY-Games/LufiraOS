#pragma once

#include "lib/types.h"

/*
 * Общие типы/утилиты сетевого стека (kernel/net/) и точка входа
 * (net_init()/net_poll()). Стек: Ethernet/ARP/IPv4/ICMP/TCP/UDP/DNS/TLS
 * поверх ОДНОЙ из kernel/drivers/net/{rtl8139,e1000,e1000e}.c — см.
 * nic_driver_t ниже: net_init() пробует их по очереди и использует первую
 * найденную карту, остальной стек (eth.c и выше) больше не знает, какая
 * именно это карта, и обращается к ней только через net_found()/
 * net_get_mac()/net_send_frame()/net_poll().
 *
 * IP-адреса везде в этом стеке хранятся как uint32_t В ХОСТОВОМ порядке
 * байт (a.b.c.d == (a<<24)|(b<<16)|(c<<8)|d) — сеть видит их только через
 * htonl()/ntohl() на границе сборки/разбора заголовков.
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
    // Резолвер для dns_resolve() (dns.c) — нужен dlpg sync/upgrade для
    // обращения по имени хоста, а не только по IP. Дефолт — встроенный
    // DNS-форвардер QEMU SLIRP (10.0.2.3, см. net.c), заменяется DHCP при
    // успехе (см. dhcp.c) — DHCP-сервер обычно и есть DNS-резолвер.
    uint32_t dns_server;
} net_config_t;

// Единый интерфейс драйвера карты — см. net_init() ниже. Каждый драйвер
// (rtl8139.c/e1000.c/e1000e.c) предоставляет ровно такой набор функций под
// своими собственными именами (rtl8139_init()/rtl8139_found()/... и т.д.);
// эта структура — просто таблица указателей на них, не отдельный ABI.
typedef struct {
    const char *name;
    void (*init)(void);
    int (*found)(void);
    const uint8_t *(*get_mac)(void);
    int (*send)(const void *frame, uint16_t len);
    void (*poll)(void);
} nic_driver_t;

// Инициализация: пробует RTL8139/E1000/E1000E по очереди (см. nic_driver_t),
// использует первую найденную карту; при успехе — статический дефолт под
// подсеть QEMU user-mode (SLIRP): 10.0.2.15/255.255.255.0, шлюз 10.0.2.2,
// который dhcp_start() (см. kernel.c) пытается тут же заменить настоящей
// арендой. Безопасно вызывать, даже если ни одна карта не найдена —
// net_found() после этого просто вернёт 0.
void net_init(void);

int net_found(void);

// 6-байтный MAC активной карты (валиден только при net_found()).
const uint8_t *net_get_mac(void);

// Отправить готовый Ethernet-кадр через активную карту — см. eth_send().
int net_send_frame(const void *frame, uint16_t len);

// Вызывается раз в тик PIT из timer_irq_handler() (pit.c), сразу после
// usb_poll(). Опрашивает активную NIC и прогоняет пришедшие кадры через
// весь стек (eth_receive() -> arp/ip -> icmp/tcp).
void net_poll(void);

void net_set_config(uint32_t ip, uint32_t netmask, uint32_t gateway, uint32_t dns_server);
const net_config_t *net_get_config(void);

// НАЙДЕННЫЙ БАГ: SYS_NET_FETCH (dlpg sync/upgrade) держал процессор ВСЕЙ
// системы — не только свой собственный процесс — на всё время скачивания:
// каждый блокирующий цикл стека (tcp_connect()/tcp_send()/dns_resolve()/
// tls.c/http_client.c) был устроен как "опросить -> pit_wait_ms(1)", а
// pit_wait_ms() просто делает hlt до следующего тика В ТОМ ЖЕ процессе —
// schedule() никогда не вызывается, так что ни один другой READY-процесс
// (включая сам shell) не получал ни одного кванта времени, пока fetch не
// закончится. Исправлено заменой pit_wait_ms(1) на process_sleep(1) во
// всех этих циклах (tcp.c/dns.c/tls.c/http_client.c/arp.c/icmp.c) —
// process_sleep() уже делает и ожидание тика, и schedule() за один вызов
// (см. process.c), так что другие процессы теперь крутятся параллельно с
// сетевым ожиданием.
//
// Побочный эффект той же починки: теперь МЕЖДУ "опросить" и "ждать" любого
// из этих циклов другой процесс может успеть вклиниться и запустить СВОЙ
// сетевой вызов — а весь стек выше (g_conn в tcp.c, g_ping в icmp.c, ...)
// рассчитан ровно на одно соединение разом, без какой-либо собственной
// защиты от параллельного использования. net_lock_acquire()/
// net_lock_release() — простая рекурсивно-безопасная (не настоящий мьютекс,
// он тут и не нужен — переключение контекста в этом ядре происходит только
// в явных точках вроде process_sleep(), не посреди произвольной инструкции)
// сериализация: второй вызывающий ждёт (тем же process_sleep()), а не
// порождает гонку за g_conn/g_ping. Берётся на самом верхнем уровне каждого
// независимого входа в сетевой стек (sys_net_fetch() в syscall.c) — так
// держится дольше необходимого самому TCP, зато гарантированно накрывает
// ВСЮ последовательность DNS+TCP+TLS+HTTP одного fetch.
int net_lock_acquire(uint32_t timeout_ms);
void net_lock_release(void);
