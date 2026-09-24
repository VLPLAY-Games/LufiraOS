#pragma once

#include "lib/types.h"

// ELF Magic
#define ELF_MAGIC 0x464C457F  // "\x7FELF" в little-endian

// Класс ELF (64-bit)
#define ELFCLASS64 2

// Тип ELF (исполняемый файл)
#define ET_EXEC 2
#define ET_DYN  3  // Position-independent executable (PIE)

// Архитектура (x86-64)
#define EM_X86_64 62

// Типы программных заголовков
#define PT_LOAD     1
#define PT_PHDR     6
#define PT_GNU_STACK 0x6474E551

// Флаги сегментов
#define PF_X 1  // Executable
#define PF_W 2  // Writable
#define PF_R 4  // Readable

// Заголовок ELF файла (64-bit)
typedef struct __attribute__((packed)) {
    uint32_t magic;          // 0x7F 'E' 'L' 'F'
    uint8_t  elf_class;      // 1=32-bit, 2=64-bit
    uint8_t  data;           // 1=little-endian, 2=big-endian
    uint8_t  version;        // 1=current
    uint8_t  os_abi;         // 0=System V, 3=Linux
    uint8_t  abi_version;
    uint8_t  padding[7];
    uint16_t type;           // 2=executable, 3=shared (PIE)
    uint16_t machine;        // 0x3E=x86-64
    uint32_t version2;
    uint64_t entry;          // Entry point
    uint64_t phoff;          // Program header offset
    uint64_t shoff;          // Section header offset
    uint32_t flags;
    uint16_t ehsize;         // Size of this header
    uint16_t phentsize;      // Program header entry size
    uint16_t phnum;          // Number of program headers
    uint16_t shentsize;      // Section header entry size
    uint16_t shnum;          // Number of section headers
    uint16_t shstrndx;       // Section header string table index
} elf64_header_t;

// Программный заголовок (64-bit)
typedef struct __attribute__((packed)) {
    uint32_t type;           // PT_LOAD, PT_PHDR, etc.
    uint32_t flags;          // PF_R, PF_W, PF_X
    uint64_t offset;         // Offset in file
    uint64_t vaddr;          // Virtual address
    uint64_t paddr;          // Physical address (unused)
    uint64_t filesz;         // Size in file
    uint64_t memsz;          // Size in memory
    uint64_t align;          // Alignment
} elf64_program_header_t;

// Структура процесса (forward declaration)
typedef struct process process_t;

// Функции
int elf_validate(const elf64_header_t *header);
void* elf_load_to_process(const void *elf_data, uint64_t elf_size, 
                          process_t *proc, const char *name);
// argv/envp — NULL-терминированные массивы (как у execve()); передайте
// NULL, если аргументов/окружения нет вовсе (эквивалентно argc=0). Обе
// функции только ЧИТАЮТ argv/envp (build_exec_stack(), process.c) —
// владение и освобождение остаётся за вызывающим, в отличие от elf_data
// (тот всегда освобождается этой функцией, успех или нет).
int elf_exec(const void *elf_data, uint64_t elf_size, const char *name,
             char *const argv[], char *const envp[]);
int elf_exec_background(const void *elf_data,
                        uint64_t elf_size,
                        const char *name,
                        char *const argv[], char *const envp[]);

// Настоящий execve(): заменяет ТЕКУЩИЙ процесс образом новой программы
// вместо создания нового процесса (используется командой shell "exec" и
// системным вызовом SYS_EXEC). В отличие от elf_exec()/elf_exec_background()
// выше, ЗАБИРАЕТ владение argv/envp — освобождает их сама (kfree каждой
// строки + самого массива) в КАЖДОМ пути возврата, тем же соглашением, что
// уже действует для elf_data. Вызывающий обязан передавать сюда только
// куски, полученные через kmalloc (свежую строку на каждый элемент +
// отдельный kmalloc на сам массив указателей) — например,
// copy_user_string_array() в syscall.c.
int elf_exec_replace(const void *elf_data, uint64_t elf_size, const char *name,
                     char *argv[], char *envp[]);

// Освобождает argv[]/envp[] в форме "kmalloc на каждую строку + kmalloc на
// сам массив" (то, что строит copy_user_string_array() в syscall.c) — оба
// параметра можно передавать NULL. Экспортирована из elf.c для do_exec()
// (syscall.c), у которого есть собственные пути отказа ДО того, как
// владение реально перейдёт к elf_exec_replace().
void free_argv_envp(char *argv[], char *envp[]);