// hpet.c — High Precision Event Timer. Используется только как свободно
// бегущий счётчик (главный MMIO-регистр по смещению 0xF0) — сравнивающие
// таймеры HPET (и их прерывания) этому ядру не нужны, PIT (pit.c) уже
// обслуживает тики планировщика через IRQ0; HPET здесь — ТОЛЬКО источник
// точного времени, опрашивается как обычный регистр, без единого
// прерывания, тем же принципом, что и остальной ввод-вывод в этом ядре.
#include "hpet.h"
#include "system/acpi/acpi.h"
#include "system/mm/paging.h"
#include "system/devmode/devmode.h"

// ACPI "HPET" table (ACPI spec, "IA-PC HPET Description Table").
typedef struct __attribute__((packed)) {
    ACPI_SDTHeader header;
    uint32_t event_timer_block_id;
    ACPI_GAS base_address;
    uint8_t  hpet_number;
    uint16_t minimum_tick;
    uint8_t  page_protection;
} ACPI_HPET;

#define HPET_REG_CAPABILITIES   0x000
#define HPET_REG_CONFIG         0x010
#define HPET_REG_MAIN_COUNTER   0x0F0
#define HPET_MMIO_SIZE          1024 // с запасом хватает на caps/config/счётчик

#define HPET_CFG_ENABLE (1ULL << 0)

static volatile uint8_t *hpet_base = NULL;
static uint64_t hpet_period_fs = 0; // длительность одного тика, фемтосекунды
static int g_hpet_available = 0;

static inline uint64_t hpet_reg_read(uint32_t offset) {
    return *(volatile uint64_t*)(hpet_base + offset);
}

static inline void hpet_reg_write(uint32_t offset, uint64_t value) {
    *(volatile uint64_t*)(hpet_base + offset) = value;
}

// 128-битное (широкое умножение) / 64-битное деление напрямую через
// аппаратный DIV — компилятор для настоящего `(unsigned __int128)a / b`
// сгенерировал бы вызов __udivti3 из libgcc, которого в этой
// -nostdlib-сборке попросту нет (см. аналогичное рассуждение в
// LufiraOS/libc/src/math.c). Здесь это безопасно: частное всегда помещается
// в 64 бита для любых реалистичных ns/us-вычислений (до ~584 лет), так что
// DIV не поймает #DE.
static inline uint64_t udiv128_64(unsigned __int128 dividend, uint64_t divisor) {
    uint64_t hi = (uint64_t)(dividend >> 64);
    uint64_t lo = (uint64_t)dividend;
    uint64_t quot;
    asm volatile ("divq %[d]" : "=a"(quot) : "a"(lo), "d"(hi), [d] "rm"(divisor) : "cc");
    return quot;
}

void hpet_init(void) {
    void *table = acpi_find_table_by_signature("HPET");
    if (!table) {
        DLOG("[HPET] ACPI HPET table not found\n");
        return;
    }

    ACPI_HPET *hpet_table = (ACPI_HPET*)table;
    if (hpet_table->base_address.AddressSpaceId != ACPI_GAS_SYSTEM_MEMORY ||
        hpet_table->base_address.Address == 0) {
        DLOG("[HPET] base address not in system memory, unsupported\n");
        return;
    }

    hpet_base = (volatile uint8_t*)mmio_map(hpet_table->base_address.Address, HPET_MMIO_SIZE);
    if (!hpet_base) {
        DLOG("[HPET] mmio_map failed\n");
        return;
    }

    uint64_t caps = hpet_reg_read(HPET_REG_CAPABILITIES);
    hpet_period_fs = caps >> 32; // COUNTER_CLK_PERIOD, старшие 32 бита Capabilities
    if (hpet_period_fs == 0) {
        DLOG("[HPET] invalid COUNTER_CLK_PERIOD, disabling\n");
        hpet_base = NULL;
        return;
    }

    uint64_t cfg = hpet_reg_read(HPET_REG_CONFIG);
    hpet_reg_write(HPET_REG_CONFIG, cfg & ~HPET_CFG_ENABLE);
    hpet_reg_write(HPET_REG_MAIN_COUNTER, 0);
    hpet_reg_write(HPET_REG_CONFIG, cfg | HPET_CFG_ENABLE);

    g_hpet_available = 1;
    DLOG("[HPET] enabled: base=0x%lx period=%lu fs (~%lu Hz)\n",
         (uint64_t)hpet_table->base_address.Address, hpet_period_fs,
         hpet_ticks_per_second());
}

int hpet_available(void) {
    return g_hpet_available;
}

uint64_t hpet_read_counter(void) {
    if (!g_hpet_available) return 0;
    return hpet_reg_read(HPET_REG_MAIN_COUNTER);
}

uint64_t hpet_ticks_per_second(void) {
    if (!g_hpet_available) return 0;
    // 1 секунда = 10^15 фемтосекунд.
    return udiv128_64((unsigned __int128)1000000000000000ULL, hpet_period_fs);
}

uint64_t hpet_read_ns(void) {
    if (!g_hpet_available) return 0;
    uint64_t ticks = hpet_reg_read(HPET_REG_MAIN_COUNTER);
    // ns = тики * период_fs / 10^6 (1 нс = 10^6 фс). Умножение тиков
    // (до 2^64) на период_fs легко выходит за 64 бита — отсюда широкое
    // __int128-умножение (просто `mul`, без каких-либо вызовов) перед
    // делением через udiv128_64() выше.
    return udiv128_64((unsigned __int128)ticks * hpet_period_fs, 1000000ULL);
}

void hpet_wait_us(uint32_t us) {
    if (!g_hpet_available || us == 0) return;

    uint64_t delta = udiv128_64((unsigned __int128)us * 1000000000ULL, hpet_period_fs);
    uint64_t target = hpet_reg_read(HPET_REG_MAIN_COUNTER) + delta;

    // Сравнение через знаковую разность — корректно переживает
    // закольцовывание 64-битного счётчика (на практике недостижимое при
    // реальных периодах работы, но бесплатно и по той же защитной логике,
    // что и остальные циклы ожидания в этом ядре).
    while ((int64_t)(hpet_reg_read(HPET_REG_MAIN_COUNTER) - target) < 0) {
        asm volatile ("pause");
    }
}
