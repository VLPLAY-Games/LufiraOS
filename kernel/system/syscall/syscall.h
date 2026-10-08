#pragma once

#include "lib/types.h"
#include "system/ipc/mailbox.h" // MAX_IPC_MSG_PAYLOAD — см. lufira_ipc_msg_t ниже

// Номера системных вызовов
#define SYS_WRITE    0
#define SYS_READ     1
#define SYS_EXIT     2
#define SYS_GETPID   3
#define SYS_YIELD    4
#define SYS_GETTICK  5
#define SYS_OPEN     6
#define SYS_CLOSE    7
#define SYS_SEEK     8
#define SYS_MMAP     9
#define SYS_MUNMAP   10
#define SYS_EXEC     11
#define SYS_FORK     12
#define SYS_WAIT     13
#define SYS_GETCWD   14
#define SYS_CHDIR    15
#define SYS_SLEEP    16
#define SYS_KILL     17
#define SYS_PIPE     18
#define SYS_CHMOD    19
#define SYS_CHOWN    20
#define SYS_GETUID   21
#define SYS_GETGID   22
#define SYS_MKDIR    23
#define SYS_RMDIR    24
#define SYS_UNLINK   25
#define SYS_READDIR  26
#define SYS_STATFS   27
#define SYS_MEMINFO  28
#define SYS_CPULOAD  29
#define SYS_PSLIST   30
// SYS_SET_FOREGROUND (31): pid (0 — снять). foreground_pid (process.h)
// используется shell_handle_ctrl_c() (shell.c) для process_signal(SIGINT).
// Даёт userspace-шеллу то же самое: fork() -> SYS_SET_FOREGROUND(pid ребёнка)
// -> SYS_WAIT(pid). Разрешено выставлять только PID собственного ребёнка
// (process_set_foreground() проверяет ppid).
#define SYS_SET_FOREGROUND 31

// SYS_SU (32): username_ptr, password_ptr — меняет current_process->uid/gid
// (заменяет мёртвый command_su(), менять uid/gid напрямую из userspace
// нельзя). Проверка пароля внутри ядра: root может стать кем угодно без
// пароля, иначе users_check_password() обязателен. cwd не трогает — cd в
// домашнюю папку делает сам shell.elf после успешного вызова.
#define SYS_SU 32

// SYS_MOUNT (33): prefix_ptr ("/mnt/usb0"), usb_index. Монтирует USB MSD
// как FAT под VFS-путём: open/read/write/mkdir/unlink/readdir видят файлы
// под этим префиксом напрямую (см. fat_mount.h: только корневой уровень
// каждого монтирования, real-time синк после записи). >=0 (слот) при
// успехе; иначе отрицательный код vfs_fat_mount() (fat_mount.c): -1 плохой
// префикс, -2 занят, -3 нет свободных слотов, -4 нет устройства, -5
// неподдерживаемый блок, -6 устройство большое, -7 нет памяти, -8 ошибка
// чтения, -9 не FAT.
#define SYS_MOUNT 33

// SYS_UNMOUNT (34): prefix_ptr. Синкает "грязные" секторы на флешку и
// снимает монтирование. 0 при успехе, -1 если такого монтирования нет.
#define SYS_UNMOUNT 34

// SYS_REBOOT (35) / SYS_SHUTDOWN (36) — переносят command_reboot()/
// command_shutdown() в syscall'ы. Требуют root. Синкают LufiraFS
// (lufirafs_flush()) перед reset/shutdown. Не возвращаются при успехе.
#define SYS_REBOOT 35
#define SYS_SHUTDOWN 36

// SYS_DEVMODE (37): 0 — прочитать состояние (0/1), 1 — включить, 2 —
// выключить. Обёртка над devmode_set()/devmode_is_enabled() (devmode.h).
#define SYS_DEVMODE 37

// SYS_USERADD (38): username_ptr, password_ptr, group_ptr (0 — завести
// новую группу с тем же именем, как Linux useradd по умолчанию). Root-only.
// Обёртка над users_add()/groups_add() (users.h) плюс создание домашнего
// каталога (своя копия ensure_home_dir() в syscall.c).
#define SYS_USERADD 38

// SYS_GROUPADD (39): groupname_ptr. Root-only. Тонкая обёртка над
// groups_add().
#define SYS_GROUPADD 39

// SYS_PASSWD (40): username_ptr (0 — сменить свой собственный пароль, без
// проверки прав — current_process уже аутентифицирован), new_password_ptr.
// Ненулевой username_ptr — сброс пароля ЛЮБОГО пользователя, root-only.
#define SYS_PASSWD 40

// SYS_USB_COUNT (41): без аргументов — число найденных USB mass storage
// устройств (xhci_msd_device_count()).
#define SYS_USB_COUNT 41

// SYS_USB_INFO (42): index, out_ptr (struct lufira_usb_info{max_lba,
// block_size} — см. ниже). -1 если такого устройства нет.
#define SYS_USB_INFO 42

// SYS_USB_READ (43): index, lba, buf_ptr, buf_size (должен быть >= реального
// block_size устройства, иначе -EINVAL — тот же приём, что уже у SYS_MEMINFO/
// SYS_STATFS с размером структуры). Читает ОДИН блок.
#define SYS_USB_READ 43

// SYS_USB_WRITE (44): index, lba, buf_ptr, buf_size — пишет ОДИН блок.
// Root-only (в отличие от чтения) — прямая запись по LBA на реальное
// устройство необратима и может повредить файловую систему, смонтированную
// тем же устройством через SYS_MOUNT.
#define SYS_USB_WRITE 44

// SYS_POLL (45): fds_ptr (массив lufira_pollfd_t), nfds, timeout_ms (0 —
// опросить и вернуться, >0 — ждать не больше стольки мс, <0 — неограниченно).
// Реализация — опрос с шагом в тик (10мс) через process_sleep(), не честное
// пробуждение по событию (потребовало бы регистрировать процесс сразу в
// нескольких independent waiter-слотах) — задержка до 10мс незаметна для
// ввода, а реализация проще. Возвращает число fd с ненулевым revents, 0 при
// таймауте, иначе -errno.
#define SYS_POLL 45

// SYS_SIGACTION (46): sig, handler_ptr (0 — вернуть default). Разрешены
// только SIGINT/SIGTERM (см. SIGINT в process.h) — SIGKILL/SIGSTOP/SIGCONT
// не ловятся, как в POSIX.
#define SYS_SIGACTION 46

// SYS_SIGRETURN (47): без аргументов, обрабатывается отдельно в
// syscall_handler() (как SYS_FORK) — нужен frame_ptr, не обычные 5
// аргументов (process_sigreturn(), process.c). Обязательный способ
// завершить обработчик сигнала — восстанавливает точное состояние
// процесса на момент, когда сигнал его прервал.
#define SYS_SIGRETURN 47

// SYS_ALARM (48): milliseconds (0 — снять будильник). SIGALRM доставляется
// один раз, не периодически (как POSIX alarm(), не setitimer()) — для
// повтора вызывающий переустанавливает его в обработчике.
#define SYS_ALARM 48

// SYS_GET_FOREGROUND (49): без аргументов, возвращает foreground_pid (0,
// если не выставлен) — спутник SYS_SET_FOREGROUND(31) для чтения значения.
#define SYS_GET_FOREGROUND 49

// ===== v0.8 (GUI+WM) =====
// WM вынесен из ядра в userspace-процесс (lufira-packages/src/apps/wm.c,
// см. wm_protocol.h про архитектуру). Сигнатуры SYS_WIN_* не изменились —
// каждый внутри syscall.c теперь тонкая RPC-обёртка (упаковать запрос,
// mailbox_send() WM pid, дождаться ответа), не прямой кернел-вызов.
// Без иконок — только примитивы (прямоугольники, текст битмап-шрифтом).

// SYS_WIN_CREATE (50): x, y, w, h, title_ptr (может быть NULL). Создаёт
// окно w x h (клиентская область) в (x, y), делает активным. -1, если WM
// не зарегистрирован. Возвращает id окна (>=0) или -1.
#define SYS_WIN_CREATE 50

// SYS_WIN_DESTROY (51): window_id. Закрывает окно (только владельцу —
// проверяет WM). Когда закрывается последнее окно, GUI-режим выключается
// и экран возвращается к текстовой консоли (console_redraw_from_history()).
#define SYS_WIN_DESTROY 51

// SYS_WIN_FILL (52): window_id, color (0xRRGGBB). Заливает клиентскую
// область одним цветом.
#define SYS_WIN_FILL 52

// SYS_WIN_DRAW_RECT (53): window_id, x, y, w, h, color — прямоугольник в
// клиентских координатах окна (0,0 — левый верхний угол клиентской
// области, не экрана).
#define SYS_WIN_DRAW_RECT 53

// SYS_WIN_DRAW_TEXT (54): window_id, x, y, text_ptr, color — битмап-шрифт
// 8x8. text_ptr обрезается до 63 байт при релее через mailbox
// (wm_request_t.str, wm_protocol.h).
#define SYS_WIN_DRAW_TEXT 54

// SYS_WIN_POLL_EVENT (55): window_id, event_buf_ptr (lufira_gui_event_t*).
// Неблокирующая проверка — 1, если событие было и записано, 0 если пусто.
#define SYS_WIN_POLL_EVENT 55

// SYS_WIN_MOVE (56): window_id, x, y — программное перемещение окна
// (помимо перетаскивания титлбара мышью самим WM).
#define SYS_WIN_MOVE 56

// ===== generic IPC + привилегированные syscall'ы WM =====

// SYS_IPC_SEND (57): dest_pid, msg_ptr, len (<= MAX_IPC_MSG_PAYLOAD,
// mailbox.h). Копирует len байт в почтовый ящик dest_pid — общий примитив,
// не привязанный к GUI (протокол WM — один из потребителей). 0 при успехе,
// -EINVAL (нет pid / len велик), -EAGAIN (ящик полон).
#define SYS_IPC_SEND 57

// SYS_IPC_RECV (58): msg_out_ptr (lufira_ipc_msg_t*), timeout_ms (signed).
// Забирает следующее сообщение своего ящика целиком. timeout_ms<0 — ждать
// неограниченно (настоящая блокировка), 0 — вернуть 0 немедленно если
// пусто, >0 — короткий опрос с шагом в тик. Возвращает 1, если получено.
#define SYS_IPC_RECV 58

// SYS_WM_REGISTER (59): без аргументов. Делает вызывающего единственным
// оконным сервером системы (process_get_wm_pid()) — SYS_WIN_*/события
// ввода адресуются его mailbox'у, только ему доступны SYS_FB_INFO/
// SYS_FB_PRESENT. 0 при успехе, -1 если WM уже зарегистрирован.
#define SYS_WM_REGISTER 59

// SYS_FB_INFO (60): out_ptr (lufira_fb_info_t*). Разрешён любому процессу
// (просто размеры экрана) — WM использует при старте для своего буфера.
// 0 при успехе, -EFAULT при плохом указателе.
#define SYS_FB_INFO 60

// SYS_FB_PRESENT (61): buf_ptr, w, h, dirty_xy, dirty_wh. buf_ptr — всегда
// полный кадр (w*h*4 байт в формате SYS_FB_INFO.pixel_format), но в
// реальный framebuffer копируется только под-прямоугольник dirty_xy/dirty_wh
// (gfx_blit(), graphics2d.c) — иначе каждое шевеление курсора мыши гоняло
// бы в MMIO/VRAM целый кадр и заметно тормозило. dirty_wh==0 — презентовать
// кадр целиком (совместимость со старым sys_fb_present(buf,w,h)).
// Упаковка (нет места на 2 int32-пары в 5 регистрах syscall'а): dirty_xy =
// (x<<32)|y, dirty_wh = (w<<32)|h — см. sys_fb_present_rect() (libc).
// Доступен только зарегистрированному WM pid, иначе -EPERM. -EINVAL при
// несовпадении размера буфера, -EFAULT при плохом указателе.
#define SYS_FB_PRESENT 61

// SYS_FB_FONT (62): out_ptr, max_bytes. Копирует битмап-шрифт 8x8
// (full_font_data, console.c, иначе недостижим для userspace) в out_ptr,
// не больше max_bytes. Возвращает реальный размер шрифта
// (console_get_font_size()) — может быть больше max_bytes, тогда вызывающий
// перевызывает с большим буфером.
#define SYS_FB_FONT 62

// SYS_CONSOLE_INJECT (63) / SYS_CONSOLE_REDRAW (64) — удалены. Были нужны,
// когда рабочий стол WM рисовался только при открытом окне; теперь рисуется
// постоянно с момента SYS_WM_REGISTER, назад к консоли ведёт кнопка "Exit"
// в таскбаре (обычный sys_exit() WM). Номера оставлены неиспользуемыми.

// SYS_DUP2 (65): oldfd, newfd. Обёртка над vfs_dup2() (vfs.h), доступна
// userspace напрямую. Нужна терминальному GUI-приложению: перед exec()
// дочернего /bin/shell.elf подменяет fd 0/1/2 на концы двух pipe().
// Возвращает newfd при успехе, -1 при плохом oldfd/newfd.
#define SYS_DUP2 65

// SYS_NET_FETCH (66): url_ptr, out_buf_ptr, out_cap, status_ptr. Пакетный
// менеджер (dlpg sync/upgrade, lufira-packages/base/dlpg.c) — единственный
// сегодняшний вызывающий. Единственный сетевой syscall, который реально
// нужен userspace: не голый сокет-API (SEND/RECV/CONNECT по отдельности),
// а готовый "скачать URL целиком" поверх http_client.c (http_fetch()) —
// тот уже делает ВСЁ: парсинг URL (схема http/https, хост, порт, путь),
// DNS (dns.c, если хост не голый IP), TCP (tcp.c) или TLS 1.2 поверх него
// (tls.c, только для https://) и разбор HTTP-ответа (включая
// Transfer-Encoding: chunked). Тот же "блокирующий поллинг-цикл внутри
// одного синхронного вызова" стиль, что уже у tcp_connect()/dns_resolve()
// — ничего нового в модели конкурентности не добавляет.
//
// url_ptr — С-строка ("http://..." или "https://..."); out_buf_ptr/out_cap
// — буфер вызывающего под ТЕЛО ответа (без статус-строки/заголовков);
// status_ptr (может быть 0/NULL) — если не NULL, кернел пишет туда HTTP
// статус-код (200/404/...), если успел дойти до разбора заголовков.
//
// Возврат: >=0 — длина тела в out_buf (гарантированно <= out_cap);
// отрицательные коды ошибок см. у sys_net_fetch() (syscall.c) и
// http_fetch() (net/http_client.h) — DNS/TCP/TLS/HTTP-разбор/ENOSPC
// развёрнуты по отдельным кодам, не одной общей "-1".
//
// БЕЗОПАСНОСТЬ TLS (https://): сертификат сервера разбирается ровно
// настолько, чтобы проверить подпись ServerKeyExchange его же открытым
// ключом (см. tls.c) — это защищает от пассивного/наивного MITM без
// валидного ключа хоть для какого-то сертификата, но ЦЕПОЧКА ДОВЕРИЯ
// (что сертификат подписан настоящим корневым CA для именно этого хоста)
// НЕ проверяется — нет встроенного доверенного набора корневых
// сертификатов. Против атакующего, способного подсунуть СВОЙ
// самоподписанный сертификат, это НЕ защищает. Сознательное урезание
// объёма (полная проверка цепочки требует наборов доверенных корневых
// сертификатов и их парсинга) — см. комментарий в начале tls.c.
#define SYS_NET_FETCH 66

// Флаги для sys_open
#define O_RDONLY    0
#define O_WRONLY    1
#define O_RDWR      2
#define O_CREAT     4
#define O_TRUNC     8
#define O_APPEND    16

// Флаги для sys_seek
#define SEEK_SET    0
#define SEEK_CUR    1
#define SEEK_END    2

// Флаги для sys_mmap
#define PROT_READ   1
#define PROT_WRITE  2
#define PROT_EXEC   4

// Флаги flags для sys_mmap (значения как в Linux, для совместимости с
// будущей libc). Требует MAP_ANONYMOUS (файловый mmap не поддерживается),
// отвергает MAP_FIXED (свой адрес не учитывается — отказ честнее молчаливого
// игнорирования).
#define MAP_SHARED     0x01
#define MAP_PRIVATE    0x02
#define MAP_FIXED      0x10
#define MAP_ANONYMOUS  0x20

// Коды ошибок — подмножество POSIX/Linux errno (те же числа). Возвращаются
// как (uint64_t)-CODE. Только то, что реально различают проверки
// user-указателей и sys_getcwd()/sys_chdir() — остальные VFS-сбои пока
// простой -1.
#define EPERM    1
#define ENOENT   2
#define EAGAIN   11 // v0.8 (GUI+WM), этап 3: SYS_IPC_SEND — почтовый ящик адресата полон
#define EACCES   13
#define EFAULT   14
#define ENOTDIR  20
#define EINVAL   22
#define ERANGE   34

// Структуры для SYS_STATFS/SYS_MEMINFO/SYS_CPULOAD — те же данные, что
// command_df()/command_free()/command_cpuload() (shell/commands/) печатают
// из lufirafs.sb/pmm_get_*()/heap_get_stats()/pit_get_*(), через буфер в
// userspace вместо printf() внутри ядра. Мирроятся байт-в-байт в
// libc/include/lufira/syscall.h, как и vfs_dirent_t для SYS_READDIR.
typedef struct {
    uint32_t block_size;
    uint32_t total_blocks;
    uint32_t free_blocks;
    uint32_t inode_count;
    uint32_t free_inodes;
} lufira_statfs_t;

typedef struct {
    uint64_t total_pages;      // физическая RAM, страницы по 4KB (pmm.c)
    uint64_t used_pages;
    uint64_t heap_total_bytes; // куча ядра (heap.c)
    uint64_t heap_used_bytes;
} lufira_meminfo_t;

typedef struct {
    uint64_t total_ticks;      // сырые счётчики PIT (pit.c) — сэмплирование
    uint64_t idle_ticks;       // (два снимка + sleep между ними) делает сама
                                // пользовательская программа, как и сегодня
                                // делает kernel-native command_cpuload().
} lufira_cpuload_t;

// Одна запись снимка SYS_PSLIST (30) — то же, что process_ps() печатает
// построчно, плюс ppid/uid/cpu_ticks (для per-process разбивки в
// command_cpuload()). state — сырое process_state_t; программа сама мапит
// числа в строки (см. user/ps.c).
typedef struct {
    uint32_t pid;
    uint32_t ppid;
    char name[32];
    uint32_t state;
    uint32_t uid;
    uint64_t cpu_ticks;
} lufira_ps_entry_t;

// SYS_USB_INFO (42) — зеркало xhci_msd_get_info() (xhci.h), байт-в-байт
// мирроится в libc/include/lufira/syscall.h, как и структуры выше.
typedef struct {
    uint32_t max_lba;
    uint32_t block_size;
} lufira_usb_info_t;

// SYS_IPC_RECV (58) — тот же layout, что ipc_msg_t (mailbox.h): ядро
// memcpy() структуру целиком между kernel-side mailbox и буфером пользователя
// (sys_ipc_recv(), syscall.c), без отдельной клиентской раскладки.
typedef struct __attribute__((packed)) {
    uint32_t sender_pid;
    uint32_t len;
    uint8_t payload[MAX_IPC_MSG_PAYLOAD];
} lufira_ipc_msg_t;

// SYS_FB_INFO (60). pixel_format — 0/1, та же величина, что внутреннее
// console.c::pixel_format (0 = RGB-резерв, 1 = BGR-резерв, см.
// convert_color()) — WM реплицирует эту же логику byte-swap в userspace
// (не может позвать convert_color() напрямую).
typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t pixel_format;
} lufira_fb_info_t;

// SYS_POLL (45) — те же значения LUFIRA_POLLIN/OUT, что и в
// vfs_poll_check() (fs/vfs/vfs.h) и userspace-зеркале.
#define LUFIRA_POLLIN  1
#define LUFIRA_POLLOUT 2

typedef struct {
    int32_t fd;
    int32_t events;
    int32_t revents;
} lufira_pollfd_t;

// Потолок длины ЛЮБОЙ NUL-терминированной строки от пользователя (filename
// для open/exec, path для chdir) — не даёт неверно терминированному буферу
// заставить нас сканировать по странице за страницей бесконечно.
#define USER_STRING_MAX 4096

// Прототипы
void syscall_init(void);
// frame_ptr — указатель на кадр регистров, сохранённый syscall_entry.S на
// ядерном стеке (см. syscall_frame_t в process.c); нужен только SYS_FORK.
uint64_t syscall_handler(uint64_t syscall_num, uint64_t arg1,
                         uint64_t arg2, uint64_t arg3,
                         uint64_t arg4, uint64_t arg5,
                         uint64_t frame_ptr);

// Открывает filename и заменяет им текущий процесс (execve()-подобно).
// Используется SYS_EXEC и командой shell "exec". Забирает владение
// argv/envp (kmalloc на каждую строку + на сам массив, NULL допустим) —
// освобождает их сама на любом исходе (см. free_argv_envp() в elf.h).
int do_exec(const char *filename, char *argv[], char *envp[]);