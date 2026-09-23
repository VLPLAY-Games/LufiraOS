#include "rtl8139.h"
#include "net/eth.h"

#include "drivers/pci/pci.h"
#include "system/mm/pmm.h"
#include "system/mm/paging.h"
#include "system/timer/pit.h"
#include "drivers/console/console.h"
#include "lib/string.h"
#include "system/devmode/devmode.h"
#include "system/klog/klog.h"

/* ======================================================================== */
/* Порт I/O (как и в pci.c/ac97.c — свой локальный набор, не общий)         */
/* ======================================================================== */

static inline uint8_t rtl_in8(uint16_t port) {
    uint8_t v; __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static inline uint16_t rtl_in16(uint16_t port) {
    uint16_t v; __asm__ volatile ("inw %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static inline uint32_t rtl_in32(uint16_t port) {
    uint32_t v; __asm__ volatile ("inl %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static inline void rtl_out8(uint16_t port, uint8_t v) {
    __asm__ volatile ("outb %0, %1" : : "a"(v), "Nd"(port));
}
static inline void rtl_out16(uint16_t port, uint16_t v) {
    __asm__ volatile ("outw %0, %1" : : "a"(v), "Nd"(port));
}
static inline void rtl_out32(uint16_t port, uint32_t v) {
    __asm__ volatile ("outl %0, %1" : : "a"(v), "Nd"(port));
}

/* ======================================================================== */
/* Регистры (смещения от base I/O)                                          */
/* ======================================================================== */

#define RTL_REG_MAC0      0x00 /* IDR0..IDR5, 6 байт */
#define RTL_REG_TSD0      0x10 /* +4*n, n=0..3, u32 */
#define RTL_REG_TSAD0     0x20 /* +4*n, n=0..3, u32 */
#define RTL_REG_RBSTART   0x30 /* u32 */
#define RTL_REG_CR        0x37 /* u8  */
#define RTL_REG_CAPR      0x38 /* u16 */
#define RTL_REG_IMR       0x3C /* u16 */
#define RTL_REG_ISR       0x3E /* u16 */
#define RTL_REG_TCR       0x40 /* u32 */
#define RTL_REG_RCR       0x44 /* u32 */
#define RTL_REG_CONFIG1   0x52 /* u8  */

#define RTL_CR_BUFE  (1u << 0)
#define RTL_CR_TE    (1u << 2)
#define RTL_CR_RE    (1u << 3)
#define RTL_CR_RST   (1u << 4)

#define RTL_ISR_ROK  (1u << 0)
#define RTL_ISR_RER  (1u << 1)
#define RTL_ISR_TOK  (1u << 2)
#define RTL_ISR_TER  (1u << 3)

#define RTL_RCR_AAP        (1u << 0) // Accept All Packets (promiscuous) — не используем
#define RTL_RCR_APM        (1u << 1) // Accept Physical Match (наш MAC)
#define RTL_RCR_AB         (1u << 3) // Accept Broadcast
#define RTL_RCR_WRAP       (1u << 7) // разрешить записи, пересекающие конец кольца,
                                       // в запасные байты после него — без этого
                                       // пришлось бы вручную разбирать заголовок
                                       // пакета, разорванный ровно на границе кольца
#define RTL_RCR_MXDMA_UNLIMITED (7u << 8)
#define RTL_RCR_RBLEN_8K   (0u << 11)
#define RTL_RCR_RXFTH_NONE (7u << 13) // не передавать в хост, пока не пришёл пакет целиком

#define RTL_TCR_MXDMA_UNLIMITED (7u << 8)
#define RTL_TCR_IFG_STANDARD    (3u << 24)

// Заголовок каждого принятого кадра внутри кольцевого буфера: 2 байта
// статус + 2 байта длина (включая 4-байтный CRC в хвосте).
#define RTL_RX_STATUS_ROK (1u << 0)

// TSD (Transmit Status Descriptor): биты 0-12 — размер кадра (запись сюда
// запускает передачу), бит 15 — TOK (успешно передан), бит 14 — TUN
// (опустошение FIFO), бит 31 — TABT (передача прервана).
#define RTL_TSD_TOK  (1u << 15)
#define RTL_TSD_TUN  (1u << 14)
#define RTL_TSD_TABT (1u << 31)

/* ======================================================================== */
/* Состояние драйвера                                                       */
/* ======================================================================== */

// 8192 (RBLEN=8K) + 16 (запас по спецификации на аппаратный prefetch) +
// 1500 (запас под WRAP — см. RTL_RCR_WRAP выше) округлено вверх до страниц.
#define RTL_RX_BUF_LOGICAL_SIZE 8192u
#define RTL_RX_BUF_PHYS_SIZE    (8192u + 16u + 1500u)
#define RTL_RX_BUF_PAGES        3 // ceil(9708/4096)

#define RTL_TX_SLOTS       4
#define RTL_TX_SLOT_BYTES  4096u // одна страница на слот, хватает на любой Ethernet-кадр

// rtl8139_poll()/rtl8139_send() вызываются и из timer_irq_handler() (через
// net_poll(), IF=0 внутри ISR), и синхронно из обычного кода с IF=1
// (arp_resolve()/tcp_recv_poll() крутят собственный busy-wait, вызывая
// rtl8139_poll() напрямую) — таймерный тик может прервать такой синхронный
// вызов ровно посреди работы с общим состоянием (rtl_rx_offset/rtl_tx_next)
// и вызвать те же функции повторно. Та же проблема и то же решение
// (сохранить/cli/восстановить flags), что уже задокументировано в
// pmm_lock()/pmm_unlock() (kernel/system/mm/pmm.c) — "наблюдалось на
// практике" там относится к ровно такому классу гонки.
static inline uint64_t rtl_cli_save(void) {
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0" : "=r"(flags) :: "memory");
    __asm__ volatile ("cli");
    return flags;
}
static inline void rtl_sti_restore(uint64_t flags) {
    __asm__ volatile ("push %0; popfq" : : "r"(flags) : "memory", "cc");
}

// rtl8139_poll() не может просто держать cli на всё время своей работы:
// eth_receive() внутри неё может дойти до ARP/ICMP-автоответа, который
// зовёт rtl8139_send(), а тот блокирующе ждёт TOK через pit_wait_ms() —
// это требует, чтобы таймерные тики продолжали идти (IF=1), иначе hlt
// внутри pit_wait_ms() зависнет навсегда. Поэтому вместо блокировки данных
// на время всей функции — простой guard "уже выполняюсь, выйти" вокруг
// ЕДИНСТВЕННОГО ресурса, который не может быть корректно обработан дважды
// параллельно (общий курсор rtl_rx_offset/CAPR, в отличие от TX, где у
// каждого слота свои независимые регистры TSAD/TSD): если вложенный
// таймерный тик застаёт уже идущий синхронный опрос, он просто ничего не
// делает в этот раз — внешний (уже выполняющийся) вызов сам разберёт всё,
// что успело прийти, на следующей итерации своего цикла.
static volatile int rtl_poll_busy = 0;

static int      rtl_found = 0;
static uint16_t rtl_io_base = 0;
static uint8_t  rtl_mac[6];

static uint64_t rtl_rx_buf_phys = 0;
static uint8_t *rtl_rx_buf_virt = NULL;
static uint32_t rtl_rx_offset = 0;

static uint64_t rtl_tx_buf_phys[RTL_TX_SLOTS];
static uint8_t *rtl_tx_buf_virt[RTL_TX_SLOTS];
static int      rtl_tx_next = 0;

#define RTL_POLL_MAX_PACKETS_PER_TICK 8

/* ======================================================================== */
/* PCI-обнаружение                                                          */
/* ======================================================================== */

static int rtl8139_find_controller(void) {
    const pci_device_t *dev = pci_find_device(0x10EC, 0x8139);
    if (!dev) {
        DLOG("[RTL8139] Controller not found\n");
        return 0;
    }

    DLOG("[RTL8139] Found at %u:%u.%u\n", dev->bus, dev->device, dev->function);

    pci_bar_t bar0;
    if (pci_get_bar(dev, 0, &bar0) != 0 || !bar0.is_io) {
        printf("[RTL8139] Expected I/O BAR0\n");
        return 0;
    }
    if (bar0.address > 0xFFFF) {
        printf("[RTL8139] Invalid I/O BAR address\n");
        return 0;
    }

    rtl_io_base = (uint16_t)bar0.address;
    DLOG("[RTL8139] I/O base = %04X\n", rtl_io_base);

    pci_enable_io(dev);
    pci_enable_bus_master(dev);
    pci_disable_interrupts(dev); // опрашиваем ISR сами из net_poll(), см. заголовок файла

    return 1;
}

/* ======================================================================== */
/* Сброс и настройка                                                        */
/* ======================================================================== */

static int rtl8139_reset(void) {
    // Пробуждение устройства из режима энергосбережения (стандартный шаг
    // перед сбросом у этой карты).
    rtl_out8((uint16_t)(rtl_io_base + RTL_REG_CONFIG1), 0x00);

    rtl_out8((uint16_t)(rtl_io_base + RTL_REG_CR), RTL_CR_RST);

    for (int i = 0; i < 1000; i++) {
        if (!(rtl_in8((uint16_t)(rtl_io_base + RTL_REG_CR)) & RTL_CR_RST)) {
            DLOG("[RTL8139] Reset complete\n");
            return 1;
        }
        pit_wait_ms(1);
    }

    printf("[RTL8139] Reset timed out\n");
    return 0;
}

static int rtl8139_alloc_buffers(void) {
    rtl_rx_buf_phys = pmm_alloc_contiguous_pages(RTL_RX_BUF_PAGES);
    if (rtl_rx_buf_phys == 0) {
        printf("[RTL8139] Failed to allocate contiguous RX buffer\n");
        return 0;
    }
    rtl_rx_buf_virt = (uint8_t *)phys_to_virt(rtl_rx_buf_phys);
    memset(rtl_rx_buf_virt, 0, (size_t)RTL_RX_BUF_PAGES * PAGE_SIZE);

    for (int i = 0; i < RTL_TX_SLOTS; i++) {
        rtl_tx_buf_phys[i] = pmm_alloc_page();
        if (rtl_tx_buf_phys[i] == 0) {
            printf("[RTL8139] Failed to allocate TX buffer %d\n", i);
            return 0;
        }
        rtl_tx_buf_virt[i] = (uint8_t *)phys_to_virt(rtl_tx_buf_phys[i]);
    }

    return 1;
}

static void rtl8139_configure(void) {
    rtl_out32((uint16_t)(rtl_io_base + RTL_REG_RBSTART), (uint32_t)rtl_rx_buf_phys);

    // IMR остаётся 0 — реальная линия INTx никогда не взводится (см.
    // заголовок файла), но биты ISR всё равно защёлкиваются аппаратно и
    // читаются из rtl8139_poll().
    rtl_out16((uint16_t)(rtl_io_base + RTL_REG_IMR), 0x0000);

    rtl_out32((uint16_t)(rtl_io_base + RTL_REG_TCR),
              RTL_TCR_MXDMA_UNLIMITED | RTL_TCR_IFG_STANDARD);

    rtl_out32((uint16_t)(rtl_io_base + RTL_REG_RCR),
              RTL_RCR_APM | RTL_RCR_AB | RTL_RCR_WRAP |
              RTL_RCR_MXDMA_UNLIMITED | RTL_RCR_RBLEN_8K | RTL_RCR_RXFTH_NONE);

    rtl_out8((uint16_t)(rtl_io_base + RTL_REG_CR), RTL_CR_RE | RTL_CR_TE);

    // CAPR аппаратно всегда хранится СМЕЩЁННЫМ на -16 от настоящей позиции
    // чтения (тот же "-16", что и в rtl8139_poll() на каждый разобранный
    // пакет) — это касается и самого первого значения при старте, не только
    // обновлений после первого пакета. Записав сюда голый 0 вместо 0-16
    // (0xFFF0), карта никогда не сочтёт кольцо пустым (бит BUFE в CR не
    // взводится), и rtl8139_poll() будет читать нулевой мусор из ещё
    // нетронутого буфера на каждом тике — именно так и проявлялось при
    // живом тестировании в QEMU.
    rtl_out16((uint16_t)(rtl_io_base + RTL_REG_CAPR), (uint16_t)(0u - 16u));
    rtl_rx_offset = 0;

    for (int i = 0; i < 6; i++) {
        rtl_mac[i] = rtl_in8((uint16_t)(rtl_io_base + RTL_REG_MAC0 + i));
    }
}

/* ======================================================================== */
/* Публичное API                                                            */
/* ======================================================================== */

void rtl8139_init(void) {
    if (!rtl8139_find_controller()) return;
    if (!rtl8139_reset()) return;
    if (!rtl8139_alloc_buffers()) return;
    rtl8139_configure();

    rtl_found = 1;

    DLOG("[RTL8139] MAC = %x:%x:%x:%x:%x:%x\n",
         rtl_mac[0], rtl_mac[1], rtl_mac[2], rtl_mac[3], rtl_mac[4], rtl_mac[5]);
    klog("[RTL8139] controller ready");
}

int rtl8139_found(void) {
    return rtl_found;
}

const uint8_t *rtl8139_get_mac(void) {
    return rtl_mac;
}

int rtl8139_send(const void *frame, uint16_t len) {
    if (!rtl_found || !frame || len == 0) return -1;
    if (len > RTL_TX_SLOT_BYTES) return -1;

    uint64_t slot_flags = rtl_cli_save();
    int slot = rtl_tx_next;
    rtl_tx_next = (rtl_tx_next + 1) % RTL_TX_SLOTS;
    rtl_sti_restore(slot_flags);

    memcpy(rtl_tx_buf_virt[slot], frame, len);

    uint16_t tsad_port = (uint16_t)(rtl_io_base + RTL_REG_TSAD0 + slot * 4);
    uint16_t tsd_port  = (uint16_t)(rtl_io_base + RTL_REG_TSD0 + slot * 4);

    rtl_out32(tsad_port, (uint32_t)rtl_tx_buf_phys[slot]);
    // Минимальный кадр Ethernet — 60 байт данных (+4 CRC, который считает сама карта).
    uint32_t tx_len = len < 60 ? 60 : len;
    rtl_out32(tsd_port, tx_len);

    for (int i = 0; i < 200; i++) {
        uint32_t status = rtl_in32(tsd_port);
        if (status & (RTL_TSD_TOK | RTL_TSD_TUN | RTL_TSD_TABT)) {
            if (status & RTL_TSD_TOK) return 0;
            DLOG("[RTL8139] TX error, TSD=%x\n", status);
            return -1;
        }
        pit_wait_ms(1);
    }

    DLOG("[RTL8139] TX timed out\n");
    return -1;
}

void rtl8139_poll(void) {
    if (!rtl_found) return;

    uint64_t busy_flags = rtl_cli_save();
    if (rtl_poll_busy) { rtl_sti_restore(busy_flags); return; }
    rtl_poll_busy = 1;
    rtl_sti_restore(busy_flags);

    for (int i = 0; i < RTL_POLL_MAX_PACKETS_PER_TICK; i++) {
        if (rtl_in8((uint16_t)(rtl_io_base + RTL_REG_CR)) & RTL_CR_BUFE) break;

        uint8_t *hdr = rtl_rx_buf_virt + rtl_rx_offset;
        uint16_t status = *(uint16_t *)hdr;
        uint16_t frame_len = *(uint16_t *)(hdr + 2);

        if (!(status & RTL_RX_STATUS_ROK) || frame_len < 4 || frame_len > 1600) {
            // Рассинхронизация кольца — редкий случай на практике для
            // этого простого драйвера, дальше не разбираем, просто
            // останавливаемся до следующего тика (документированное
            // упрощение: полноценный recovery через переинициализацию RX
            // здесь не реализован).
            DLOG("[RTL8139] bad RX header, status=%x len=%u\n", status, frame_len);
            break;
        }

        uint16_t payload_len = (uint16_t)(frame_len - 4); // без хвостового CRC
        eth_receive(hdr + 4, payload_len);

        uint32_t advance = (uint32_t)((frame_len + 4 + 3) & ~3u);
        rtl_rx_offset = (rtl_rx_offset + advance) % RTL_RX_BUF_LOGICAL_SIZE;

        rtl_out16((uint16_t)(rtl_io_base + RTL_REG_CAPR),
                   (uint16_t)(rtl_rx_offset - 16));
    }

    // Подтверждаем всё, что успело защёлкнуться в ISR (ROK/TOK/ошибки) —
    // IMR=0, так что это чисто программное подтверждение статусных битов,
    // ни на что аппаратно не влияющее (см. заголовок файла).
    uint16_t isr = rtl_in16((uint16_t)(rtl_io_base + RTL_REG_ISR));
    if (isr) rtl_out16((uint16_t)(rtl_io_base + RTL_REG_ISR), isr);

    busy_flags = rtl_cli_save();
    rtl_poll_busy = 0;
    rtl_sti_restore(busy_flags);
}
