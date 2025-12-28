; stage2.asm - Второй этап загрузчика для LufiraOS
bits 16
org 0x7e00

KERNEL_OFFSET equ 0x1000          ; Адрес загрузки ядра (64K)
KERNEL_START_SECTOR equ 6         ; Ядро начинается после Stage2 (2+4=6)
KERNEL_SECTORS equ 53            ; размер ядра в секторах (27 136 Bytes)

MEMORY_MAP_OFFSET equ 0x5000      ; Адрес для хранения карты памяти

start:
    ; Сохраняем номер загрузочного диска
    mov [BOOT_DRIVE], dl
    
    ; Сообщение о запуске Stage2
    mov si, stage2_msg
    call print_string
    
    ; Проверяем расширения INT 13h
    call check_int13_extensions
    
    ; Получаем карту памяти
    call get_memory_map
    
    ; Загрузка ядра с диска
    call load_kernel
    
    ; Переход в защищенный режим
    call switch_to_pm
    
    ; Сюда не должны дойти
    jmp $

; ====================================
; Получение карты памяти через INT 15h, AX=E820h
; ====================================
get_memory_map:
    pusha
    push es
    
    mov ax, 0
    mov es, ax
    mov di, MEMORY_MAP_OFFSET + 4  ; Пропускаем счетчик записей (первые 4 байта)
    
    xor ebx, ebx                   ; EBX должен быть 0 для первой итерации
    mov edx, 0x534D4150            ; Сигнатура 'SMAP'
    mov eax, 0xE820
    mov ecx, 24                    ; Размер структуры (24 байта)
    
    int 0x15
    jc .error                      ; Если CF=1 - ошибка
    
    cmp eax, 0x534D4150            ; Проверяем сигнатуру
    jne .error
    
    mov dword [MEMORY_MAP_OFFSET], 1  ; Начальное количество записей
    
.loop:
    test ebx, ebx                  ; Если EBX = 0, это последняя запись
    je .done
    
    ; Увеличиваем счетчик записей
    mov eax, [MEMORY_MAP_OFFSET]
    inc eax
    mov [MEMORY_MAP_OFFSET], eax
    
    ; Подготавливаем следующий вызов
    add di, 24                     ; Переходим к следующей записи
    
    mov eax, 0xE820
    mov ecx, 24
    mov edx, 0x534D4150
    
    int 0x15
    jc .done                       ; Если ошибка, завершаем
    
    jmp .loop

.error:
    mov si, memory_map_error_msg
    call print_string
    mov dword [MEMORY_MAP_OFFSET], 0  ; Ноль записей при ошибке
    
.done:
    ; Выводим информацию о полученной карте памяти
    mov si, memory_map_ok_msg
    call print_string
    
    mov eax, [MEMORY_MAP_OFFSET]
    call print_hex_word
    
    mov si, memory_map_entries_msg
    call print_string
    
    pop es
    popa
    ret

; ====================================
; Вывод 32-битного слова в hex
; ====================================
print_hex_word:
    pusha
    
    ; Выводим старшее слово
    mov bx, ax
    shr eax, 16
    call print_hex
    
    ; Выводим младшее слово
    mov ax, bx
    call print_hex
    
    popa
    ret

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
    popa
    ret
    
.no_extensions:
    ; Расширения не поддерживаются, используем CHS
    mov byte [LBA_SUPPORT], 0
    popa
    ret

; ====================================
; Функция загрузки ядра
; ====================================
load_kernel:
    pusha
    
    cmp byte [LBA_SUPPORT], 1
    je .use_lba
    
    ; Используем CHS (старый метод)
    ; Сброс дискового контроллера
    mov ah, 0x00
    mov dl, [BOOT_DRIVE]
    int 0x13
    jc disk_error
    
    ; Настраиваем сегмент для загрузки ядра
    mov ax, 0x0000
    mov es, ax
    mov bx, KERNEL_OFFSET
    
    ; Параметры для чтения диска
    mov ah, 0x02            ; Функция чтения секторов
    mov al, KERNEL_SECTORS  ; Количество секторов (теперь 100)
    mov ch, 0               ; Цилиндр 0
    mov cl, KERNEL_START_SECTOR ; Стартовый сектор (с 1)
    mov dh, 0               ; Головка 0
    mov dl, [BOOT_DRIVE]    ; Номер диска
    
    int 0x13                ; Прерывание диска
    jc disk_error           ; Если ошибка - CF=1
    
    ; Проверяем, сколько секторов прочитано
    cmp al, KERNEL_SECTORS
    jne disk_error
    
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
    mov si, kernel_loaded_msg
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
; Переключение в защищенный режим
; ====================================
switch_to_pm:
    cli                     ; Запрещаем прерывания
    
    ; Сообщение о переходе в защищенный режим
    mov si, pm_msg
    call print_string
    
    ; Загружаем GDT
    lgdt [gdt_descriptor]
    
    ; Устанавливаем бит защищенного режима в CR0
    mov eax, cr0
    or eax, 0x1
    mov cr0, eax
    
    ; Дальний прыжок для очистки конвейера и переключения в 32-битный код
    jmp CODE_SEG:init_pm

bits 32
init_pm:
    ; Инициализируем сегментные регистры
    mov ax, DATA_SEG
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    
    ; Настраиваем стек
    mov ebp, 0x90000
    mov esp, ebp
    
    ; Передаем информацию о памяти ядру через регистр EBX
    mov ebx, MEMORY_MAP_OFFSET
    
    ; Вызываем ядро
    call KERNEL_OFFSET
    
    ; Если ядро вернет управление
    cli
    hlt

; ====================================
; 16-битные функции
; ====================================
bits 16

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
; Глобальная таблица дескрипторов
; ====================================
gdt_start:
    ; Нулевой дескриптор
    dq 0x0000000000000000

gdt_code:
    dw 0xFFFF               ; Лимит (0-15)
    dw 0x0000               ; База (0-15)
    db 0x00                 ; База (16-23)
    db 10011010b            ; Флаги доступа (P=1, DPL=00, S=1, E=1, DC=0, RW=1, A=0)
    db 11001111b            ; Гранулярность (G=1, D/B=1, L=0, AVL=0) + Лимит (16-19)
    db 0x00                 ; База (24-31)

gdt_data:
    dw 0xFFFF               ; Лимит (0-15)
    dw 0x0000               ; База (0-15)
    db 0x00                 ; База (16-23)
    db 10010010b            ; Флаги доступа (P=1, DPL=00, S=1, E=0, DC=0, RW=1, A=0)
    db 11001111b            ; Гранулярность + Лимит
    db 0x00                 ; База (24-31)

gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1  ; Размер GDT
    dd gdt_start                ; Адрес GDT

CODE_SEG equ gdt_code - gdt_start
DATA_SEG equ gdt_data - gdt_start

; ====================================
; Данные
; ====================================
stage2_msg db 'LufiraOS Stage2: Loading kernel...', 0x0D, 0x0A, 0
kernel_loaded_msg db 'Kernel loaded successfully', 0x0D, 0x0A, 0
pm_msg db 'Switching to protected mode...', 0x0D, 0x0A, 0
memory_map_ok_msg db 'Memory map obtained: ', 0
memory_map_entries_msg db ' entries', 0x0D, 0x0A, 0
memory_map_error_msg db 'Error getting memory map', 0x0D, 0x0A, 0
disk_err_msg db 'Stage2 Disk error: 0x', 0
error_prompt db 0x0D, 0x0A, 'Press any key to reboot', 0x0D, 0xA, 0
hex_chars db '0123456789ABCDEF'
BOOT_DRIVE db 0
LBA_SUPPORT db 0

; Структура DAP (Disk Address Packet) для LBA
DAPACK:
    db 0x10                 ; Размер структуры (16 байт)
    db 0                    ; Всегда 0
    dw KERNEL_SECTORS       ; Количество секторов для чтения (теперь 100)
    dw KERNEL_OFFSET        ; Смещение буфера
    dw 0x0000               ; Сегмент буфера
    dq KERNEL_START_SECTOR  ; Начальный сектор LBA (64-битный)

; Заполнение Stage2 до 4 секторов (2048 байт)
times 2048-($-$$) db 0