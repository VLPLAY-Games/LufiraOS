; stage2.asm — вторая стадия legacy BIOS загрузчика. Грузится stage1
; целиком в STAGE2_LOAD_SEGMENT:0x0000 и получает управление через far
; jump (CS=STAGE2_LOAD_SEGMENT, IP=0) — файл ассемблируется с ORG 0,
; метки относительны начала сегмента (стандартная схема для boot-кода).
;
; Правило по всему файлу: собственные данные (строки, переменные, DAP,
; GDT) адресуются только через префикс cs:, никогда через голый DS — DS
; постоянно переключается между сегментами (LOW_SCRATCH_SEGMENT,
; VESA_INFO_SEGMENT, таблица режимов VESA), полагаться на "DS сейчас как
; раз CS" нельзя. ES отдельно зарезервирован под "unreal" плоское окно и
; трогается только через reenter_unreal_es.
;
; Общий план (подробности — в комментариях разделов):
;   1. Реальный режим: сегменты/стек, статусные строки через INT10h.
;   2. A20 (быстрый метод, порт 0x92).
;   3. "Unreal mode" для ES — нужен, чтобы писать в физическую память
;      выше 1 MiB из 16-битного кода без постоянного входа в protected
;      mode (INT13 на это не способен: DAP адресует только segment:offset,
;      т.е. до ~1 MiB+64 KiB — поэтому диск читаем чанками в
;      LOW_SCRATCH_ADDR и копируем оттуда на физический адрес через
;      ES-unreal).
;   4. E820 и VESA VBE — те же данные, что UEFI берёт из GOP/
;      GetMemoryMap(), но через BIOS.
;   5. Грузим legacy_main.bin (<1MB, обычный INT13), kernel.bin (unreal-
;      копирование на 0x100000) и весь диск (на FATIMAGE_LOAD_ADDR).
;   6. Переход real → protected (32-бит) → long mode (identity-карта
;      0-4GB 2MB-страницами — покрывает RAM и 32-битный VESA LFB).
;   7. Прыжок на legacy_main.bin (RDI = указатель на LegacyHandoff).
;
; Ядро не знает про этот файл — задача здесь в том, чтобы к вызову
; kernel_entry(BootInfo*) окружение было неотличимо от того, что готовит
; boot/ (UEFI-путь).

%include "layout.inc"

BITS 16
ORG 0

stage2_entry:
    mov ax, cs
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0xF000           ; стек растёт вниз от конца собственного сегмента
    mov [cs:boot_drive], dl  ; DL с самого входа BIOS — stage1 передаёт
                              ; управление, не трогая DL между своим
                              ; чтением диска и дальним прыжком сюда
    sti

    mov si, msg_stage2
    call print_str

    ; ---- A20 (быстрый метод через порт 0x92; почти везде работает, в
    ; т.ч. во всех актуальных эмуляторах) ----
    in al, 0x92
    test al, 2
    jnz .a20_done
    or al, 2
    and al, 0xFE              ; бит0 — INIT_NOW на некоторых чипсетах, не трогаем
    out 0x92, al
.a20_done:

    ; ---- Загружаем legacy_main.bin (<1MB, обычный INT13, без unreal) ----
    mov si, msg_main
    call print_str
    mov word [cs:dap_count], LEGACY_MAIN_MAX_SECTORS
    mov word [cs:dap_offset], 0x0000
    mov word [cs:dap_segment], (LEGACY_MAIN_LOAD_ADDR >> 4)
    mov dword [cs:dap_lba_lo], LEGACY_MAIN_LBA
    mov dword [cs:dap_lba_hi], 0
    call disk_read_ext
    jc fatal_disk

    ; ---- Memory map (E820) ----
    mov si, msg_mmap
    call print_str
    call do_e820

    ; ---- VESA VBE: линейный framebuffer ----
    mov si, msg_gfx
    call print_str
    call do_vesa

    ; ---- Поднимаем "unreal" окно на ES один раз — держится до следующей
    ; обычной загрузки ES числом (см. reenter_unreal_es, вызывается заново
    ; везде, где BIOS-вызов между делом мог тронуть ES). ----
    call reenter_unreal_es

    ; ================= Ядро =================
    mov si, msg_kernel
    call print_str

    mov ax, LOW_SCRATCH_SEGMENT
    mov ds, ax
    mov word [cs:dap_count], 1
    mov word [cs:dap_offset], 0x0000
    mov word [cs:dap_segment], LOW_SCRATCH_SEGMENT
    mov dword [cs:dap_lba_lo], KERNEL_HEADER_LBA
    mov dword [cs:dap_lba_hi], 0
    call disk_read_ext
    jc fatal_disk

    mov eax, [0x0000]          ; DS=LOW_SCRATCH_SEGMENT:0 — magic
    cmp eax, KERNEL_MAGIC
    jne fatal_kernel_magic
    mov eax, [0x0004]          ; размер ядра в байтах
    mov [cs:kernel_size_var], eax

    xor edx, edx
    mov ebx, 512
    div ebx                     ; eax = целых секторов, edx = остаток
    test edx, edx
    jz .no_round
    inc eax
.no_round:
    mov [cs:kernel_sectors_var], eax

    mov esi, KERNEL_HEADER_LBA + 1   ; LBA первого сектора данных ядра
    mov edi, KERNEL_LOAD_ADDR
    mov ecx, [cs:kernel_sectors_var]
    call load_and_relocate

    ; ================= Весь диск (FAT image) =================
    mov si, msg_fatimg
    call print_str

    mov ax, LOW_SCRATCH_SEGMENT
    mov ds, ax
    xor ax, ax
    mov cx, 16
    xor di, di
.clear_edd_buf:                   ; буфер дочиста — BIOS может не заполнить
    mov [di], ax                  ; все поля (особенно за пределами базовой
    add di, 2                     ; EDD 1.0 структуры), оставляя мусор от
    loop .clear_edd_buf           ; предыдущего использования этого же буфера
    mov word [0x0000], 0x1A      ; EDD 1.0 — 26 байт, универсально поддерживается
                                   ; (0x1E/30 запрашивает EDD2.x device path,
                                   ; который не все BIOS реализуют корректно)
    mov dl, [cs:boot_drive]
    mov si, 0x0000
    mov ah, 0x48
    int 0x13
    jc .fatimg_size_fail
    mov eax, [0x0010]            ; offset 0x10 буфера = младшие 32 бита total sectors
    test eax, eax
    jz .fatimg_size_fail          ; 0 — BIOS ничего не сообщил, не доверяем
    jmp .fatimg_size_ok
.fatimg_size_fail:
    mov eax, 65536                ; разумный дефолт (32MB), если BIOS не ответил
.fatimg_size_ok:
    mov ebx, (FATIMAGE_MAX_BYTES / 512)
    cmp eax, ebx
    jbe .fatimg_cap_ok
    mov eax, ebx
.fatimg_cap_ok:
    mov [cs:fatimage_sectors_var], eax
    mov [cs:fatimage_bytes_var], eax
    shl dword [cs:fatimage_bytes_var], 9   ; *512

    xor esi, esi                  ; LBA 0 — весь диск с самого начала
    mov edi, FATIMAGE_LOAD_ADDR
    mov ecx, [cs:fatimage_sectors_var]
    call load_and_relocate

    ; ================= handoff: kernel/fatimage base+size =================
    ; (E820/VESA поля уже записаны внутри do_e820/do_vesa)
    call reenter_unreal_es
    mov edi, HANDOFF_STRUCT_ADDR + HANDOFF_OFF_KERNEL_BASE
    mov dword [es:edi], KERNEL_LOAD_ADDR
    mov dword [es:edi+4], 0
    mov edi, HANDOFF_STRUCT_ADDR + HANDOFF_OFF_KERNEL_SIZE
    mov eax, [cs:kernel_size_var]
    mov dword [es:edi], eax
    mov dword [es:edi+4], 0
    mov edi, HANDOFF_STRUCT_ADDR + HANDOFF_OFF_FATIMAGE_BASE
    mov dword [es:edi], FATIMAGE_LOAD_ADDR
    mov dword [es:edi+4], 0
    mov edi, HANDOFF_STRUCT_ADDR + HANDOFF_OFF_FATIMAGE_SIZE
    mov eax, [cs:fatimage_bytes_var]
    mov dword [es:edi], eax
    mov dword [es:edi+4], 0

    mov si, msg_starting
    call print_str

    jmp enter_protected_mode

; ============================================================
; load_and_relocate(ESI=начальный LBA, EDI=физический адрес назначения,
; ECX=число секторов) — читает чанками по CHUNK_SECTORS в LOW_SCRATCH_ADDR
; (обычный INT13, DS=LOW_SCRATCH_SEGMENT) и копирует каждый чанк на ES:EDI
; (unreal, уже поднят вызывающим). Параметры/прогресс хранятся в
; фиксированных cs:-переменных, а не на стеке — меньше риска запутаться в
; push/pop на 16-битном стеке вперемешку с 32-битными регистрами.
; ============================================================
load_and_relocate:
    mov [cs:lr_lba], esi
    mov [cs:lr_dest], edi
    mov [cs:lr_remaining], ecx
.loop:
    cmp dword [cs:lr_remaining], 0
    je .done

    mov eax, CHUNK_SECTORS
    cmp dword [cs:lr_remaining], eax
    jae .chunk_sized
    mov eax, [cs:lr_remaining]
.chunk_sized:
    mov [cs:lr_chunk], eax

    mov ax, LOW_SCRATCH_SEGMENT
    mov ds, ax
    mov ax, [cs:lr_chunk]
    mov [cs:dap_count], ax
    mov word [cs:dap_offset], 0x0000
    mov word [cs:dap_segment], LOW_SCRATCH_SEGMENT
    mov eax, [cs:lr_lba]
    mov [cs:dap_lba_lo], eax
    mov dword [cs:dap_lba_hi], 0
    call disk_read_ext
    jc fatal_disk

    ; BIOS не обязан сохранять ES через int 0x13 (AH=42h сам его не
    ; использует — DAP кодирует segment:offset внутри себя, — но
    ; конкретная реализация вправе трогать ES как временный регистр).
    ; Поэтому "unreal" окно на ES поднимается заново на каждой итерации,
    ; после disk_read_ext, а не один раз снаружи цикла.
    call reenter_unreal_es

    ; копируем lr_chunk*512 байт из LOW_SCRATCH_SEGMENT:0 на ES:[lr_dest]
    mov eax, [cs:lr_chunk]
    shl eax, 9
    mov [cs:lr_chunk_bytes], eax

    mov ax, LOW_SCRATCH_SEGMENT
    mov ds, ax
    xor esi, esi
    mov edi, [cs:lr_dest]
    mov ecx, [cs:lr_chunk_bytes]
    cld
    a32 rep movsb                ; DS:ESI(unreal нет, но в пределах 64KB) -> ES:EDI(unreal)
    ; a32 обязателен: REP MOVSB не принимает регистры как операнды, поэтому
    ; NASM не может определить разрядность адресации из самой мнемоники (в
    ; отличие от "mov eax, [esi]") и без префикса кодирует 16-битные
    ; CX/SI/DI. Без a32 копирование шло бы по DI (младшие 16 бит EDI) —
    ; для EDI=0x00100000 это физический 0x00000000, т.е. запись поверх IVT.

    mov eax, [cs:lr_chunk]
    add [cs:lr_lba], eax
    mov eax, [cs:lr_chunk_bytes]
    add [cs:lr_dest], eax
    mov eax, [cs:lr_chunk]
    sub [cs:lr_remaining], eax
    jmp .loop
.done:
    ret

lr_lba:          dd 0
lr_dest:         dd 0
lr_remaining:    dd 0
lr_chunk:        dd 0
lr_chunk_bytes:  dd 0

; ============================================================
; disk_read_ext — INT13h AH=42h по текущему DAP (cs:dap_*), DL=boot_drive.
; CF возвращается как есть (ошибка — CF=1). SI/DS на входе не важны —
; явно берём DAP через cs:.
; ============================================================
disk_read_ext:
    push ax
    push dx
    push si
    push ds
    push cs
    pop ds
    mov si, dap
    mov ah, 0x42
    mov dl, [cs:boot_drive]
    int 0x13
    pop ds
    pop si
    pop dx
    pop ax
    ret

; ============================================================
; do_e820 — заполняет E820_BUFFER_ADDR сырыми записями (по 24 байта
; каждая, ES:DI на буфер в обычном реальном режиме) и handoff.e820_addr/
; e820_count через unreal ES в самом конце.
; ============================================================
do_e820:
    push es
    push di
    push bx
    push cx
    push dx
    push si
    push ebp

    mov ax, (E820_BUFFER_ADDR >> 4)
    mov es, ax
    xor di, di
    xor ebx, ebx
    xor ebp, ebp
.loop:
    mov eax, 0xE820
    mov ecx, 24
    mov edx, 0x534D4150
    int 0x15
    jc .done
    cmp eax, 0x534D4150
    jne .done
    inc ebp
    add di, 24
    test ebx, ebx
    jz .done
    cmp di, 0xF000                ; защита от переполнения буфера
    jae .done
    jmp .loop
.done:
    mov [cs:e820_count_var], ebp

    call reenter_unreal_es
    mov edi, HANDOFF_STRUCT_ADDR + HANDOFF_OFF_E820_ADDR
    mov dword [es:edi], E820_BUFFER_ADDR
    mov dword [es:edi+4], 0
    mov edi, HANDOFF_STRUCT_ADDR + HANDOFF_OFF_E820_COUNT
    mov eax, [cs:e820_count_var]
    mov dword [es:edi], eax

    pop ebp
    pop si
    pop dx
    pop cx
    pop bx
    pop di
    pop es
    ret

e820_count_var: dd 0

; ============================================================
; reenter_unreal_es — входит в protected mode ровно на время загрузки в ES
; селектора с 4GB лимитом (DATA32_SEL), затем возвращается в real mode —
; кэшированные база/лимит у ES остаются в силе ("unreal mode" трюк), пока
; ES не перезагрузят обычным 16-битным значением. Вызывать заново после
; любого BIOS-вызова, которому отдавали ES под что-то другое (VESA/E820).
; ============================================================
reenter_unreal_es:
    push eax
    push bx
    push ds
    cli
    lgdt [cs:gdt_descriptor]
    mov eax, cr0
    or al, 1
    mov cr0, eax
    jmp short $+2
    mov bx, DATA32_SEL
    mov es, bx
    mov eax, cr0
    and al, 0xFE
    mov cr0, eax
    sti
    pop ds
    pop bx
    pop eax
    ret

; ============================================================
; do_vesa — ищет линейный 32bpp режим (приоритет 1024x768, 800x600,
; 640x480, иначе первый подходящий любого разрешения), устанавливает его,
; пишет framebuffer-поля в handoff.
; ============================================================
do_vesa:
    push es
    push di
    push si
    push ax
    push bx
    push cx
    push dx
    push ds

    mov ax, VESA_INFO_SEGMENT
    mov es, ax
    xor di, di
    mov dword [es:di], 0x32454256    ; "VBE2" little-endian — просим VBE2.0+ поля
    mov ax, 0x4F00
    int 0x10
    cmp ax, 0x004F
    jne .fail

    mov ax, [es:di+0x0E]              ; VideoModePtr offset
    mov [cs:mode_list_off], ax
    mov ax, [es:di+0x10]              ; VideoModePtr segment
    mov [cs:mode_list_seg], ax

    mov word [cs:best_mode], 0xFFFF
    mov byte [cs:target_pass], 0
.pass_loop:
    mov ax, [cs:mode_list_seg]
    mov ds, ax
    mov si, [cs:mode_list_off]
.mode_loop:
    lodsw
    cmp ax, 0xFFFF
    je .pass_done
    mov [cs:cur_mode], ax

    push es
    push di
    mov di, VESA_INFO_SEGMENT
    mov es, di
    mov di, 0x0200                   ; ModeInfoBlock после ControllerInfo (0x0000-0x01FF)
    mov cx, [cs:cur_mode]
    mov ax, 0x4F01
    int 0x10
    cmp ax, 0x004F
    jne .mode_skip

    ; ModeAttributes: требуем bit0(support)+bit3(color)+bit4(graphics)+bit7(LFB)
    mov ax, [es:di+0x00]
    and ax, 0x0099
    cmp ax, 0x0099
    jne .mode_skip
    mov al, [es:di+0x1B]              ; MemoryModel
    cmp al, 6                          ; Direct Color
    jne .mode_skip
    mov al, [es:di+0x19]              ; BitsPerPixel
    cmp al, 32
    jne .mode_skip

    mov ax, [es:di+0x12]               ; XResolution
    mov bx, [es:di+0x14]               ; YResolution
    mov cl, [cs:target_pass]
    cmp cl, 3
    je .mode_candidate
    cmp cl, 0
    jne .chk1
    cmp ax, 1024
    jne .mode_skip
    cmp bx, 768
    jne .mode_skip
    jmp .mode_candidate
.chk1:
    cmp cl, 1
    jne .chk2
    cmp ax, 800
    jne .mode_skip
    cmp bx, 600
    jne .mode_skip
    jmp .mode_candidate
.chk2:
    cmp ax, 640
    jne .mode_skip
    cmp bx, 480
    jne .mode_skip

.mode_candidate:
    mov ax, [cs:cur_mode]
    mov [cs:best_mode], ax
    mov eax, [es:di+0x28]               ; PhysBasePtr
    mov [cs:best_physbase], eax
    mov ax, [es:di+0x10]                 ; BytesPerScanLine
    mov [cs:best_pitch], ax
    mov ax, [es:di+0x12]
    mov [cs:best_xres], ax
    mov ax, [es:di+0x14]
    mov [cs:best_yres], ax
    ; +0x1F в ModeInfoBlock — это RedMaskSize, не RedFieldPosition (тот на
    ; +0x20, байтом дальше) — не путать смещения, от этого зависит
    ; BootInfo.PixelFormat ("== 16") в legacy_main.c.
    mov al, [es:di+0x20]                  ; RedFieldPosition
    mov [cs:best_redpos], al

.mode_skip:
    pop di
    pop es
    jmp .mode_loop
.pass_done:
    cmp word [cs:best_mode], 0xFFFF
    jne .found
    inc byte [cs:target_pass]
    cmp byte [cs:target_pass], 4
    jb .pass_loop
    jmp .fail

.found:
    mov bx, [cs:best_mode]
    or bx, 0x4000                         ; бит14 = использовать линейный framebuffer
    mov ax, 0x4F02
    int 0x10
    cmp ax, 0x004F
    jne .fail

    call reenter_unreal_es
    mov edi, HANDOFF_STRUCT_ADDR + HANDOFF_OFF_FB_BASE
    mov eax, [cs:best_physbase]
    mov dword [es:edi], eax
    mov dword [es:edi+4], 0
    mov edi, HANDOFF_STRUCT_ADDR + HANDOFF_OFF_FB_WIDTH
    movzx eax, word [cs:best_xres]
    mov dword [es:edi], eax
    mov edi, HANDOFF_STRUCT_ADDR + HANDOFF_OFF_FB_HEIGHT
    movzx eax, word [cs:best_yres]
    mov dword [es:edi], eax
    mov edi, HANDOFF_STRUCT_ADDR + HANDOFF_OFF_FB_PITCH
    movzx eax, word [cs:best_pitch]
    mov dword [es:edi], eax
    mov edi, HANDOFF_STRUCT_ADDR + HANDOFF_OFF_FB_BPP
    mov dword [es:edi], 32
    mov edi, HANDOFF_STRUCT_ADDR + HANDOFF_OFF_FB_RED_FIELD
    movzx eax, byte [cs:best_redpos]
    mov dword [es:edi], eax
    jmp .out
.fail:
    mov si, msg_vesa_fail
    call print_str
.out:
    pop ds
    pop dx
    pop cx
    pop bx
    pop ax
    pop si
    pop di
    pop es
    ret

mode_list_off: dw 0
mode_list_seg: dw 0
cur_mode:      dw 0
target_pass:   db 0
best_mode:     dw 0
best_physbase: dd 0
best_pitch:    dw 0
best_xres:     dw 0
best_yres:     dw 0
best_redpos:   db 0

; ============================================================
; print_str — [cs:si] -> ASCIIZ, INT10h teletype. Вызывающий передаёт SI
; как смещение В ЭТОМ ЖЕ сегменте (все msg_* тут же); DS принудительно
; переключается на CS на время печати и восстанавливается.
; ============================================================
print_str:
    pusha
    push ds
    push cs
    pop ds
.loop:
    lodsb
    test al, al
    jz .done
    mov ah, 0x0e
    mov bx, 0x0007
    int 0x10
    jmp .loop
.done:
    pop ds
    popa
    ret

fatal_disk:
    mov si, msg_disk_fail
    call print_str
    jmp halt
fatal_kernel_magic:
    mov si, msg_bad_magic
    call print_str
    jmp halt
halt:
    cli
    hlt
    jmp halt

boot_drive: db 0

align 4
dap:
    db 0x10
    db 0
dap_count:    dw 0
dap_offset:   dw 0
dap_segment:  dw 0
dap_lba_lo:   dd 0
dap_lba_hi:   dd 0

kernel_size_var:      dd 0
kernel_sectors_var:   dd 0
fatimage_sectors_var: dd 0
fatimage_bytes_var:   dd 0

msg_stage2:    db "Stage2: entered, enabling A20...", 13, 10, 0
msg_main:      db "Loading legacy_main...", 13, 10, 0
msg_mmap:      db "Reading memory map...", 13, 10, 0
msg_gfx:       db "Initializing graphics...", 13, 10, 0
msg_kernel:    db "Loading kernel...", 13, 10, 0
msg_fatimg:    db "Loading FAT image...", 13, 10, 0
msg_starting:  db "Starting kernel...", 13, 10, 0
msg_disk_fail: db "Disk read failed!", 13, 10, 0
msg_bad_magic: db "Kernel header corrupt!", 13, 10, 0
msg_vesa_fail: db "No suitable video mode found!", 13, 10, 0

; ============================================================
; GDT — null, data32 (flat, тоже используется для unreal mode и как
; data-сегмент в long mode), code32, code64.
; ============================================================
align 8
gdt_start:
    dq 0
gdt_data32:
    dw 0xFFFF, 0x0000
    db 0x00, 0x92, 0xCF, 0x00
gdt_code32:
    dw 0xFFFF, 0x0000
    db 0x00, 0x9A, 0xCF, 0x00
gdt_code64:
    dw 0xFFFF, 0x0000
    db 0x00, 0x9A, 0xAF, 0x00
gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1
    dd STAGE2_LOAD_SEGMENT * 16 + gdt_start

DATA32_SEL equ (gdt_data32 - gdt_start)
CODE32_SEL equ (gdt_code32 - gdt_start)
CODE64_SEL equ (gdt_code64 - gdt_start)

; ============================================================
; enter_protected_mode — финальный переход real -> protected -> long mode.
; ============================================================
enter_protected_mode:
    cli
    lgdt [cs:gdt_descriptor]
    mov eax, cr0
    or al, 1
    mov cr0, eax
    ; Файл собран с ORG 0 (метки относительны начала сегмента — верно для
    ; 16-битного real-mode кода, где CPU сам добавляет CS*16). Но
    ; CODE32_SEL/CODE64_SEL в GDT ниже — flat-сегменты с base=0: в
    ; protected/long mode EIP интерпретируется как физический адрес
    ; напрямую, без сложения с CS. Поэтому к метке нужно явно прибавлять
    ; STAGE2_PHYS_BASE — физический адрес, по которому реально загружен
    ; stage2 (0x10000).
    jmp dword CODE32_SEL:(protected_mode_entry + STAGE2_PHYS_BASE)

BITS 32
protected_mode_entry:
    mov ax, DATA32_SEL
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x0007FFF0       ; тот же адрес, что потом в long_mode_entry —
                               ; выше PAGETABLES_BASE/HANDOFF, ничего не затирает

    ; ---- Identity-карта 0..4GB 2MB-страницами: PML4[0]->PDPT[0..3]->PD
    ; (x4, по 512 записей 2MB каждая). Строим циклом вместо хранения
    ; 24KB готовых таблиц в файле. ----
    mov edi, PAGETABLES_BASE
    mov ecx, (6 * 4096) / 4
    xor eax, eax
    rep stosd                  ; обнуляем все 6 страниц (PML4+PDPT+4×PD)

    mov dword [PAGETABLES_BASE], (PAGETABLES_BASE + 0x1000) | 0x03   ; PML4[0] -> PDPT

    xor ebx, ebx
.pdpt_loop:
    mov eax, PAGETABLES_BASE + 0x2000
    mov edx, ebx
    shl edx, 12                  ; PD(i) располагается на +0x2000 + i*0x1000
    add eax, edx
    or eax, 0x03
    mov [PAGETABLES_BASE + 0x1000 + ebx*8], eax
    inc ebx
    cmp ebx, 4
    jb .pdpt_loop

    xor ebx, ebx                  ; ebx = глобальный индекс 2MB-страницы (0..2047)
.pd_loop:
    mov eax, ebx
    shl eax, 21                    ; физический адрес = index * 2MB
    or eax, 0x83                   ; PRESENT|WRITE|PS(2MB)
    mov edi, ebx
    shl edi, 3
    add edi, PAGETABLES_BASE + 0x2000
    mov [edi], eax
    inc ebx
    cmp ebx, 2048
    jb .pd_loop

    mov eax, PAGETABLES_BASE
    mov cr3, eax

    mov eax, cr4
    or eax, (1 << 5)                ; PAE
    mov cr4, eax

    ; Вместе с LME (бит 8) обязательно включаем и NXE (бит 11) — ядро
    ; (elf.c, map_page_in_space()) ставит бит NX (63) на страницы данных,
    ; а при EFER.NXE=0 этот бит архитектурно зарезервирован (не NX), и его
    ; установка даёт #PF с RSVD=1 при любом обращении к странице. UEFI-путь
    ; этим не страдает, т.к. прошивка сама включает NXE заранее.
    mov ecx, 0xC0000080              ; EFER
    rdmsr
    or eax, (1 << 8) | (1 << 11)      ; LME | NXE
    wrmsr

    mov eax, cr0
    or eax, (1 << 31)                 ; PG
    mov cr0, eax

    ; CODE64_SEL тоже flat (base=0) — как и выше для protected_mode_entry,
    ; к метке нужно явно прибавлять STAGE2_PHYS_BASE.
    jmp CODE64_SEL:(long_mode_entry + STAGE2_PHYS_BASE)

BITS 64
long_mode_entry:
    mov ax, DATA32_SEL
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov rsp, 0x7FFF0

    mov rdi, HANDOFF_STRUCT_ADDR
    mov rax, LEGACY_MAIN_LOAD_ADDR
    jmp rax

; места в файле не осталось для дальнейшего кода — всё, что нужно после
; входа в long mode, делает legacy_main.bin (C, собран отдельно).
