; layout.inc — константы дискового layout'а, общие для stage1.asm и
; stage2.asm. ЗЕРКАЛЬНО повторяются в lufira_builder/legacy_boot.py
; (LufiraOS-Builder) при патче образа — расхождение здесь и там значит,
; что stage1/stage2 будут читать не те сектора. Два источника правды вместо
; одного — слабое место, но общего заголовка между NASM и Python не
; сделать; оба места явно ссылаются друг на друга в комментариях.
;
; Разметка ESP (первые cfg.esp_size байт disk.img, см. LufiraOS-Builder/
; lufira_builder/config.py):
;   LBA 0           — загрузочный сектор FAT12 (BPB цел, байты 0x3E..0x1FD
;                      пропатчены нашим STAGE1_CODE из этого же образа).
;   LBA 1..64       — stage2.bin (до STAGE2_MAX_SECTORS*512 байт) — real
;                      mode: A20/unreal/E820/VESA/диск, затем
;                      protected/long mode transition.
;   LBA 65..96      — legacy_main.bin (до LEGACY_MAIN_MAX_SECTORS*512
;                      байт) — скомпилированный как обычный freestanding
;                      x86-64 C, собирается И грузится как плоский
;                      бинарник точно так же, как kernel.bin ниже (та же
;                      objcopy -O binary схема) — а не линкуется с
;                      stage2.asm напрямую: 16-битный real-mode код
;                      stage2 адресует себя сегмент-относительно (org 0,
;                      CS=STAGE2_LOAD_SEGMENT), что плохо сочетается с
;                      обычным ELF-линковщиком, рассчитывающим на плоские
;                      линейные адреса — поэтому legacy_main собирается
;                      СОВЕРШЕННО НЕЗАВИСИМО и грузится stage2 отдельным
;                      INT13-чтением (см. ниже), как ещё один "kernel.bin".
;   LBA 97          — заголовок ядра: 4 байта magic 'LFKR' + 4 байта
;                      little-endian размер в байтах, затем сразу raw
;                      kernel.bin.
;   LBA 98..4191    — сам kernel.bin (до KERNEL_MAX_SECTORS*512 байт).
;   LBA 4192+       — обычная FAT12-область (FAT-таблицы/root dir/данные:
;                      EFI/BOOT/BOOTX64.EFI, /kernel.bin — для UEFI-пути).
;
; Reserved sector count в BPB (mkfs.fat -R) ДОЛЖЕН быть >= RESERVED_SECTORS
; ниже — иначе эти LBA окажутся внутри настоящей FAT-области и образ будет
; испорчен при следующей сборке/установке пакетов.

STAGE2_LOAD_SEGMENT equ 0x1000      ; физический адрес 0x10000
STAGE2_LBA_START    equ 1
STAGE2_MAX_SECTORS  equ 64          ; 32 KiB — бюджет под stage2.bin

LEGACY_MAIN_LBA          equ (STAGE2_LBA_START + STAGE2_MAX_SECTORS) ; 65
LEGACY_MAIN_MAX_SECTORS  equ 32                                      ; 16 KiB
LEGACY_MAIN_LOAD_ADDR    equ 0x00090000 ; <1MB — обычное INT13, unreal не нужен

KERNEL_HEADER_LBA    equ (LEGACY_MAIN_LBA + LEGACY_MAIN_MAX_SECTORS) ; 97
KERNEL_MAGIC         equ 0x524B464C ; 'LFKR' little-endian в памяти
KERNEL_MAX_SECTORS   equ 4094
RESERVED_SECTORS     equ (KERNEL_HEADER_LBA + 1 + KERNEL_MAX_SECTORS) ; 4192

; Физические адреса временных буферов/таблиц реал-мод стадии stage2 —
; подобраны так, чтобы не пересекаться друг с другом и с самим stage2.bin
; (0x10000..0x18000, см. STAGE2_LOAD_SEGMENT/STAGE2_MAX_SECTORS):
LOW_SCRATCH_ADDR      equ 0x00020000 ; буфер под чтение чанков с диска
LOW_SCRATCH_SIZE      equ 0x00010000 ; 64 KiB
LOW_SCRATCH_SEGMENT   equ 0x2000
; 64 сектора (32 KiB) за один INT13-вызов. Реальная причина зависания,
; найденная живой отладкой под QEMU/SeaBIOS, была не в размере чанка, а в
; том, что BIOS не обязан сохранять ES через int 0x13 — "unreal"-окно на
; ES (см. reenter_unreal_es в stage2.asm) нужно поднимать заново на каждой
; итерации load_and_relocate, не один раз снаружи цикла. После этой
; починки размер чанка снова можно выбирать свободно — каждый INT13-вызов
; стоит заметных миллисекунд под QEMU/SeaBIOS PIO независимо от размера,
; так что меньше вызовов = заметно быстрее загрузка.
; 128 секторов (64 KiB = весь LOW_SCRATCH_SIZE) за один INT13-вызов —
; максимум без переполнения буфера чанков. Минимизирует число BIOS-вызовов
; (каждый стоит заметных миллисекунд под QEMU/SeaBIOS PIO независимо от
; размера). Нереальная причина зависания, найденная живой отладкой, была не
; в размере чанка — ES не держит "unreal" через int 0x13 (см.
; reenter_unreal_es в stage2.asm, вызывается заново на каждой итерации).
CHUNK_SECTORS         equ 128

E820_BUFFER_ADDR      equ 0x00040000 ; до ~2KB, ~87 записей по 24 байта
VESA_INFO_ADDR        equ 0x00041000 ; ControllerInfoBlock (512) + ModeInfoBlock (256)
VESA_INFO_SEGMENT     equ 0x4100

PAGETABLES_BASE       equ 0x00070000 ; PML4+PDPT+4×PD, 6×4KiB = 24 KiB подряд
HANDOFF_STRUCT_ADDR   equ 0x00076000 ; LegacyHandoff — см. legacy_boot.h

; Смещения полей LegacyHandoff (должны совпадать со _Static_assert-ом в
; legacy_boot.h — см. комментарий там же).
HANDOFF_OFF_E820_ADDR      equ 0
HANDOFF_OFF_E820_COUNT     equ 8
HANDOFF_OFF_FB_BASE        equ 12
HANDOFF_OFF_FB_WIDTH       equ 20
HANDOFF_OFF_FB_HEIGHT      equ 24
HANDOFF_OFF_FB_PITCH       equ 28
HANDOFF_OFF_FB_BPP         equ 32
HANDOFF_OFF_FB_RED_FIELD   equ 36
HANDOFF_OFF_KERNEL_BASE    equ 40
HANDOFF_OFF_KERNEL_SIZE    equ 48
HANDOFF_OFF_FATIMAGE_BASE  equ 56
HANDOFF_OFF_FATIMAGE_SIZE  equ 64
HANDOFF_STRUCT_SIZE        equ 72

; Физические адреса, куда stage2 грузит данные через unreal mode (см.
; подробный комментарий в stage2.asm про unreal mode). KERNEL_LOAD_ADDR
; обязан совпадать с ORIGIN в kernel/linker.ld (0x100000) — тем же
; значением, что KernelBase у UEFI-пути (boot/loaders/kernel_loader.c).
KERNEL_LOAD_ADDR     equ 0x00100000

; Куда грузим "FAT image" (весь диск целиком, см. boot/loaders/
; fat_loader.c) — тот же смысл, что bi->FATImageBase. Выбран с запасом
; выше ядра (которое максимум ~16 MiB по текущему KERNEL_MAX_SECTORS) и
; ниже типичного потолка RAM тестовых VM (256 MiB).
FATIMAGE_LOAD_ADDR   equ 0x02000000
FATIMAGE_MAX_BYTES   equ (256 * 1024 * 1024)
