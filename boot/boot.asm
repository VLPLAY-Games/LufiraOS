; boot.asm - MBR загрузчик для LufiraOS (для жесткого диска)
bits 16
org 0x7c00

STAGE2_OFFSET equ 0x7e00      ; Загружаем Stage2 сюда (сразу после MBR)
STAGE2_START_SECTOR equ 1     ; Stage2 начинается с 1-го сектора (после MBR)
STAGE2_SECTORS equ 4          ; Размер Stage2 в секторах (2KB)

start:
    ; Настройка сегментов
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7c00
    sti
    
    ; Сохраняем номер загрузочного диска
    mov [BOOT_DRIVE], dl
    
    ; Проверяем, загружаемся ли мы с жесткого диска (0x80+)
    cmp dl, 0x80
    jb .is_floppy
    mov byte [IS_HDD], 1
    jmp .continue
.is_floppy:
    mov byte [IS_HDD], 0
    
.continue:
    ; Печать приветственного сообщения
    mov si, boot_msg
    call print_string
    
    ; Проверяем, поддерживает ли BIOS расширенные функции чтения (LBA)
    call check_int13_extensions
    
    ; Загрузка Stage2 с диска
    call load_stage2
    
    ; Переход к Stage2
    mov dl, [BOOT_DRIVE]    ; Передаем номер диска в DL
    jmp STAGE2_OFFSET
    
    ; Сюда не должны дойти
    jmp $

; ====================================
; Проверка расширений INT 13h (LBA)
; ====================================
check_int13_extensions:
    pusha
    mov ah, 0x41
    mov bx, 0x55AA
    mov dl, [BOOT_DRIVE]
    int 0x13
    jc .no_extensions
    cmp bx, 0xAA55
    jne .no_extensions
    test cl, 1
    jz .no_extensions
    
    ; Расширения поддерживаются
    mov byte [LBA_SUPPORT], 1
    mov si, lba_support_msg
    call print_string
    popa
    ret
    
.no_extensions:
    ; Расширения не поддерживаются, используем CHS
    mov byte [LBA_SUPPORT], 0
    mov si, chs_mode_msg
    call print_string
    popa
    ret

; ====================================
; Функция загрузки Stage2
; ====================================
load_stage2:
    pusha
    
    cmp byte [LBA_SUPPORT], 1
    je .use_lba
    
    ; Используем CHS (старый метод)
    ; Для жесткого диска используем другой подход
    cmp byte [IS_HDD], 1
    je .chs_hdd
    
    ; Для флоппи-диска
    mov ah, 0x02            ; Функция чтения секторов
    mov al, STAGE2_SECTORS  ; Количество секторов для чтения
    mov ch, 0               ; Цилиндр 0
    mov cl, STAGE2_START_SECTOR ; Стартовый сектор (с 1)
    mov dh, 0               ; Головка 0
    mov dl, [BOOT_DRIVE]    ; Номер диска
    mov bx, STAGE2_OFFSET   ; Буфер для данных
    
    int 0x13                ; Прерывание диска
    jc disk_error           ; Если ошибка - CF=1
    
    ; Проверяем, сколько секторов прочитано
    cmp al, STAGE2_SECTORS
    jne disk_error
    
    jmp .success
    
.chs_hdd:
    ; Для жесткого диска через CHS
    ; Сброс дискового контроллера
    mov ah, 0x00
    mov dl, [BOOT_DRIVE]
    int 0x13
    jc disk_error
    
    ; Чтение Stage2 для жесткого диска
    mov ax, 0x0200 + STAGE2_SECTORS  ; AH=02 (читать), AL=секторы
    mov cx, 0x0002                   ; CH=0 (цилиндр), CL=2 (сектор)
    mov dh, 0                        ; Головка 0
    mov dl, [BOOT_DRIVE]             ; Номер диска
    mov bx, STAGE2_OFFSET            ; Буфер
    int 0x13
    jc disk_error
    
    jmp .success
    
.use_lba:
    ; Используем LBA (расширенный метод)
    push es
    mov ax, 0
    mov es, ax
    mov si, DAPACK          ; Адрес структуры DAP
    mov ah, 0x42            ; Расширенное чтение
    mov dl, [BOOT_DRIVE]    ; Номер диска
    int 0x13
    pop es
    jc disk_error
    
.success:
    ; Сообщение об успешной загрузке
    mov si, stage2_loaded_msg
    call print_string
    
    popa
    ret

; ====================================
; Обработка ошибки диска
; ====================================
disk_error:
    mov si, disk_err_msg
    call print_string
    
    ; Выводим код ошибки из AH
    mov al, ah
    call print_hex
    
    mov si, error_prompt
    call print_string
    
    ; Пытаемся прочитать клавишу
    mov ah, 0x00
    int 0x16
    
    ; Перезагрузка
    int 0x19

; ====================================
; Вывод hex-числа
; ====================================
print_hex:
    pusha
    mov cx, 4
.hex_loop:
    rol ax, 4
    mov bx, ax
    and bx, 0x0F
    mov bl, [hex_chars + bx]
    
    mov ah, 0x0E
    mov al, bl
    int 0x10
    
    loop .hex_loop
    
    ; Пробел после числа
    mov ah, 0x0E
    mov al, ' '
    int 0x10
    
    popa
    ret

; ====================================
; Вывод строки
; ====================================
print_string:
    pusha
    mov ah, 0x0E            ; Функция BIOS для вывода символа
.next_char:
    lodsb                   ; Загружаем следующий символ
    cmp al, 0
    je .done
    int 0x10                ; Выводим символ
    jmp .next_char
.done:
    popa
    ret

; ====================================
; Данные
; ====================================
boot_msg db 'LufiraOS S1: Loading S2...', 0x0D, 0x0A, 0
stage2_loaded_msg db 'S2 loaded successfully', 0x0D, 0x0A, 0
disk_err_msg db 'S1 Disk error: 0x', 0
error_prompt db 0x0D, 0x0A, 'Press any key to reboot', 0x0D, 0x0A, 0
lba_support_msg db 'LBA supported', 0x0D, 0x0A, 0
chs_mode_msg db 'Using CHS mode', 0x0D, 0x0A, 0
hex_chars db '0123456789ABCDEF'
BOOT_DRIVE db 0
LBA_SUPPORT db 0
IS_HDD db 0

; Структура DAP (Disk Address Packet) для LBA
DAPACK:
    db 0x10            ; Размер структуры (16 байт)
    db 0               ; Всегда 0
    dw STAGE2_SECTORS  ; Количество секторов для чтения
    dw STAGE2_OFFSET   ; Смещение буфера
    dw 0x0000          ; Сегмент буфера
    dq STAGE2_START_SECTOR  ; Начальный сектор LBA (64-битный)

; Заполнение и сигнатура
times 446-($-$$) db 0           ; Заполняем до конца MBR кода

; Таблица разделов (64 байта)
; Создаем один активный раздел, занимающий весь диск
partition1:
    db 0x80                    ; Флаг активности (0x80 = активный)
    db 0x00, 0x00, 0x00       ; CHS начало (не используется для LBA)
    db 0x0C                   ; Тип раздела (0x0C = FAT32 LBA)
    db 0xFE, 0xFF, 0xFF       ; CHS конец (не используется для LBA)
    dd 0x00000001             ; LBA начало раздела (сектор 1)
    dd 2879                   ; Размер раздела в секторах (всего 2880 секторов - 1)

; Остальные разделы пустые
times 16 db 0
times 16 db 0
times 16 db 0

; Сигнатура MBR
dw 0xAA55