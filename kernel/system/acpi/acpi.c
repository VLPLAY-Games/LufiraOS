#include "acpi.h"
#include "drivers/console/console.h"
#include "lib/stddef.h"

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
    
    printf("[ACPI] RSDP found, Revision %d\n", rsdp->rev1.Revision);
    
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
        printf("[ACPI] Using XSDT at 0x%lx\n", (uint64_t)rsdp->XsdtAddress);
    } else {
        use_64bit = 0;
        sdt = (void*)(uint64_t)rsdp->rev1.RsdtAddress;
        ACPI_SDTHeader *header = (ACPI_SDTHeader*)sdt;
        
        if (acpi_checksum(header, header->Length) != 0) {
            printf("[ACPI] RSDT checksum failed\n");
            return -1;
        }
        
        sdt_entries = (header->Length - sizeof(ACPI_SDTHeader)) / 4;
        printf("[ACPI] Using RSDT at 0x%x\n", rsdp->rev1.RsdtAddress);
    }
    
    printf("[ACPI] %u SDT entries found\n", sdt_entries);
    
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
    
    printf("[ACPI] FADT found at 0x%lx\n", (uint64_t)fadt);
    printf("[ACPI] SMI_CMD: 0x%x\n", fadt->SMI_CMD);
    printf("[ACPI] ACPI_ENABLE: 0x%x\n", fadt->ACPI_ENABLE);
    printf("[ACPI] PM1a_CNT_BLK: 0x%x\n", fadt->PM1a_CNT_BLK);
    
    // Включаем ACPI режим, если нужно
    if (fadt->SMI_CMD != 0 && fadt->ACPI_ENABLE != 0) {
        printf("[ACPI] Enabling ACPI mode...\n");
        
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
                    printf("[ACPI] ACPI mode enabled\n");
                } else {
                    printf("[ACPI] WARNING: Failed to enable ACPI mode (timeout)\n");
                    // Продолжаем - может, всё равно сработает
                }
            } else {
                printf("[ACPI] ACPI already enabled\n");
            }
        }
    }
    
    acpi_ready = 1;
    printf("[ACPI] Initialization complete\n");
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
    }
    
    printf("[ACPI] Shutting down via ACPI...\n");
    
    // Стандартное SLP_TYPa для S5 обычно 0
    uint16_t slp_typa = 0;  // Можно также прочитать из _S5 объекта DSDT
    
    // Формируем значение для порта: SLP_TYPa | SLP_EN
    uint16_t pm1a_value = slp_typa | ACPI_SLP_EN;
    
    printf("[ACPI] Writing 0x%x to PM1a_CNT (0x%x)\n", 
           pm1a_value, fadt->PM1a_CNT_BLK);
    
    if (fadt->PM1a_CNT_BLK != 0) {
        // Читаем текущее значение, чтобы сохранить биты
        uint16_t current = inw(fadt->PM1a_CNT_BLK);
        current &= ~0x1FFF;  // Очищаем SLP_TYPa и SLP_EN
        current |= pm1a_value;
        outw(fadt->PM1a_CNT_BLK, current);
    }
    
    if (fadt->PM1b_CNT_BLK != 0) {
        uint16_t current = inw(fadt->PM1b_CNT_BLK);
        current &= ~0x1FFF;
        current |= pm1a_value;
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