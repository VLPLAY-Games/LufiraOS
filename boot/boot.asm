; boot.asm - MBR загрузчик для LufiraOS
bits 16
org 0x7c00

KERNEL_OFFSET equ 0x1000  ; Куда грузим ядро
KERNEL_START_SECTOR equ 2 ; Начинаем с сектора 2 (после MBR)
SECTORS_PER_TRACK equ 18  ; Для 1.44MB дискеты

start:
    ; Настройка сегментов
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7c00
    
    ; Сохраняем номер загрузочного диска
    mov [BOOT_DRIVE], dl
    
    ; Печать приветственного сообщения
    mov si, boot_msg
    call print_string
    
    ; Загрузка ядра с диска
    call load_kernel
    
    ; Переход в защищенный режим
    call switch_to_pm
    
    ; Сюда не должны дойти
    jmp $

; ====================================
; Функция загрузки ядра
; ====================================
load_kernel:
    pusha
    
    ; Настраиваем сегмент для загрузки ядра
    mov ax, 0x0000
    mov es, ax
    mov bx, KERNEL_OFFSET
    
    ; Параметры для чтения диска
    mov ah, 0x02        ; Функция чтения секторов
    mov al, 50          ; Читаем 50 секторов (примерно 25KB)
    mov ch, 0           ; Цилиндр 0
    mov cl, KERNEL_START_SECTOR ; Сектор 2
    mov dh, 0           ; Головка 0
    mov dl, [BOOT_DRIVE] ; Номер диска
    
    int 0x13            ; Прерывание диска
    jc disk_error       ; Если ошибка - CF=1
    
    ; Проверяем, сколько секторов прочитано
    cmp al, 50
    jne disk_error
    
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
    jmp $

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
    
    ; Дальний прыжок для очистки конвейера
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
    
    ; Переход в ядро
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
    mov ah, 0x0E        ; Функция BIOS для вывода символа
.next_char:
    lodsb               ; Загружаем следующий символ
    cmp al, 0
    je .done
    int 0x10            ; Выводим символ
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
    dw 0xFFFF           ; Лимит
    dw 0x0000           ; База (0-15)
    db 0x00             ; База (16-23)
    db 10011010b        ; Флаги доступа
    db 11001111b        ; Флаги + лимит
    db 0x00             ; База (24-31)

gdt_data:
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 10010010b
    db 11001111b
    db 0x00

gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1
    dd gdt_start

CODE_SEG equ gdt_code - gdt_start
DATA_SEG equ gdt_data - gdt_start

; ====================================
; Данные
; ====================================
boot_msg db 'Booting LufiraOS...', 0x0D, 0x0A, 0
disk_err_msg db 'Disk error: 0x', 0
kernel_loaded_msg db 'Kernel loaded', 0x0D, 0x0A, 0
pm_msg db 'Switching to protected mode...', 0x0D, 0x0A, 0
error_prompt db 0x0D, 0x0A, 'System halted', 0
hex_chars db '0123456789ABCDEF'
BOOT_DRIVE db 0

; Заполнение и сигнатура
times 510-($-$$) db 0
dw 0xAA55