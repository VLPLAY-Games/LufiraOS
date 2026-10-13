#include "acpi.h"
#include "drivers/console/console.h"
#include "lib/stddef.h"
#include "system/devmode/devmode.h"
#include "system/klog/klog.h"

// I/O port helpers
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    asm volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outb(uint16_t port, uint8_t val) {
    asm volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint16_t inw(uint16_t port) {
    uint16_t ret;
    asm volatile ("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outw(uint16_t port, uint16_t val) {
    asm volatile ("outw %0, %1" : : "a"(val), "Nd"(port));
}

// Глобальные переменные для ACPI
static ACPI_FADT *fadt = NULL;
static int acpi_ready = 0;

// Реальные SLP_TYPa/SLP_TYPb для S5 (soft off), уже сдвинутые в биты 10-12
// PM1_CNT (т.е. готовые к | с ACPI_SLP_EN) — находим их в DSDT при
// acpi_init(), см. acpi_find_s5_sleep_type(). 0 — спец-значение "не нашли",
// которое совпадает с типичным значением на QEMU/Bochs/VirtualBox, поэтому
// используется и как безопасный фоллбэк.
static uint16_t g_slp_typa = 0;
static uint16_t g_slp_typb = 0;

// Проверка контрольной суммы таблицы
static uint8_t acpi_checksum(void *table, uint32_t length) {
    uint8_t *bytes = (uint8_t*)table;
    uint8_t sum = 0;
    for (uint32_t i = 0; i < length; i++) {
        sum += bytes[i];
    }
    return sum;
}

// Поиск таблицы по сигнатуре
static void* acpi_find_table(void *sdt_start, uint32_t sdt_entries, 
                              int use_64bit, const char *signature) {
    for (uint32_t i = 0; i < sdt_entries; i++) {
        uint64_t table_addr;
        if (use_64bit) {
            uint64_t *xsdt = (uint64_t*)((uint8_t*)sdt_start + sizeof(ACPI_SDTHeader));
            table_addr = xsdt[i];
        } else {
            uint32_t *rsdt = (uint32_t*)((uint8_t*)sdt_start + sizeof(ACPI_SDTHeader));
            table_addr = (uint64_t)rsdt[i];
        }
        
        if (table_addr == 0) continue;
        
        ACPI_SDTHeader *header = (ACPI_SDTHeader*)table_addr;
        
        // Проверяем сигнатуру
        int match = 1;
        for (int j = 0; j < 4; j++) {
            if (header->Signature[j] != signature[j]) {
                match = 0;
                break;
            }
        }
        
        if (match && acpi_checksum(header, header->Length) == 0) {
            return (void*)table_addr;
        }
    }
    return NULL;
}

// Ищем в DSDT объявление \_S5 (System State 5 — soft off) и достаём
// реальные SLP_TYPa/SLP_TYPb. Это НЕ полноценный интерпретатор AML — целевой
// поиск байтового паттерна "_S5_" с минимальной проверкой контекста, тот же
// приём, что используют почти все hobby-ОС без ACPICA (см. OSDev wiki,
// статья "AML"). Без этого shutdown "работал" только там, где платформа
// случайно использует SLP_TYPa=0 (QEMU/Bochs/VirtualBox) — на части
// реального железа он другой (3, 5, 6, 7...), и ACPI-выключение просто
// не срабатывало бы, уходя в legacy-фоллбэки.
static int acpi_find_s5_sleep_type(uint16_t *out_typa, uint16_t *out_typb) {
    if (!fadt || fadt->Dsdt == 0) return -1;

    ACPI_SDTHeader *dsdt = (ACPI_SDTHeader*)(uint64_t)fadt->Dsdt;
    if (acpi_checksum(dsdt, dsdt->Length) != 0) {
        DLOG("[ACPI] DSDT checksum failed, skipping _S5 parse\n");
        return -1;
    }

    uint8_t *start = (uint8_t*)dsdt + sizeof(ACPI_SDTHeader);
    uint32_t len = dsdt->Length > sizeof(ACPI_SDTHeader)
                        ? dsdt->Length - sizeof(ACPI_SDTHeader) : 0;

    for (uint32_t i = 0; i + 4 <= len; i++) {
        if (!(start[i] == '_' && start[i + 1] == 'S' &&
              start[i + 2] == '5' && start[i + 3] == '_')) {
            continue;
        }

        uint32_t name_end = i + 4;

        // Подтверждаем, что это Name(_S5_, Package{...}) — объявление, а не
        // просто упоминание \_S5 где-то внутри метода (If/Notify и т.п.):
        // перед именем должен стоять NameOp (0x08) (возможно, с префиксом
        // корня '\\'), а сразу после имени — PackageOp (0x12).
        int has_nameop = (i >= 1 && start[i - 1] == 0x08) ||
                         (i >= 2 && start[i - 2] == 0x08 && start[i - 1] == '\\');
        if (!has_nameop || name_end >= len || start[name_end] != 0x12) continue;

        uint32_t p = name_end + 1; // первый байт PkgLength
        if (p >= len) continue;

        // PkgLength (AML §20.2.4): биты 7:6 первого байта — число
        // дополнительных байт длины. Пропускаем сам PkgLength целиком плюс
        // идущий следом байт NumElements.
        uint32_t extra = (uint32_t)((start[p] & 0xC0) >> 6);
        p += 1 + extra + 1;

        if (p >= len) continue;
        if (start[p] == 0x0A) p++; // BytePrefix перед SLP_TYPa
        if (p >= len) continue;
        uint8_t typa = start[p];
        p++;

        if (p >= len) continue;
        if (start[p] == 0x0A) p++; // BytePrefix перед SLP_TYPb
        if (p >= len) continue;
        uint8_t typb = start[p];

        *out_typa = (uint16_t)((typa & 0x7u) << 10);
        *out_typb = (uint16_t)((typb & 0x7u) << 10);
        return 0;
    }

    return -1;
}

int acpi_init(uint64_t rsdp_address) {
    if (rsdp_address == 0) {
        printf("[ACPI] RSDP address is NULL\n");
        return -1;
    }
    
    ACPI_RSDP_Rev2 *rsdp = (ACPI_RSDP_Rev2*)rsdp_address;
    
    // Проверяем сигнатуру
    for (int i = 0; i < 8; i++) {
        if (rsdp->rev1.Signature[i] != "RSD PTR "[i]) {
            printf("[ACPI] Invalid RSDP signature\n");
            return -1;
        }
    }
    
    // Проверяем контрольную сумму RSDP
    uint8_t checksum = acpi_checksum(rsdp, sizeof(ACPI_RSDP_Rev1));
    if (rsdp->rev1.Revision >= 2) {
        checksum = acpi_checksum(rsdp, sizeof(ACPI_RSDP_Rev2));
    }
    if (checksum != 0) {
        printf("[ACPI] RSDP checksum failed\n");
        return -1;
    }
    
    DLOG("[ACPI] RSDP found, Revision %d\n", rsdp->rev1.Revision);
    
    int use_64bit = 0;
    void *sdt = NULL;
    uint32_t sdt_entries = 0;
    
    if (rsdp->rev1.Revision >= 2 && rsdp->XsdtAddress != 0) {
        use_64bit = 1;
        sdt = (void*)(uint64_t)rsdp->XsdtAddress;
        ACPI_SDTHeader *header = (ACPI_SDTHeader*)sdt;
        
        if (acpi_checksum(header, header->Length) != 0) {
            printf("[ACPI] XSDT checksum failed\n");
            return -1;
        }
        
        sdt_entries = (header->Length - sizeof(ACPI_SDTHeader)) / 8;
        DLOG("[ACPI] Using XSDT at 0x%lx\n", (uint64_t)rsdp->XsdtAddress);
    } else {
        use_64bit = 0;
        sdt = (void*)(uint64_t)rsdp->rev1.RsdtAddress;
        ACPI_SDTHeader *header = (ACPI_SDTHeader*)sdt;
        
        if (acpi_checksum(header, header->Length) != 0) {
            printf("[ACPI] RSDT checksum failed\n");
            return -1;
        }
        
        sdt_entries = (header->Length - sizeof(ACPI_SDTHeader)) / 4;
        DLOG("[ACPI] Using RSDT at 0x%x\n", rsdp->rev1.RsdtAddress);
    }

    DLOG("[ACPI] %u SDT entries found\n", sdt_entries);
    
    // Ищем FADT
    fadt = (ACPI_FADT*)acpi_find_table(sdt, sdt_entries, use_64bit, "FACP");
    if (!fadt) {
        // Пробуем "FADT" (старая сигнатура)
        fadt = (ACPI_FADT*)acpi_find_table(sdt, sdt_entries, use_64bit, "FADT");
    }
    
    if (!fadt) {
        printf("[ACPI] FADT table not found\n");
        return -1;
    }
    
    DLOG("[ACPI] FADT found at 0x%lx\n", (uint64_t)fadt);
    DLOG("[ACPI] SMI_CMD: 0x%x\n", fadt->SMI_CMD);
    DLOG("[ACPI] ACPI_ENABLE: 0x%x\n", fadt->ACPI_ENABLE);
    DLOG("[ACPI] PM1a_CNT_BLK: 0x%x\n", fadt->PM1a_CNT_BLK);

    // Включаем ACPI режим, если нужно
    if (fadt->SMI_CMD != 0 && fadt->ACPI_ENABLE != 0) {
        DLOG("[ACPI] Enabling ACPI mode...\n");
        
        // Проверяем, включён ли уже ACPI
        if (fadt->PM1a_CNT_BLK != 0) {
            uint16_t pm1a = inw(fadt->PM1a_CNT_BLK);
            if (!(pm1a & 0x01)) {  // SCI_EN bit
                // Пытаемся включить ACPI
                outb(fadt->SMI_CMD, fadt->ACPI_ENABLE);
                
                // Ждём установки SCI_EN
                for (int i = 0; i < 300; i++) {
                    pm1a = inw(fadt->PM1a_CNT_BLK);
                    if (pm1a & 0x01) break;
                    for (volatile int j = 0; j < 10000; j++);
                }
                
                if (pm1a & 0x01) {
                    DLOG("[ACPI] ACPI mode enabled\n");
                } else {
                    printf("[ACPI] WARNING: Failed to enable ACPI mode (timeout)\n");
                    // Продолжаем - может, всё равно сработает
                }
            } else {
                DLOG("[ACPI] ACPI already enabled\n");
            }
        }
    }

    if (acpi_find_s5_sleep_type(&g_slp_typa, &g_slp_typb) == 0) {
        DLOG("[ACPI] _S5 found in DSDT: SLP_TYPa=0x%x SLP_TYPb=0x%x\n",
             g_slp_typa, g_slp_typb);
    } else {
        DLOG("[ACPI] _S5 not found/parseable in DSDT, falling back to "
             "SLP_TYPa=SLP_TYPb=0 (correct on QEMU/Bochs/VirtualBox)\n");
        g_slp_typa = 0;
        g_slp_typb = 0;
    }

    acpi_ready = 1;
    DLOG("[ACPI] Initialization complete\n");
    klog("[ACPI] initialized");
    return 0;
}

void acpi_shutdown(void) {
    if (!acpi_ready || !fadt) {
        printf("[ACPI] ACPI not initialized, using legacy methods\n");
        // Пробуем старые методы
        outw(0x604, 0x2000);  // QEMU
        outw(0xB004, 0x2000); // VirtualBox

        // Пробуем bochs
        outw(0x8900, 0xdead); // old bochs
        return; // fadt==NULL дальше — без этого return ниже был бы NULL-деref
    }

    DLOG("[ACPI] Shutting down via ACPI...\n");

    // Формируем значения для портов: SLP_TYPx | SLP_EN. g_slp_typa/b уже
    // найдены в DSDT при acpi_init() (или 0, если _S5 не распарсился) —
    // см. acpi_find_s5_sleep_type().
    uint16_t pm1a_value = g_slp_typa | ACPI_SLP_EN;
    uint16_t pm1b_value = g_slp_typb | ACPI_SLP_EN;

    DLOG("[ACPI] Writing 0x%x/0x%x to PM1a/PM1b_CNT (0x%x/0x%x)\n",
           pm1a_value, pm1b_value, fadt->PM1a_CNT_BLK, fadt->PM1b_CNT_BLK);

    if (fadt->PM1a_CNT_BLK != 0) {
        // Читаем текущее значение, чтобы сохранить биты
        uint16_t current = inw(fadt->PM1a_CNT_BLK);
        current &= ~(uint16_t)0x1FFF;  // Очищаем SLP_TYPa и SLP_EN
        current |= pm1a_value;
        outw(fadt->PM1a_CNT_BLK, current);
    }

    if (fadt->PM1b_CNT_BLK != 0) {
        uint16_t current = inw(fadt->PM1b_CNT_BLK);
        current &= ~(uint16_t)0x1FFF;
        current |= pm1b_value;
        outw(fadt->PM1b_CNT_BLK, current);
    }

    // Если не сработало, пробуем резервные методы
    printf("[ACPI] Shutdown didn't work, trying fallback methods...\n");
    outw(0x604, 0x2000);
    outw(0xB004, 0x2000);
    outw(0x8900, 0xdead);

    // Если и это не сработало
    printf("[ACPI] All shutdown methods failed\n");
}

int acpi_reset(void) {
    if (!acpi_ready || !fadt) return -1;
    if (fadt->header.Revision < 2) return -1;      // RESET_REG появился в ACPI 2.0
    if (fadt->header.Length < 129) return -1;       // ResetReg физически не влезает в таблицу
    if (!(fadt->Flags & ACPI_FADT_FLAG_RESET_REG_SUPPORTED)) return -1;

    const ACPI_GAS *reg = &fadt->ResetReg;
    if (reg->Address == 0) return -1;

    DLOG("[ACPI] Reset via RESET_REG (space=%u addr=0x%lx value=%u)\n",
         reg->AddressSpaceId, (uint64_t)reg->Address, fadt->ResetValue);

    if (reg->AddressSpaceId == ACPI_GAS_SYSTEM_IO) {
        outb((uint16_t)reg->Address, fadt->ResetValue);
    } else if (reg->AddressSpaceId == ACPI_GAS_SYSTEM_MEMORY) {
        *(volatile uint8_t *)(uint64_t)reg->Address = fadt->ResetValue;
    } else {
        // PCI config space и прочие варианты спецификация допускает, но
        // почти никто их не использует — отказываем чисто, пусть вызывающий
        // откатится на другой метод, вместо угадывания нестандартного доступа.
        return -1;
    }

    // Если дошли сюда — сброса не произошло (при успехе мы бы сюда уже не
    // вернулись). Даём немного времени на случай срабатывания с задержкой,
    // затем сообщаем вызывающему, что нужно пробовать другой метод.
    for (volatile int i = 0; i < 1000000; i++);
    return -1;
}