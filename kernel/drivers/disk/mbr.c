#include "mbr.h"
#include "disk.h"
#include "lib/string.h"

#define MBR_PARTITION_TABLE_OFFSET 0x1BE
#define MBR_PARTITION_ENTRY_SIZE   16
#define MBR_PARTITION_COUNT        4
#define MBR_SIGNATURE_OFFSET       0x1FE

int mbr_find_partition(uint8_t type, uint32_t *out_start_lba) {
    if (!out_start_lba) return -1;

    uint8_t sector[512];
    if (disk_read_sectors(0, 1, sector) != 0) return -1;
    if (sector[MBR_SIGNATURE_OFFSET] != 0x55 || sector[MBR_SIGNATURE_OFFSET + 1] != 0xAA) return -1;

    for (int i = 0; i < MBR_PARTITION_COUNT; i++) {
        const uint8_t *entry = sector + MBR_PARTITION_TABLE_OFFSET + i * MBR_PARTITION_ENTRY_SIZE;
        if (entry[4] != type) continue;

        // LBA little-endian в байтах 8..11 — совпадает с representation
        // на x86-64, прямой memcpy без свопа байт.
        memcpy(out_start_lba, entry + 8, sizeof(*out_start_lba));
        return 0;
    }
    return -1;
}
