; boot.asm - MBR загрузчик
bits 16
org 0x7c00

KERNEL_OFFSET equ 0x1000  ; Куда грузим ядро
SECTORS_TO_READ equ 50    ; Сколько секторов грузим (должно хватить для ядра)

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
    mov bx, KERNEL_OFFSET
    mov dh, SECTORS_TO_READ  ; Количество секторов
    mov dl, [BOOT_DRIVE]     ; Номер диска
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
    push dx
    
    mov ah, 0x02        ; Функция чтения секторов
    mov al, dh          ; Количество секторов
    mov ch, 0x00        ; Цилиндр 0
    mov dh, 0x00        ; Головка 0
    mov cl, 0x02        ; Начинаем со второго сектора (после MBR)
    
    int 0x13            ; Прерывание диска
    jc disk_error       ; Если ошибка
    
    pop dx
    cmp al, dh          ; Проверяем, сколько секторов прочитано
    jne disk_error
    
    popa
    ret

disk_error:
    mov si, disk_err_msg
    call print_string
    jmp $

; ====================================
; Переключение в защищенный режим
; ====================================
switch_to_pm:
    cli                     ; Запрещаем прерывания
    
    ; Загружаем GDT
    lgdt [gdt_descriptor]
    
    ; Устанавливаем бит защищенного режима в CR0
    mov eax, cr0
    or eax, 0x1
    mov cr0, eax
    
    ; Дальний прыжок для очистки конвейера и перехода в 32-битный код
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
; Глобальная таблица дескрипторов (GDT)
; ====================================
gdt_start:
    ; Нулевой дескриптор (обязательный)
    dq 0x0000000000000000

; Дескриптор сегмента кода
gdt_code:
    dw 0xFFFF           ; Лимит (0-15)
    dw 0x0000           ; База (0-15)
    db 0x00             ; База (16-23)
    db 10011010b        ; Флаги доступа (P=1, DPL=00, S=1, E=1, DC=0, RW=1, A=0)
    db 11001111b        ; Гранулярность (G=1, D/B=1, L=0, AVL=0) + Лимит (16-19)
    db 0x00             ; База (24-31)

; Дескриптор сегмента данных
gdt_data:
    dw 0xFFFF           ; Лимит (0-15)
    dw 0x0000           ; База (0-15)
    db 0x00             ; База (16-23)
    db 10010010b        ; Флаги доступа (P=1, DPL=00, S=1, E=0, DC=0, RW=1, A=0)
    db 11001111b        ; Гранулярность (G=1, D/B=1, L=0, AVL=0) + Лимит (16-19)
    db 0x00             ; База (24-31)

gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1  ; Размер GDT
    dd gdt_start                ; Адрес GDT

; Константы для селекторов
CODE_SEG equ gdt_code - gdt_start
DATA_SEG equ gdt_data - gdt_start

; ====================================
; Данные
; ====================================
boot_msg db 'Booting SimpleOS...', 0x0D, 0x0A, 0
disk_err_msg db 'Disk read error!', 0x0D, 0x0A, 0
BOOT_DRIVE db 0

; Заполнение до 510 байт и сигнатура MBR
times 510-($-$$) db 0
dw 0xAA55