// apic.c — Local APIC + I/O APIC, замена легаси 8259 PIC на реальном
// железе, где 8259 физически не подключён к CPU (прошивка держит
// платформу в APIC-режиме без обходного пути через IMCR назад в PIC-режим
// — см. pic_set_imcr_mode() в kernel.c). Источник данных — ACPI MADT
// ("APIC" таблица), разбирается здесь же через acpi_find_table_by_signature().
//
// Контракт с остальным ядром не меняется: IRQ по-прежнему адресуются
// "ISA-номером" 0-15 (irq_enable()/irq_disable() в irq.c, SYS_IRQ_REGISTER
// в driver.c), и по-прежнему попадают в CPU строго на вектор 0x20+irq
// (idt_init() эти гейты уже выставляет для IRQ-диапазона 32-47 — менять
// IDT не требуется, меняется только то, ЧТО физически доставляет сигнал
// до CPU: IOAPIC вместо 8259).
#include "apic.h"
#include "drivers/console/console.h"
#include "system/acpi/acpi.h"
#include "system/mm/paging.h"
#include "system/devmode/devmode.h"

// ===== ACPI MADT ("APIC" table, ACPI spec §5.2.12) =====

typedef struct __attribute__((packed)) {
    ACPI_SDTHeader header;
    uint32_t local_apic_addr;   // 32-битный физ. адрес LAPIC (может быть
                                 // переопределён Type 5 ниже)
    uint32_t flags;             // бит0 = PCAT_COMPAT (платформа также
                                 // физически несёт пару 8259)
    uint8_t  entries[];         // дальше — вариативные записи
} ACPI_MADT;

#define MADT_TYPE_IOAPIC            1
#define MADT_TYPE_INT_SRC_OVERRIDE  2
#define MADT_TYPE_LAPIC_OVERRIDE    5

typedef struct __attribute__((packed)) {
    uint8_t type;
    uint8_t length;
} ACPI_MADT_ENTRY_HDR;

typedef struct __attribute__((packed)) {
    ACPI_MADT_ENTRY_HDR hdr;
    uint8_t  ioapic_id;
    uint8_t  reserved;
    uint32_t ioapic_addr;
    uint32_t gsi_base;
} ACPI_MADT_IOAPIC;

typedef struct __attribute__((packed)) {
    ACPI_MADT_ENTRY_HDR hdr;
    uint8_t  bus;       // 0 = ISA
    uint8_t  source;    // ISA IRQ номер
    uint32_t gsi;        // на какой GSI реально переопределён
    uint16_t flags;      // биты 0-1 polarity, биты 2-3 trigger mode (MPS INTI flags)
} ACPI_MADT_ISO;

typedef struct __attribute__((packed)) {
    ACPI_MADT_ENTRY_HDR hdr;
    uint16_t reserved;
    uint64_t local_apic_addr;
} ACPI_MADT_LAPIC_OVERRIDE;

// ===== MSR / регистры =====

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}
static inline void wrmsr(uint32_t msr, uint64_t value) {
    asm volatile("wrmsr" : : "c"(msr), "a"((uint32_t)value), "d"((uint32_t)(value >> 32)));
}

#define IA32_APIC_BASE_MSR 0x1B
#define IA32_APIC_BASE_ENABLE (1ULL << 11) // EN
#define IA32_APIC_BASE_EXTD   (1ULL << 10) // x2APIC mode (EXTD)

#define LAPIC_REG_ID    0x020
#define LAPIC_REG_VER   0x030
#define LAPIC_REG_TPR   0x080
#define LAPIC_REG_EOI   0x0B0
#define LAPIC_REG_SVR   0x0F0

#define LAPIC_SVR_ENABLE (1u << 8)
#define LAPIC_SPURIOUS_VECTOR 0xFF // попадает в isr_default() (48-255 — "безопасная заглушка")

static inline int cpu_supports_x2apic(void) {
    uint32_t eax, ebx, ecx, edx;
    asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1));
    return (ecx >> 21) & 1;
}

// x2APIC без поднятого Interrupt Remapping (VT-d IRTA.EIME + таблица
// IRTE) архитектурно не гарантирован для обычных (compatibility-format)
// прерываний от IOAPIC — а IR этот кернел не строит (отдельная большая
// фича). Поэтому НЕ переключаемся в x2APIC сами по себе (APIC_PREFER_X2APIC
// = 0) — только если прошивка уже сделала это сама (откат в xAPIC без
// полного ресета невозможен, см. lapic_init()) или если прибавится IR.
#define APIC_PREFER_X2APIC 0

static volatile uint8_t *g_lapic_base = NULL; // только xAPIC-фоллбэк (CPU без x2APIC)
static int g_x2apic_mode = 0;
static uint32_t g_bsp_apic_id = 0;

// offset — обычное xAPIC MMIO-смещение (LAPIC_REG_*); в x2APIC-режиме сам
// пересчитывает его в соответствующий MSR (0x800 + offset/0x10 — Intel
// x2APIC spec §2.1, тот же порядок регистров, что и в MMIO-варианте).
static inline uint32_t lapic_read(uint32_t offset) {
    if (g_x2apic_mode) return (uint32_t)rdmsr(0x800u + (offset >> 4));
    return *(volatile uint32_t *)(g_lapic_base + offset);
}
static inline void lapic_write(uint32_t offset, uint32_t value) {
    if (g_x2apic_mode) { wrmsr(0x800u + (offset >> 4), value); return; }
    *(volatile uint32_t *)(g_lapic_base + offset) = value;
}

#define IOAPIC_REG_IOREGSEL 0x00
#define IOAPIC_REG_IOWIN    0x10

#define IOAPIC_REG_ID       0x00
#define IOAPIC_REG_VER      0x01
#define IOAPIC_REG_REDTBL   0x10 // +2*n (low), +2*n+1 (high), n = pin index

#define IOAPIC_REDTBL_MASKED       (1u << 16)
#define IOAPIC_REDTBL_TRIGGER_LEVEL (1u << 15)
#define IOAPIC_REDTBL_POLARITY_LOW  (1u << 13)
#define IOAPIC_REDTBL_DESTMODE_PHYSICAL (0u << 11)
#define IOAPIC_REDTBL_DELMODE_FIXED (0u << 8)

// Поддерживаем только ОДИН IOAPIC — у любой реальной машины, под которую
// пишется этот кернел (один сокет, без NUMA/множественных IO-доменов),
// этого достаточно; MADT может перечислять больше одного, но мы берём
// первый найденный и используем его для GSI 0..(max_entries-1].
static volatile uint8_t *g_ioapic_base = NULL;
static uint32_t g_ioapic_gsi_base = 0;
static uint32_t g_ioapic_max_entries = 0;

// ISA IRQ -> (GSI, MPS INTI flags) из Interrupt Source Override записей.
// gsi == 0xFFFFFFFF — нет override, используем identity (gsi == irq).
#define ISA_IRQ_COUNT 16
static uint32_t g_iso_gsi[ISA_IRQ_COUNT];
static uint16_t g_iso_flags[ISA_IRQ_COUNT];

static inline uint32_t ioapic_read(uint32_t reg) {
    *(volatile uint32_t *)(g_ioapic_base + IOAPIC_REG_IOREGSEL) = reg;
    return *(volatile uint32_t *)(g_ioapic_base + IOAPIC_REG_IOWIN);
}
static inline void ioapic_write(uint32_t reg, uint32_t value) {
    *(volatile uint32_t *)(g_ioapic_base + IOAPIC_REG_IOREGSEL) = reg;
    *(volatile uint32_t *)(g_ioapic_base + IOAPIC_REG_IOWIN) = value;
}

static int g_apic_available = 0;

int apic_available(void) { return g_apic_available; }

static void lapic_init(uint64_t lapic_phys) {
    uint64_t apic_base_msr = rdmsr(IA32_APIC_BASE_MSR);

    // x2APIC берём, только если сами так решили (APIC_PREFER_X2APIC) ИЛИ
    // прошивка уже необратимо включила его сама (EXTD=1 — откатиться в
    // xAPIC без полного аппаратного ресета нельзя, Intel x2APIC spec).
    // Иначе — xAPIC (MMIO): ему, в отличие от x2APIC, Interrupt Remapping
    // не требуется вообще для доставки обычных (compatibility-format)
    // прерываний от IOAPIC.
    int firmware_already_x2apic = (apic_base_msr & IA32_APIC_BASE_EXTD) != 0;
    if ((APIC_PREFER_X2APIC || firmware_already_x2apic) && cpu_supports_x2apic()) {
        wrmsr(IA32_APIC_BASE_MSR, apic_base_msr | IA32_APIC_BASE_ENABLE | IA32_APIC_BASE_EXTD);
        g_x2apic_mode = 1;
        DLOG("[APIC] using x2APIC (MSR-based)%s\n",
             firmware_already_x2apic ? " — прошивка уже включила x2APIC" : "");
    } else {
        wrmsr(IA32_APIC_BASE_MSR, apic_base_msr | IA32_APIC_BASE_ENABLE);
        g_lapic_base = (volatile uint8_t *)mmio_map(lapic_phys, 4096);
        if (!g_lapic_base) {
            printf("[APIC] mmio_map(LAPIC) failed\n");
            return;
        }
        DLOG("[APIC] using xAPIC (MMIO at phys=0x%lx)\n", lapic_phys);
    }

    // SVR: бит8 — программный enable (отдельный от MSR EN-бита выше, ОБА
    // нужны), биты 0-7 — spurious vector.
    lapic_write(LAPIC_REG_SVR, LAPIC_SVR_ENABLE | LAPIC_SPURIOUS_VECTOR);
    // TPR=0 — принимать прерывания любого приоритета.
    lapic_write(LAPIC_REG_TPR, 0);

    // x2APIC ID register — полный 32-битный ID БЕЗ сдвига (Intel x2APIC
    // spec §2.3); xAPIC ID register — ID в битах 31:24, нужен >>24.
    g_bsp_apic_id = g_x2apic_mode ? lapic_read(LAPIC_REG_ID) : (lapic_read(LAPIC_REG_ID) >> 24);
    DLOG("[APIC] LAPIC id=%u ver=0x%x\n", g_bsp_apic_id, lapic_read(LAPIC_REG_VER));
}

void lapic_eoi(void) {
    // g_lapic_base остаётся NULL в x2APIC-режиме (там нет MMIO вообще, см.
    // lapic_read/write) — проверять нужно "LAPIC хоть как-то
    // инициализирован", не конкретно MMIO-указатель, иначе EOI в x2APIC-
    // режиме молча не отправлялся бы НИКОГДА, и уже первое же обслуженное
    // прерывание намертво блокировало бы все следующие прерывания той же
    // линии.
    if (!g_lapic_base && !g_x2apic_mode) return;
    lapic_write(LAPIC_REG_EOI, 0);
}

// irq=0..15 -> физический GSI (с учётом override), и флаги polarity/trigger
// в формате IOAPIC redirection entry (НЕ в сыром MPS INTI формате).
static uint32_t isa_irq_to_gsi(uint8_t irq, uint32_t *out_redtbl_flags) {
    uint32_t gsi = (g_iso_gsi[irq] != 0xFFFFFFFFu) ? g_iso_gsi[irq] : irq;
    uint16_t mps_flags = g_iso_flags[irq];

    uint32_t redtbl_flags = 0;
    // MPS INTI polarity: биты 0-1. 00/01 = active high (умолчание ISA),
    // 11 = active low. Trigger: биты 2-3. 00/01 = edge (умолчание ISA),
    // 11 = level.
    uint16_t polarity = mps_flags & 0x3u;
    uint16_t trigger = (mps_flags >> 2) & 0x3u;
    if (polarity == 0x3u) redtbl_flags |= IOAPIC_REDTBL_POLARITY_LOW;
    if (trigger == 0x3u) redtbl_flags |= IOAPIC_REDTBL_TRIGGER_LEVEL;

    if (out_redtbl_flags) *out_redtbl_flags = redtbl_flags;
    return gsi;
}

void ioapic_enable_irq(uint8_t irq, uint8_t vector) {
    if (!g_apic_available || irq >= ISA_IRQ_COUNT) return;

    uint32_t redtbl_flags = 0;
    uint32_t gsi = isa_irq_to_gsi(irq, &redtbl_flags);
    if (gsi < g_ioapic_gsi_base || gsi >= g_ioapic_gsi_base + g_ioapic_max_entries) {
        printf("[APIC] irq %u -> GSI %u вне диапазона этого IOAPIC (base=%u max=%u), пропускаем\n",
               irq, gsi, g_ioapic_gsi_base, g_ioapic_max_entries);
        return;
    }
    uint32_t pin = gsi - g_ioapic_gsi_base;

    uint32_t low = (uint32_t)vector | IOAPIC_REDTBL_DELMODE_FIXED |
                   IOAPIC_REDTBL_DESTMODE_PHYSICAL | redtbl_flags; // маска снята (бит16=0)
    uint32_t high = g_bsp_apic_id << 24;

    ioapic_write(IOAPIC_REG_REDTBL + 2 * pin + 1, high); // high пишем первым — low последним снимает маску
    ioapic_write(IOAPIC_REG_REDTBL + 2 * pin, low);

    DLOG("[APIC] irq %u -> GSI %u (pin %u), vector=0x%x, dest_apic=%u, flags=0x%x\n",
         irq, gsi, pin, vector, g_bsp_apic_id, redtbl_flags);
}

void ioapic_disable_irq(uint8_t irq) {
    if (!g_apic_available || irq >= ISA_IRQ_COUNT) return;

    uint32_t gsi = isa_irq_to_gsi(irq, NULL);
    if (gsi < g_ioapic_gsi_base || gsi >= g_ioapic_gsi_base + g_ioapic_max_entries) return;
    uint32_t pin = gsi - g_ioapic_gsi_base;

    uint32_t low = ioapic_read(IOAPIC_REG_REDTBL + 2 * pin);
    ioapic_write(IOAPIC_REG_REDTBL + 2 * pin, low | IOAPIC_REDTBL_MASKED);
}

int apic_init(void) {
    void *table = acpi_find_table_by_signature("APIC");
    if (!table) {
        DLOG("[APIC] ACPI MADT (\"APIC\") table not found\n");
        return 0;
    }

    ACPI_MADT *madt = (ACPI_MADT *)table;
    uint64_t lapic_phys = madt->local_apic_addr;

    for (uint8_t i = 0; i < ISA_IRQ_COUNT; i++) {
        g_iso_gsi[i] = 0xFFFFFFFFu;
        g_iso_flags[i] = 0;
    }

    uint32_t entries_len = madt->header.Length - (uint32_t)sizeof(ACPI_MADT);
    uint8_t *p = madt->entries;
    uint8_t *end = madt->entries + entries_len;
    int ioapic_found = 0;

    while (p + sizeof(ACPI_MADT_ENTRY_HDR) <= end) {
        ACPI_MADT_ENTRY_HDR *eh = (ACPI_MADT_ENTRY_HDR *)p;
        if (eh->length < sizeof(ACPI_MADT_ENTRY_HDR) || p + eh->length > end) break; // битая таблица

        if (eh->type == MADT_TYPE_IOAPIC && !ioapic_found) {
            ACPI_MADT_IOAPIC *io = (ACPI_MADT_IOAPIC *)p;
            g_ioapic_base = (volatile uint8_t *)mmio_map(io->ioapic_addr, 4096);
            g_ioapic_gsi_base = io->gsi_base;
            if (g_ioapic_base) {
                uint32_t ver = ioapic_read(IOAPIC_REG_VER);
                g_ioapic_max_entries = ((ver >> 16) & 0xFFu) + 1;
                if (g_ioapic_max_entries > 24) {
                    // Реальные одиночные IOAPIC почти всегда 24 — не бьём
                    // не глядя за пределы реальной таблицы (undefined/
                    // reserved регистровое пространство IOAPIC).
                    g_ioapic_max_entries = 24;
                }
                ioapic_found = 1;
                DLOG("[APIC] IOAPIC id=%u phys=0x%x gsi_base=%u entries=%u\n",
                     io->ioapic_id, io->ioapic_addr, g_ioapic_gsi_base, g_ioapic_max_entries);
            } else {
                printf("[APIC] mmio_map(IOAPIC) failed\n");
            }
        } else if (eh->type == MADT_TYPE_INT_SRC_OVERRIDE) {
            ACPI_MADT_ISO *iso = (ACPI_MADT_ISO *)p;
            if (iso->bus == 0 && iso->source < ISA_IRQ_COUNT) {
                g_iso_gsi[iso->source] = iso->gsi;
                g_iso_flags[iso->source] = iso->flags;
                DLOG("[APIC] ISO: ISA IRQ %u -> GSI %u (flags=0x%x)\n",
                     iso->source, iso->gsi, iso->flags);
            }
        } else if (eh->type == MADT_TYPE_LAPIC_OVERRIDE) {
            lapic_phys = ((ACPI_MADT_LAPIC_OVERRIDE *)p)->local_apic_addr;
        }

        p += eh->length;
    }

    if (!ioapic_found) {
        DLOG("[APIC] no usable I/O APIC entry in MADT\n");
        return 0;
    }

    lapic_init(lapic_phys);
    if (!g_lapic_base && !g_x2apic_mode) return 0; // ни MMIO (xAPIC), ни x2APIC не поднялись

    // Маскируем ВСЕ pin'ы IOAPIC на старте — ioapic_enable_irq() снимет
    // маску точечно по мере того, как остальное ядро реально включает
    // конкретные IRQ (irq_enable() в irq.c), ровно как раньше делал 8259.
    for (uint32_t pin = 0; pin < g_ioapic_max_entries; pin++) {
        ioapic_write(IOAPIC_REG_REDTBL + 2 * pin, IOAPIC_REDTBL_MASKED);
        ioapic_write(IOAPIC_REG_REDTBL + 2 * pin + 1, 0);
    }

    g_apic_available = 1;
    DLOG("[APIC] LAPIC+IOAPIC active — using APIC instead of legacy 8259 PIC\n");
    return 1;
}
