; stage1.asm — код загрузочного сектора ESP (FAT12 "суперфлоппи", без
; таблицы разделов — тот же disk.img, который UEFI грузит напрямую, см.
; 01_bootloader.md и LufiraOS-Builder/lufira_builder/image.py).
;
; Это НЕ весь сектор 0: mkfs.fat кладёт корректный FAT12 BPB (байты
; 0x00..0x3D, включая обязательный "jmp short 0x3E; nop") — он не
; генерируется отсюда и не трогается. Сборщик (lufira_builder/
; legacy_boot.py) берёт из этого файла только байты код-региона (448 байт
; после ассемблирования) и патчит ими diskimg[0x3E:0x1FE], оставляя BPB и
; сигнатуру 0x55AA как есть. org 0x7c3e — физический адрес этого кода
; после загрузки сектора 0 в 0x7C00 (0x7C00+0x3E).
;
; Задача stage1 — вслепую прочитать STAGE2_MAX_SECTORS секторов начиная
; с STAGE2_LBA_START через INT13h extensions (LBA, без CHS-фоллбэка — как
; и UEFI-путь работает поверх LBA) и передать туда управление. Файловой
; системы тут нет и не будет, как и для kernel.bin/FAT-образа в stage2
; (см. layout.inc).

%include "layout.inc"

BITS 16
ORG 0x7c3e

start:
    ; BIOS мог войти как 0000:7C3E или 07C0:003E — far jump нормализует
    ; CS=0 независимо от входа (дальше код предполагает CS=DS=ES=SS=0).
    jmp 0x0000:normalized

normalized:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7b00          ; растёт вниз, подальше от самого себя (0x7c3e+)
    sti

    mov [boot_drive], dl    ; BIOS передаёт номер загрузочного диска в DL

    mov si, msg_boot
    call print_str

    ; Проверяем INT13h extensions (без них LBA-чтение невозможно — а
    ; CHS-фоллбэка здесь принципиально нет, см. комментарий выше).
    mov ah, 0x41
    mov bx, 0x55aa
    mov dl, [boot_drive]
    int 0x13
    jc .no_ext
    cmp bx, 0xaa55
    jne .no_ext
    jmp short .ext_ok
.no_ext:
    mov si, msg_no_ext
    call print_str
    jmp halt
.ext_ok:

    ; Читаем stage2 целиком через INT13h AH=42h (extended read) в
    ; STAGE2_LOAD_SEGMENT:0x0000 (физически 0x10000).
    mov word [dap_count], STAGE2_MAX_SECTORS
    mov word [dap_offset], 0x0000
    mov word [dap_segment], STAGE2_LOAD_SEGMENT
    mov dword [dap_lba_lo], STAGE2_LBA_START
    mov dword [dap_lba_hi], 0

    mov si, dap
    mov ah, 0x42
    mov dl, [boot_drive]
    int 0x13
    jc .read_fail
    jmp stage2_entry_far

.read_fail:
    mov si, msg_fail
    call print_str
    jmp halt

; ---- печать ASCIIZ-строки из DS:SI через INT10h teletype (AH=0x0E) ----
print_str:
    pusha
.loop:
    lodsb
    test al, al
    jz .done
    mov ah, 0x0e
    mov bx, 0x0007
    int 0x10
    jmp .loop
.done:
    popa
    ret

halt:
    cli
    hlt
    jmp halt

stage2_entry_far:
    jmp STAGE2_LOAD_SEGMENT:0x0000

boot_drive: db 0

; Disk Address Packet (INT13h AH=42h/43h), см. layout этой структуры в
; любом справочнике по INT13h extensions — 16 байт, little-endian.
align 4
dap:
    db 0x10                 ; размер пакета
    db 0
dap_count:    dw 0
dap_offset:   dw 0
dap_segment:  dw 0
dap_lba_lo:   dd 0
dap_lba_hi:   dd 0

msg_boot:   db "LufiraOS: legacy BIOS boot", 13, 10, 0
msg_no_ext: db "LufiraOS: INT13 extensions not supported", 13, 10, 0
msg_fail:   db "LufiraOS: stage2 read failed", 13, 10, 0

; Код обязан уместиться в 448 байт (0x3E..0x1FD) — сборщик сам проверит
; итоговый размер stage1.bin и упадёт с понятной ошибкой, если превышен.
