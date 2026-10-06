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
#define PT_DYNAMIC  2 // v0.8-мост, пункт 8 (динамическая линковка): таблица Elf64_Dyn
#define PT_INTERP   3 // путь интерпретатора — только метаданные, см. dynlink.h
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

// ===== Динамическая линковка (v0.8-мост, пункт 8) =====
// См. подробное объяснение архитектуры в dynlink.h. Структуры ниже — то
// минимальное подмножество настоящего ELF ABI (System V x86-64), которое
// реально нужно для DT_NEEDED=libc.so + постраничных GOT/PLT-релокаций;
// версионирование символов (DT_VERNEED/DT_VERDEF), DT_INIT_ARRAY/
// DT_FINI_ARRAY, TLS (DT_TLS*) сюда намеренно не входят — ни один пакет
// сегодня ими не пользуется, и у этого минимального libc нет никакой
// нужды в конструкторах/TLS.

// Запись таблицы PT_DYNAMIC — ОДНА и та же структура используется и для
// d_val (целое: размеры, флаги), и для d_ptr (адрес) — какое поле валидно,
// определяется самим d_tag (см. DT_* ниже), как и в настоящем ELF ABI.
typedef struct __attribute__((packed)) {
    int64_t  d_tag;
    uint64_t d_val;
} elf64_dyn_t;

#define DT_NULL     0  // конец массива
#define DT_NEEDED   1  // d_val = смещение в .dynstr — имя нужной .so (например "libc.so")
#define DT_PLTRELSZ 2  // d_val = общий размер .rela.plt в байтах
#define DT_PLTGOT   3  // d_val = адрес .got.plt (не используется — читаем offset'ы прямо из релокаций)
#define DT_HASH     4  // d_val = адрес classic SysV .hash (nbucket,nchain,...) — нужен ТОЛЬКО nchain
#define DT_STRTAB   5  // d_val = адрес .dynstr
#define DT_SYMTAB   6  // d_val = адрес .dynsym
#define DT_RELA     7  // d_val = адрес .rela.dyn
#define DT_RELASZ   8  // d_val = общий размер .rela.dyn в байтах
#define DT_RELAENT  9  // d_val = размер одной записи .rela.dyn (всегда 24 для amd64)
#define DT_STRSZ    10 // d_val = размер .dynstr в байтах
#define DT_SYMENT   11 // d_val = размер одной записи .dynsym (всегда 24 для amd64)
#define DT_JMPREL   0x17 // d_val = адрес .rela.plt

// Запись таблицы символов .dynsym (всегда 24 байта на amd64 — см. DT_SYMENT).
typedef struct __attribute__((packed)) {
    uint32_t st_name;  // смещение в .dynstr
    uint8_t  st_info;
    uint8_t  st_other;
    uint16_t st_shndx;  // 0 (SHN_UNDEF) = символ не определён ЗДЕСЬ (импортируется)
    uint64_t st_value;
    uint64_t st_size;
} elf64_sym_t;

// Запись таблицы релокаций .rela.plt/.rela.dyn (всегда 24 байта на amd64).
typedef struct __attribute__((packed)) {
    uint64_t r_offset;  // куда писать результат (виртуальный адрес GOT-слота)
    uint64_t r_info;    // упаковка (символ, тип) — см. ELF64_R_SYM/ELF64_R_TYPE ниже
    int64_t  r_addend;
} elf64_rela_t;

#define ELF64_R_SYM(info)  ((uint32_t)((info) >> 32))
#define ELF64_R_TYPE(info) ((uint32_t)((info) & 0xffffffffu))

// Типы релокаций x86-64, которые реально встречаются при связывании
// non-PIE исполняемого файла с PIC-библиотекой (см. комментарий в
// dynlink.c с живым readelf-разбором, которым эта реализация
// проверялась) — ТОЛЬКО эти четыре, не полный список из ABI:
#define R_X86_64_RELATIVE  8 // база_библиотеки + addend — для релокаций САМОЙ libc.so (PIC/ET_DYN)
#define R_X86_64_GLOB_DAT  6 // адрес_символа — для данных, на которые ссылаются через указатель/GOT
#define R_X86_64_JUMP_SLOT 7 // адрес_символа — PLT/GOT слот для вызова функции
#define R_X86_64_COPY      5 // memcpy(offset, адрес_символа, size) — прямая (не через указатель)
                              // ссылка на данные ИЗ non-PIE исполняемого файла, см. dynlink.c

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