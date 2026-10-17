#include "hda.h"
#include "drivers/pci/pci.h"
#include "system/mm/pmm.h"
#include "system/mm/paging.h"
#include "system/timer/pit.h"
#include "drivers/console/console.h"
#include "lib/string.h"
#include "lib/stddef.h"
#include "system/klog/klog.h"
#include "system/devmode/devmode.h"

// Intel HDA-драйвер: контроллер — MMIO (BAR0), опрашиваем сами (ни PCI IRQ,
// ни MSI-X). Два независимых опрашиваемых пути: команды кодеку (CORB —
// Command Output Ring Buffer, RIRB — Response Input Ring Buffer; только во
// время hda_init() для обхода топологии и настройки виджетов) и сам поток
// аудиоданных (Stream Descriptor 0, первый выходной поток; опрашиваем
// SDnSTS.BCIS — Buffer Completion Interrupt Status).
//
// Топология кодека не фиксирована (в отличие от AC'97 с известным набором
// NAM-регистров): HDA-кодек — дерево виджетов (Pin Complex, ЦАП/Audio
// Output, Mixer, Selector, ...), обходимое verb-командами Get Parameter/
// Get Connection List Entry. Эта версия понимает только: первый найденный
// ЦАП (Audio Output widget) + первый реально подключённый выходной Pin
// Complex (Port Connectivity != "нет соединения", Default Device = Line
// Out/Speaker/HP Out), соединённые напрямую либо через один промежуточный
// узел (Mixer/Selector). Более сложная топология не поддержана: hda_init()
// возвращает -1, а не разводит звук наугад не в тот Pin.

#define HDA_CLASS    0x04
#define HDA_SUBCLASS 0x03

/* ===== Регистры контроллера — смещения от MMIO BAR0 ===== */
#define HDA_REG_GCAP      0x00 // u16
#define HDA_REG_GCTL      0x08 // u32
#define HDA_REG_STATESTS  0x0E // u16
#define HDA_REG_CORBLBASE 0x40 // u32
#define HDA_REG_CORBUBASE 0x44 // u32
#define HDA_REG_CORBWP    0x48 // u16
#define HDA_REG_CORBRP    0x4A // u16
#define HDA_REG_CORBCTL   0x4C // u8
#define HDA_REG_CORBSIZE  0x4E // u8
#define HDA_REG_RIRBLBASE 0x50 // u32
#define HDA_REG_RIRBUBASE 0x54 // u32
#define HDA_REG_RIRBWP    0x58 // u16
#define HDA_REG_RINTCNT   0x5A // u16
#define HDA_REG_RIRBCTL   0x5C // u8
#define HDA_REG_RIRBSTS   0x5D // u8, RW1C
#define HDA_REG_RIRBSIZE  0x5E // u8
#define HDA_REG_SD0_BASE  0x80 // первый поток — сразу за GCAP.ISS входными

#define HDA_GCAP_OSS(v)  (((v) >> 12) & 0xFu)
#define HDA_GCAP_ISS(v)  (((v) >> 8) & 0xFu)

#define HDA_GCTL_CRST (1u << 0)

#define HDA_CORBCTL_RUN   (1u << 1)
#define HDA_RIRBCTL_DMAEN (1u << 1)
// RINTCTL — бит 0 RIRBCTL. Переход RINTFL (RIRBSTS) 1->0 при write-1-to-
// clear сбрасывает внутренний счётчик "ответов с последнего сброса"
// контроллера — без этого бита RINTFL никогда не взводится, переход не
// происходит, и по достижении RINTCNT ответов (см. HDA_REG_RINTCNT ниже)
// контроллер навсегда прекращает забирать verb'ы из CORB. Не связано с
// pci_disable_interrupts() выше — тот управляет доставкой PCI INTx до CPU,
// это — внутренняя бухгалтерия контроллера, обходимая поллингом в любом случае.
#define HDA_RIRBCTL_IRQEN (1u << 0)

#define HDA_SDCTL_SRST (1u << 0)
#define HDA_SDCTL_RUN  (1u << 1)
#define HDA_SDCTL_STRM_SHIFT 20

#define HDA_SDSTS_BCIS (1u << 2) // байт по смещению +0x03 внутри блока потока

/* ===== Stream Descriptor — смещения ВНУТРИ блока (0x20 байт на поток) ===== */
#define HDA_SD_CTL   0x00 // 3 байта (SRST/RUN/... в младшем байте, STRM в старшем)
#define HDA_SD_STS   0x03 // 1 байт, RW1C
#define HDA_SD_LPIB  0x04
#define HDA_SD_CBL   0x08
#define HDA_SD_LVI   0x0C // 2 байта
#define HDA_SD_FIFOS 0x10 // 2 байта
#define HDA_SD_FMT   0x12 // 2 байта
#define HDA_SD_BDPL  0x18
#define HDA_SD_BDPU  0x1C

static inline uint8_t  reg_read8(void *base, uint32_t off)  { return *(volatile uint8_t  *)((uint8_t *)base + off); }
static inline uint16_t reg_read16(void *base, uint32_t off) { return *(volatile uint16_t *)((uint8_t *)base + off); }
static inline uint32_t reg_read32(void *base, uint32_t off) { return *(volatile uint32_t *)((uint8_t *)base + off); }
static inline void reg_write8(void *base, uint32_t off, uint8_t val)   { *(volatile uint8_t  *)((uint8_t *)base + off) = val; }
static inline void reg_write16(void *base, uint32_t off, uint16_t val) { *(volatile uint16_t *)((uint8_t *)base + off) = val; }
static inline void reg_write32(void *base, uint32_t off, uint32_t val) { *(volatile uint32_t *)((uint8_t *)base + off) = val; }

/* ======================================================================== */
/* CORB/RIRB — verb-команды кодеку                                          */
/* ======================================================================== */

typedef struct __attribute__((packed)) {
    uint32_t resp;
    uint32_t resp_ex; // биты 3:0 = SDATA_IN (адрес кодека), бит 4 = unsolicited
} hda_rirb_entry_t;

static void *g_mmio = NULL;
static int   g_present = 0; // контроллер найден и поднят (CORB/RIRB живы) — не то же самое, что g_ready (есть рабочий ЦАП+Pin)
static int   g_ready = 0;

static uint32_t *g_corb_virt = NULL; static uint64_t g_corb_phys = 0; static uint16_t g_corb_entries = 0;
static hda_rirb_entry_t *g_rirb_virt = NULL; static uint64_t g_rirb_phys = 0; static uint16_t g_rirb_entries = 0;
static uint16_t g_rirb_read_idx = 0;

static uint8_t g_codec_addr = 0;
static uint8_t g_afg_nid = 0;
static uint8_t g_dac_nid = 0, g_pin_nid = 0;
static int     g_pin_via_node = -1; // промежуточный Mixer/Selector между Pin и ЦАП, -1 если прямое соединение

// NumSteps+1 выходного усилителя (для громкости), 0 = у узла нет реального
// Output Amp — в этом случае verb на этот узел не посылается вовсе
// (см. hda_apply_amp()).
static uint32_t g_dac_amp_steps = 0, g_pin_amp_steps = 0;

// pit_wait_ms() требует уже тикающих таймерных прерываний (sti вызван) —
// hlt с IF=0 никогда не проснётся от таймерного IRQ. Поэтому hda_init()
// должен вызываться после sti/irq_enable() (см. kernel.c). Голый CPU-spin
// с фиксированным числом итераций не годится: под TCG (без аппаратного
// ускорения) каждый MMIO-доступ стоит на порядки дороже, и калибровка под
// реальное железо превращается в минуты под эмуляцией. pit_wait_ms() даёт
// время по реальным тикам PIT, не зависящее от скорости хоста.
#define HDA_VERB_TIMEOUT_MS 100

// Шлёт один verb в CORB (слот (wp+1) % entries — слот 0 намеренно никогда
// не используется, тот же приём, что у любого кольца с "last written index"
// семантикой в этом драйвере/ядре, см. xhci.c) и ждёт новый ответ в RIRB
// (software-tracked g_rirb_read_idx сверяется с RIRBWP — тот же паттерн,
// что использует настоящий Linux-драйвер snd-hda-intel). 0 — успех,
// *out_resp заполнен; -1 — таймаут.
static int hda_corb_send_raw(uint32_t verb, uint32_t *out_resp) {
    uint16_t wp = (uint16_t)(reg_read16(g_mmio, HDA_REG_CORBWP) & 0xFFu);
    uint16_t new_wp = (uint16_t)((wp + 1) % g_corb_entries);
    g_corb_virt[new_wp] = verb;
    reg_write16(g_mmio, HDA_REG_CORBWP, new_wp);

    for (int ms = 0; ms <= HDA_VERB_TIMEOUT_MS; ms++) {
        uint16_t rirb_wp = (uint16_t)(reg_read16(g_mmio, HDA_REG_RIRBWP) & 0xFFu);
        if (rirb_wp != g_rirb_read_idx) {
            g_rirb_read_idx = (uint16_t)((g_rirb_read_idx + 1) % g_rirb_entries);
            if (out_resp) *out_resp = g_rirb_virt[g_rirb_read_idx].resp;
            // Подтверждаем RINTFL (W1C) на каждый ответ — не копим
            // протухший статус и сбрасываем счётчик ответов контроллера
            // (см. HDA_RIRBCTL_IRQEN выше).
            reg_write8(g_mmio, HDA_REG_RIRBSTS, reg_read8(g_mmio, HDA_REG_RIRBSTS));
            return 0;
        }
        pit_wait_ms(1);
    }
    printf("[HDA] verb timed out (verb=%u) CORBRP=%u CORBWP=%u RIRBWP=%u CORBCTL=%u RIRBCTL=%u\n",
           verb, reg_read16(g_mmio, HDA_REG_CORBRP), reg_read16(g_mmio, HDA_REG_CORBWP),
           reg_read16(g_mmio, HDA_REG_RIRBWP), reg_read8(g_mmio, HDA_REG_CORBCTL), reg_read8(g_mmio, HDA_REG_RIRBCTL));
    return -1;
}

// Короткая форма (12-битный verb, 8-битный payload) — подавляющее
// большинство команд (Get Parameter, Get/Set Power State, Set Converter
// Stream/Channel, Set Pin Widget Control, Get Configuration Default, ...).
static int hda_verb(uint8_t nid, uint16_t verb12, uint8_t payload8, uint32_t *out_resp) {
    uint32_t w = ((uint32_t)g_codec_addr << 28) | ((uint32_t)nid << 20)
               | ((uint32_t)verb12 << 8) | payload8;
    return hda_corb_send_raw(w, out_resp);
}

// Длинная форма (4-битный verb, 16-битный payload) — Set/Get Amplifier
// Gain/Mute (0xB/0x3) и Set/Get Converter Format (0xA/0x2), единственные
// две, которые этому драйверу нужны.
static int hda_verb_long(uint8_t nid, uint8_t verb4, uint16_t payload16, uint32_t *out_resp) {
    uint32_t w = ((uint32_t)g_codec_addr << 28) | ((uint32_t)nid << 20)
               | ((uint32_t)verb4 << 16) | payload16;
    return hda_corb_send_raw(w, out_resp);
}

#define HDA_VERB_GET_PARAMETER   0xF00
#define HDA_VERB_GET_CONN_LIST   0xF02
#define HDA_VERB_SET_CONN_SELECT 0x701
#define HDA_VERB_SET_POWER_STATE 0x705
#define HDA_VERB_SET_STREAM_CHAN 0x706
#define HDA_VERB_SET_PIN_CTL     0x707
#define HDA_VERB_GET_CONFIG_DEF  0xF1C

#define HDA_LVERB_SET_AMP_GAIN_MUTE 0x3
#define HDA_LVERB_SET_CONV_FORMAT   0x2

#define HDA_PARAM_SUBORDINATE_NODE_COUNT 0x04
#define HDA_PARAM_FUNCTION_GROUP_TYPE    0x05
#define HDA_PARAM_AUDIO_WIDGET_CAPS      0x09
#define HDA_PARAM_PIN_CAPS               0x0C
#define HDA_PARAM_CONN_LIST_LENGTH       0x0E
#define HDA_PARAM_OUT_AMP_CAPS           0x12

static uint32_t hda_get_param(uint8_t nid, uint8_t param_id) {
    uint32_t resp = 0;
    hda_verb(nid, HDA_VERB_GET_PARAMETER, param_id, &resp);
    return resp;
}

/* ======================================================================== */
/* Сброс контроллера, CORB/RIRB                                             */
/* ======================================================================== */

static int hda_find_controller(const pci_device_t **out_dev) {
    const pci_device_t *dev = pci_find_class(HDA_CLASS, HDA_SUBCLASS);
    if (!dev) {
        printf("[HDA] Controller not found\n");
        return 0;
    }

    pci_bar_t bar0;
    if (pci_get_bar(dev, 0, &bar0) != 0 || bar0.is_io) {
        printf("[HDA] BAR0 missing or not MMIO\n");
        return 0;
    }

    pci_enable_memory(dev);
    pci_enable_bus_master(dev);
    pci_disable_interrupts(dev); // опрашиваем сами — см. разбор вверху файла

    uint64_t map_size = bar0.size ? bar0.size : 0x4000;
    g_mmio = mmio_map(bar0.address, map_size);
    if (!g_mmio) {
        printf("[HDA] Failed to map BAR0\n");
        return 0;
    }

    *out_dev = dev;
    return 1;
}

static int hda_reset_controller(void) {
    // CRST: 0 -> перевод в reset, 1 -> вывод из reset. Контроллер сам
    // доводит переход до конца — ждём readback в обе стороны (busy-wait
    // с pit_wait_ms(), см. HDA_VERB_TIMEOUT_MS выше).
    reg_write32(g_mmio, HDA_REG_GCTL, reg_read32(g_mmio, HDA_REG_GCTL) & ~HDA_GCTL_CRST);
    for (int i = 0; i < 200 && (reg_read32(g_mmio, HDA_REG_GCTL) & HDA_GCTL_CRST); i++) pit_wait_ms(1);
    if (reg_read32(g_mmio, HDA_REG_GCTL) & HDA_GCTL_CRST) {
        printf("[HDA] Controller did not enter reset\n");
        return 0;
    }

    reg_write32(g_mmio, HDA_REG_GCTL, reg_read32(g_mmio, HDA_REG_GCTL) | HDA_GCTL_CRST);
    for (int i = 0; i < 200 && !(reg_read32(g_mmio, HDA_REG_GCTL) & HDA_GCTL_CRST); i++) pit_wait_ms(1);
    if (!(reg_read32(g_mmio, HDA_REG_GCTL) & HDA_GCTL_CRST)) {
        printf("[HDA] Controller did not leave reset\n");
        return 0;
    }

    // Errata (см. revision history спецификации): минимум 521мкс (25
    // фреймов) после CRST=1 перед обращением к кодеку — один тик PIT (10мс)
    // с огромным запасом это покрывает.
    pit_wait_ms(1);
    return 1;
}

static int hda_setup_corb_rirb(void) {
    uint64_t corb_phys = pmm_alloc_page();
    uint64_t rirb_phys = pmm_alloc_page();
    if (!corb_phys || !rirb_phys) { printf("[HDA] Out of memory for CORB/RIRB\n"); return 0; }
    g_corb_phys = corb_phys; g_corb_virt = (uint32_t *)phys_to_virt(corb_phys);
    g_rirb_phys = rirb_phys; g_rirb_virt = (hda_rirb_entry_t *)phys_to_virt(rirb_phys);
    memset(g_corb_virt, 0, PAGE_SIZE);
    memset(g_rirb_virt, 0, PAGE_SIZE);

    // Останавливаем DMA перед перенастройкой (должны уже быть остановлены
    // после CRST, но без этого предположения безопаснее).
    reg_write8(g_mmio, HDA_REG_CORBCTL, 0);
    reg_write8(g_mmio, HDA_REG_RIRBCTL, 0);

    uint8_t corb_cap = (uint8_t)(reg_read8(g_mmio, HDA_REG_CORBSIZE) >> 4);
    uint8_t corb_sel = (corb_cap & 0x4) ? 2 : (corb_cap & 0x2) ? 1 : 0;
    g_corb_entries = (corb_sel == 2) ? 256 : (corb_sel == 1) ? 16 : 2;
    reg_write8(g_mmio, HDA_REG_CORBSIZE, corb_sel);

    uint8_t rirb_cap = (uint8_t)(reg_read8(g_mmio, HDA_REG_RIRBSIZE) >> 4);
    uint8_t rirb_sel = (rirb_cap & 0x4) ? 2 : (rirb_cap & 0x2) ? 1 : 0;
    g_rirb_entries = (rirb_sel == 2) ? 256 : (rirb_sel == 1) ? 16 : 2;
    reg_write8(g_mmio, HDA_REG_RIRBSIZE, rirb_sel);

    reg_write32(g_mmio, HDA_REG_CORBLBASE, (uint32_t)(corb_phys & 0xFFFFFFFFu));
    reg_write32(g_mmio, HDA_REG_CORBUBASE, (uint32_t)(corb_phys >> 32));
    reg_write32(g_mmio, HDA_REG_RIRBLBASE, (uint32_t)(rirb_phys & 0xFFFFFFFFu));
    reg_write32(g_mmio, HDA_REG_RIRBUBASE, (uint32_t)(rirb_phys >> 32));

    // CORBRP reset — "запрос-и-жди" (бит RW, читается 1 когда готово, затем
    // явно сбрасывается обратно и снова ожидается readback 0) — ИНАЧЕ, чем
    // RIRBWP ниже (тот — чистый самосбрасывающийся strobe, спецификация
    // прямо говорит "will always be read as 0").
    reg_write16(g_mmio, HDA_REG_CORBRP, 0x8000);
    for (int i = 0; i < 200 && !(reg_read16(g_mmio, HDA_REG_CORBRP) & 0x8000); i++) pit_wait_ms(1);
    reg_write16(g_mmio, HDA_REG_CORBRP, 0);
    for (int i = 0; i < 200 && (reg_read16(g_mmio, HDA_REG_CORBRP) & 0x8000); i++) pit_wait_ms(1);
    reg_write16(g_mmio, HDA_REG_CORBWP, 0);

    reg_write16(g_mmio, HDA_REG_RIRBWP, 0x8000);
    g_rirb_read_idx = 0;
    // QEMU (hw/audio/intel-hda.c) держит счётчик отправленных в RIRB ответов
    // и отказывается забирать из CORB что-либо ещё при совпадении с RINTCNT.
    // RINTCNT=0 QEMU трактует как буквальный 0 (не "256" по спецификации
    // 3.3.28), что блокирует даже первый verb — нужно максимальное ненулевое
    // значение (0xFF), с запасом на сотни verb'ов между сбросами счётчика
    // (сброс — через HDA_RIRBCTL_IRQEN, см. выше).
    reg_write16(g_mmio, HDA_REG_RINTCNT, 0xFF);

    reg_write8(g_mmio, HDA_REG_CORBCTL, HDA_CORBCTL_RUN);
    reg_write8(g_mmio, HDA_REG_RIRBCTL, HDA_RIRBCTL_DMAEN | HDA_RIRBCTL_IRQEN);

    return 1;
}

/* ======================================================================== */
/* Обход топологии кодека                                                   */
/* ======================================================================== */

#define HDA_WIDGET_TYPE_AUDIO_OUTPUT 0x0
#define HDA_WIDGET_TYPE_PIN_COMPLEX  0x4

// Первая запись Connection List виджета nid (0, если списка нет/пуст/
// long-form). Long-form списки (16-битные записи, бит7 параметра Connection
// List Length) этот драйвер не разбирает — осознанно, см. разбор в hda.h:
// 0 здесь трактуется как "нет связи", обход топологии honестно откажется,
// а не прочитает short-form байт из того, что на самом деле long-form слово.
static uint8_t hda_conn_list_first(uint8_t nid) {
    uint32_t resp = hda_get_param(nid, HDA_PARAM_CONN_LIST_LENGTH);
    if ((resp & 0x80) != 0) return 0; // long-form — не поддержано
    if ((resp & 0x7F) == 0) return 0; // длина 0 — нечего обходить
    uint32_t entries;
    if (hda_verb(nid, HDA_VERB_GET_CONN_LIST, 0, &entries) != 0) return 0;
    return (uint8_t)(entries & 0xFFu); // Entry 0 в младшем байте (short-form список)
}

// Ищет кодек: первый Audio Function Group, затем в нём — первый ЦАП
// (Audio Output widget) и первый реально подключённый выходной Pin
// Complex. Проверяет, что Pin слушает именно найденный ЦАП НАПРЯМУЮ либо
// через один промежуточный узел (см. разбор в hda.c) — иначе откат, g_ready
// остаётся 0, а не разводка звука не в тот узел.
static int hda_discover_codec(void) {
    uint32_t root_sub = hda_get_param(0, HDA_PARAM_SUBORDINATE_NODE_COUNT);
    uint8_t fg_start = (uint8_t)((root_sub >> 16) & 0xFFu);
    uint8_t fg_count = (uint8_t)(root_sub & 0xFFu);

    uint8_t afg = 0;
    for (uint8_t nid = fg_start; nid < fg_start + fg_count; nid++) {
        uint32_t fg_type = hda_get_param(nid, HDA_PARAM_FUNCTION_GROUP_TYPE);
        if ((fg_type & 0xFFu) == 0x01) { afg = nid; break; } // Audio Function Group
    }
    if (!afg) {
        printf("[HDA] No Audio Function Group found\n");
        return 0;
    }

    uint32_t afg_sub = hda_get_param(afg, HDA_PARAM_SUBORDINATE_NODE_COUNT);
    uint8_t w_start = (uint8_t)((afg_sub >> 16) & 0xFFu);
    uint8_t w_count = (uint8_t)(afg_sub & 0xFFu);

    uint8_t dac = 0, pin = 0;
    for (uint16_t nid = w_start; nid < (uint16_t)w_start + w_count; nid++) {
        uint32_t caps = hda_get_param((uint8_t)nid, HDA_PARAM_AUDIO_WIDGET_CAPS);
        uint8_t type = (uint8_t)((caps >> 20) & 0xFu);

        if (type == HDA_WIDGET_TYPE_AUDIO_OUTPUT && !dac) {
            dac = (uint8_t)nid;
        } else if (type == HDA_WIDGET_TYPE_PIN_COMPLEX) {
            uint32_t cfgdef; hda_verb((uint8_t)nid, HDA_VERB_GET_CONFIG_DEF, 0, &cfgdef);
            uint8_t port_conn = (uint8_t)((cfgdef >> 30) & 0x3u);
            uint8_t default_dev = (uint8_t)((cfgdef >> 20) & 0xFu);
            uint32_t pincaps = hda_get_param((uint8_t)nid, HDA_PARAM_PIN_CAPS);
            int output_capable = (pincaps & (1u << 4)) != 0;

            if (!pin && port_conn != 1 /* не "нет физического соединения" */ && output_capable &&
                (default_dev == 0x0 /* Line Out */ || default_dev == 0x1 /* Speaker */ || default_dev == 0x2 /* HP Out */))
            {
                pin = (uint8_t)nid;
            }
        }
    }

    if (!dac || !pin) {
        printf("[HDA] No usable DAC/output pin found (dac=%u pin=%u)\n", dac, pin);
        return 0;
    }

    // Проверяем реальное соединение Pin -> ЦАП: напрямую, либо через ОДИН
    // промежуточный узел (Mixer/Selector) — см. разбор ограничения в hda.h.
    uint8_t via = 0;
    uint8_t first = hda_conn_list_first(pin);
    if (first == dac) {
        via = 0;
    } else if (first != 0 && hda_conn_list_first(first) == dac) {
        via = first;
    } else {
        printf("[HDA] Pin %u does not reach DAC %u within 2 hops (first=%u)\n", pin, dac, first);
        return 0;
    }

    g_afg_nid = afg;
    g_dac_nid = dac;
    g_pin_nid = pin;
    g_pin_via_node = via ? (int)via : -1;

    DLOG("[HDA] Topology: AFG=%u DAC=%u Pin=%u via=%d\n", afg, dac, pin, g_pin_via_node);
    return 1;
}

// Если у узла больше одной записи в Connection List, нужно явно выбрать ту,
// что ведёт к нашему ЦАП (Set Connection Select Control) — при единственной
// записи этот control вообще не существует (см. спецификацию 7.3.3.2), и
// посылать verb не нужно (и небезопасно полагаться на его эффект).
static void hda_select_connection_to_dac(uint8_t node, uint8_t target_dac) {
    uint32_t len_resp = hda_get_param(node, HDA_PARAM_CONN_LIST_LENGTH);
    if (len_resp & 0x80) return; // long-form — не поддержано, см. hda_conn_list_first()
    uint8_t len = (uint8_t)(len_resp & 0x7Fu);
    if (len <= 1) return;

    uint32_t entries;
    if (hda_verb(node, HDA_VERB_GET_CONN_LIST, 0, &entries) != 0) return;
    for (uint8_t i = 0; i < len && i < 4; i++) {
        uint8_t e = (uint8_t)((entries >> (i * 8)) & 0xFFu);
        if (e == target_dac) {
            hda_verb(node, HDA_VERB_SET_CONN_SELECT, i, NULL);
            return;
        }
    }
}

/* ======================================================================== */
/* Громкость/формат — кэшированное состояние, write-through в кодек        */
/* (та же модель, что ac97_set_volume()/ac97_set_sample_rate() — см. ac97.c) */
/* ======================================================================== */

static uint8_t  g_volume = 80;
static uint32_t g_sample_rate = 48000;
static uint16_t g_fmt_reg = 0x11; // 48kHz/16bit/stereo — см. hda_rate_to_fmt()

static uint16_t hda_rate_to_fmt(uint32_t rate, uint32_t *out_actual_rate) {
    // {запрошено-или-ближе-всего, BASE, MULT, DIV} — покрывает весь набор,
    // который реально можно собрать из BASE(1бит)/MULT(3бита)/DIV(3бита) на
    // стандартных 48кГц/44.1кГц базах (см. разбор в hda.h/Табл. "Stream
    // Format" спецификации, раздел 3.7.1 — то же кодирование, что и у
    // SDnFMT, Set Converter Format использует его 1:1).
    static const struct { uint32_t rate; uint8_t base, mult, div; } table[] = {
        { 8000,  0, 0, 5 }, { 11025, 1, 0, 3 }, { 16000, 0, 0, 2 },
        { 22050, 1, 0, 1 }, { 32000, 0, 1, 2 }, { 44100, 1, 0, 0 },
        { 48000, 0, 0, 0 }, { 88200, 1, 1, 0 }, { 96000, 0, 1, 0 },
        { 176400,1, 3, 0 }, { 192000,0, 3, 0 },
    };
    uint32_t best = 48000; uint32_t best_diff = 0xFFFFFFFFu;
    uint8_t base = 0, mult = 0, div = 0;
    for (unsigned i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        uint32_t diff = (rate > table[i].rate) ? (rate - table[i].rate) : (table[i].rate - rate);
        if (diff < best_diff) { best_diff = diff; best = table[i].rate; base = table[i].base; mult = table[i].mult; div = table[i].div; }
    }
    if (out_actual_rate) *out_actual_rate = best;
    // BITS=001 (16 бит), CHAN=0001 (2 канала, стерео) — фиксированно, этот
    // драйвер играет только 16-битное стерео (см. hda_play_pcm_stereo()).
    return (uint16_t)(((uint32_t)base << 14) | ((uint32_t)mult << 11) | ((uint32_t)div << 8) | (1u << 4) | 1u);
}

void hda_set_sample_rate(uint32_t rate) {
    uint32_t actual;
    g_fmt_reg = hda_rate_to_fmt(rate, &actual);
    g_sample_rate = actual;
}

uint32_t hda_get_sample_rate(void) { return g_sample_rate; }

// num_steps == 0 означает, что у узла нет реального Output Amp — verb не
// посылается вовсе (см. hda_init()).
static void hda_apply_amp(uint8_t nid, uint32_t num_steps, uint8_t volume) {
    if (num_steps == 0) return;
    uint32_t gain = ((uint32_t)volume * num_steps) / 100;
    if (gain > 0x7F) gain = 0x7F;
    uint8_t mute = (volume == 0) ? 1 : 0;
    // Два отдельных verb'а (Left, затем Right) вместо одного с обоими
    // битами сразу — спецификация (7.3.3.7) формально разрешает оба
    // варианта, но раздельные verb'ы надёжнее/совместимее (тот же приём,
    // что использует Linux snd-hda-intel).
    uint16_t base = (uint16_t)((1u << 15) | ((uint32_t)mute << 7) | gain);
    hda_verb_long(nid, HDA_LVERB_SET_AMP_GAIN_MUTE, (uint16_t)(base | (1u << 13)), NULL); // Left
    hda_verb_long(nid, HDA_LVERB_SET_AMP_GAIN_MUTE, (uint16_t)(base | (1u << 12)), NULL); // Right
}

void hda_set_volume(uint8_t volume) {
    if (volume > 100) volume = 100;
    g_volume = volume;
    if (!g_present) return;
    hda_apply_amp(g_dac_nid, g_dac_amp_steps, volume);
    hda_apply_amp(g_pin_nid, g_pin_amp_steps, volume);
}

uint8_t hda_get_volume(void) { return g_volume; }

/* ======================================================================== */
/* Вывод потока — Buffer Descriptor List + Stream Descriptor 0              */
/* ======================================================================== */

typedef struct __attribute__((packed)) {
    uint64_t address;
    uint32_t length;
    uint32_t ioc; // бит 0 = Interrupt On Completion (нам не нужно само прерывание, но ставим — безвредно и ближе к спеке)
} hda_bdl_entry_t;

#define HDA_BDL_ENTRIES   32
#define HDA_DMA_PAGE_SIZE 4096 // 1024 стерео 16-битных фрейма/страница — тот же расчёт, что у AC'97

static void  *g_sd0_base = NULL;
static uint64_t g_bdl_phys = 0; static hda_bdl_entry_t *g_bdl_virt = NULL;
static uint64_t g_data_phys[HDA_BDL_ENTRIES]; static int16_t *g_data_virt[HDA_BDL_ENTRIES];

static int hda_alloc_stream_buffers(void) {
    uint64_t bdl_phys = pmm_alloc_page();
    if (!bdl_phys) return 0;
    g_bdl_phys = bdl_phys;
    g_bdl_virt = (hda_bdl_entry_t *)phys_to_virt(bdl_phys);
    memset(g_bdl_virt, 0, PAGE_SIZE);

    for (int i = 0; i < HDA_BDL_ENTRIES; i++) {
        uint64_t p = pmm_alloc_page();
        if (!p) return 0;
        g_data_phys[i] = p;
        g_data_virt[i] = (int16_t *)phys_to_virt(p);
    }
    return 1;
}

// Программирует BDL/CBL/LVI под frame_count фреймов (<= HDA_BDL_ENTRIES *
// HDA_DMA_PAGE_SIZE/4), запускает поток и блокирующе ждёт BCIS (пока весь
// буфер не будет вычитан контроллером). В отличие от AC'97, RUN здесь не
// сбрасывается контроллером сам по себе по достижении LVI (поток
// зацикливается на BDL entry 0) — RUN нужно снять вручную сразу после
// BCIS, иначе DMA продолжит бесконечно переигрывать тот же буфер.
static int hda_run_buffer(uint32_t total_bytes, int entries_used) {
    // Stream Reset (SRST) перед (пере)программированием CBL/LVI/FMT/BDPL/
    // BDPU — спецификация (3.3.35) требует менять эти регистры только
    // после Global/Controller/Stream Reset, при RUN=0, иначе поведение не
    // определено (без сброса контроллер не подхватывает свежие CBL/BDPL —
    // поток идёт штатно по времени, но полностью без звука).
    reg_write8(g_sd0_base, HDA_SD_CTL, HDA_SDCTL_SRST);
    for (int i = 0; i < 200 && !(reg_read8(g_sd0_base, HDA_SD_CTL) & HDA_SDCTL_SRST); i++) pit_wait_ms(1);
    reg_write8(g_sd0_base, HDA_SD_CTL, 0);
    for (int i = 0; i < 200 && (reg_read8(g_sd0_base, HDA_SD_CTL) & HDA_SDCTL_SRST); i++) pit_wait_ms(1);

    reg_write32(g_sd0_base, HDA_SD_CBL, total_bytes);
    reg_write16(g_sd0_base, HDA_SD_LVI, (uint16_t)(entries_used - 1));
    reg_write16(g_sd0_base, HDA_SD_FMT, g_fmt_reg);
    reg_write32(g_sd0_base, HDA_SD_BDPL, (uint32_t)(g_bdl_phys & 0xFFFFFFFFu));
    reg_write32(g_sd0_base, HDA_SD_BDPU, (uint32_t)(g_bdl_phys >> 32));

    hda_verb_long(g_dac_nid, HDA_LVERB_SET_CONV_FORMAT, g_fmt_reg, NULL);
    hda_verb(g_dac_nid, HDA_VERB_SET_STREAM_CHAN, (1u << 4) | 0u, NULL); // stream=1, channel=0

    reg_write8(g_sd0_base, HDA_SD_STS, reg_read8(g_sd0_base, HDA_SD_STS)); // гасим протухшие RW1C-биты от прошлого прогона

    uint32_t ctl = reg_read32(g_sd0_base, HDA_SD_CTL);
    ctl &= ~(uint32_t)0xFFFFFFu; // низкие 24 бита = CTL, 25-31 принадлежат STS — не трогаем их при RMW
    ctl |= (1u << HDA_SDCTL_STRM_SHIFT) | HDA_SDCTL_RUN;
    reg_write32(g_sd0_base, HDA_SD_CTL, ctl);

    int ok = 0;
    for (int ms = 0; ms < 5000; ms++) {
        if (reg_read8(g_sd0_base, HDA_SD_STS) & HDA_SDSTS_BCIS) { ok = 1; break; }
        pit_wait_ms(1);
    }

    reg_write32(g_sd0_base, HDA_SD_CTL, reg_read32(g_sd0_base, HDA_SD_CTL) & ~(uint32_t)HDA_SDCTL_RUN);
    reg_write8(g_sd0_base, HDA_SD_STS, reg_read8(g_sd0_base, HDA_SD_STS)); // W1C: подтверждаем BCIS/прочее

    if (!ok) printf("[HDA] stream buffer timed out\n");
    return ok ? 0 : -1;
}

int hda_play_pcm_stereo(const int16_t *samples, uint32_t frames) {
    if (!g_ready || frames == 0) return -1;

    const uint32_t frames_per_page = HDA_DMA_PAGE_SIZE / 4; // 4 байта/стерео-фрейм (16-бит L+R)
    uint32_t done = 0;

    while (done < frames) {
        uint32_t remaining = frames - done;
        uint32_t chunk_frames = remaining;
        uint32_t max_frames = frames_per_page * HDA_BDL_ENTRIES;
        if (chunk_frames > max_frames) chunk_frames = max_frames;

        int entries_used = 0;
        uint32_t copied = 0;
        while (copied < chunk_frames) {
            uint32_t n = chunk_frames - copied;
            if (n > frames_per_page) n = frames_per_page;
            memcpy(g_data_virt[entries_used], samples + (uint64_t)(done + copied) * 2, (size_t)n * 4);

            g_bdl_virt[entries_used].address = g_data_phys[entries_used];
            g_bdl_virt[entries_used].length = n * 4;
            g_bdl_virt[entries_used].ioc = 0;

            copied += n;
            entries_used++;
        }
        g_bdl_virt[entries_used - 1].ioc = 1; // IOC только на последней записи — см. разбор BCIS выше

        if (hda_run_buffer(chunk_frames * 4, entries_used) != 0) return (int)done > 0 ? (int)done : -1;
        done += chunk_frames;
    }
    return (int)done;
}

// Дешёвая целочисленная аппроксимация синусоиды (треугольник) — тот же
// подход, что ac97_wave()/ac97_play_tone() в ac97.c (никакого libm), просто
// отдельная реализация под эту пару DMA-страниц/формат.
static int16_t hda_wave(uint32_t phase) {
    int32_t v = (int32_t)(phase >> 16) - 32768; // 0..65535 -> -32768..32767, линейный треугольник
    if (v < 0) v = -v;
    v = 16384 - v; // переворачиваем в треугольник вокруг нуля
    return (int16_t)(v * 2);
}

int hda_play_tone(uint32_t frequency, uint32_t duration_ms) {
    if (!g_ready || frequency == 0 || duration_ms == 0) return -1;

    uint32_t total_frames = (uint32_t)((uint64_t)g_sample_rate * duration_ms / 1000u);
    uint32_t phase_step = (uint32_t)(((uint64_t)frequency << 32) / g_sample_rate);
    uint32_t phase = 0;

    const uint32_t frames_per_page = HDA_DMA_PAGE_SIZE / 4;
    uint32_t done = 0;
    while (done < total_frames) {
        uint32_t remaining = total_frames - done;
        uint32_t max_frames = frames_per_page * HDA_BDL_ENTRIES;
        uint32_t chunk_frames = remaining > max_frames ? max_frames : remaining;

        int entries_used = 0;
        uint32_t copied = 0;
        while (copied < chunk_frames) {
            uint32_t n = chunk_frames - copied;
            if (n > frames_per_page) n = frames_per_page;
            for (uint32_t i = 0; i < n; i++) {
                int16_t s = hda_wave(phase);
                phase += phase_step;
                g_data_virt[entries_used][i * 2] = s;
                g_data_virt[entries_used][i * 2 + 1] = s;
            }
            g_bdl_virt[entries_used].address = g_data_phys[entries_used];
            g_bdl_virt[entries_used].length = n * 4;
            g_bdl_virt[entries_used].ioc = 0;
            copied += n;
            entries_used++;
        }
        g_bdl_virt[entries_used - 1].ioc = 1;

        if (hda_run_buffer(chunk_frames * 4, entries_used) != 0) return -1;
        done += chunk_frames;
    }
    return 0;
}

void hda_stop(void) {
    if (!g_present || !g_sd0_base) return;
    reg_write32(g_sd0_base, HDA_SD_CTL, reg_read32(g_sd0_base, HDA_SD_CTL) & ~(uint32_t)HDA_SDCTL_RUN);
}

/* ======================================================================== */
/* Публичный API                                                            */
/* ======================================================================== */

int hda_is_available(void) { return g_ready; }

int hda_init(void) {
    const pci_device_t *dev = NULL;
    if (!hda_find_controller(&dev)) return -1;
    if (!hda_reset_controller()) return -1;

    uint16_t statests = reg_read16(g_mmio, HDA_REG_STATESTS);
    if (statests == 0) {
        printf("[HDA] No codec responded (STATESTS=0)\n");
        return -1;
    }
    for (int i = 0; i < 15; i++) {
        if (statests & (1u << i)) { g_codec_addr = (uint8_t)i; break; }
    }
    reg_write16(g_mmio, HDA_REG_STATESTS, statests); // RW1CS — подтверждаем
    DLOG("[HDA] STATESTS=%u codec_addr=%u GCAP=%u\n", statests, g_codec_addr, reg_read16(g_mmio, HDA_REG_GCAP));

    if (!hda_setup_corb_rirb()) return -1;
    g_present = 1;

    if (!hda_discover_codec()) return -1;

    // Pin часто не имеет своего Output Amp (OUT_AMP_CAPS==0). При mixer=on
    // кодек учитывает gain каждого узла в цепочке DAC->Pin, поэтому "+1"
    // без проверки на 0 слало бы на такой Pin verb с gain=0 и глушило бы
    // звук целиком независимо от gain ЦАП. OUT_AMP_CAPS==0 трактуется как
    // "amp отсутствует" — verb на этот узел не посылается (hda_apply_amp()).
    uint32_t dac_amp_caps = hda_get_param(g_dac_nid, HDA_PARAM_OUT_AMP_CAPS);
    g_dac_amp_steps = dac_amp_caps ? (((dac_amp_caps >> 8) & 0x7Fu) + 1) : 0;
    uint32_t pin_amp_caps = hda_get_param(g_pin_nid, HDA_PARAM_OUT_AMP_CAPS);
    g_pin_amp_steps = pin_amp_caps ? (((pin_amp_caps >> 8) & 0x7Fu) + 1) : 0;

    hda_verb(g_afg_nid, HDA_VERB_SET_POWER_STATE, 0, NULL); // D0
    hda_verb(g_dac_nid, HDA_VERB_SET_POWER_STATE, 0, NULL); // D0
    hda_verb(g_pin_nid, HDA_VERB_SET_POWER_STATE, 0, NULL);
    if (g_pin_via_node > 0) hda_verb((uint8_t)g_pin_via_node, HDA_VERB_SET_POWER_STATE, 0, NULL);

    if (g_pin_via_node > 0) hda_select_connection_to_dac((uint8_t)g_pin_via_node, g_dac_nid);
    else hda_select_connection_to_dac(g_pin_nid, g_dac_nid);

    // PinCntl: Out Enable (бит6) + H-Phn Enable (бит7, безвредно для не-HP
    // Pin — см. спецификацию 7.3.3.13, игнорируется несуществующим усилителем).
    hda_verb(g_pin_nid, HDA_VERB_SET_PIN_CTL, (1u << 6) | (1u << 7), NULL);

    // EAPD (External Amplifier Power Down, verb 0x70C бит1) — по умолчанию
    // на некоторых кодеках усилитель выключен (EAPD=0), спецификация прямо
    // рекомендует драйверам явно включать его (см. revision history HDA 1.0a).
    hda_verb(g_pin_nid, 0x70C, 0x02, NULL);

    uint32_t oss = HDA_GCAP_OSS(reg_read16(g_mmio, HDA_REG_GCAP));
    uint32_t iss = HDA_GCAP_ISS(reg_read16(g_mmio, HDA_REG_GCAP));
    if (oss == 0) { printf("[HDA] Controller reports no output streams\n"); return -1; }
    (void)iss;
    g_sd0_base = (uint8_t *)g_mmio + HDA_REG_SD0_BASE + (uint64_t)iss * 0x20;

    if (!hda_alloc_stream_buffers()) { printf("[HDA] Out of memory for stream buffers\n"); return -1; }

    g_ready = 1;
    hda_set_sample_rate(48000);
    hda_set_volume(g_volume);

    DLOG("[HDA] Ready: codec=%u dac=%u pin=%u\n", g_codec_addr, g_dac_nid, g_pin_nid);
    klog("[HDA] controller ready (codec=%u dac=%u pin=%u)", g_codec_addr, g_dac_nid, g_pin_nid);
    return 0;
}
