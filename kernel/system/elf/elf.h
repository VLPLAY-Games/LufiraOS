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
int elf_exec(const void *elf_data, uint64_t elf_size, const char *name);
int elf_exec_background(const void *elf_data,
                        uint64_t elf_size,
                        const char *name);