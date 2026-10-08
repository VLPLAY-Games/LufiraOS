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
// SYS_SET_FOREGROUND (31): pid (0 — снять). v0.7 план, этап 5, под-этап 6
// (Ctrl+C). foreground_pid (process.h) уже существует и уже безопасно
// используется shell_handle_ctrl_c() (kernel/shell/shell.c, отложенно из
// timer_irq_handler() — см. комментарий у shell_ctrl_c_pending в shell.h)
// для process_signal(foreground_pid, SIGINT) — сегодня его выставляет
// только кернел-нативный command_run(). Этот syscall даёт то же самое
// userspace-процессу (будущему shell.elf): fork() → SYS_SET_FOREGROUND(pid
// ребёнка) → SYS_WAIT(pid) — если Ctrl+C убьёт ребёнка, SYS_WAIT вернётся
// как обычно (тот же путь, что и при естественном завершении), никакого
// отдельного уведомления не нужно. Разрешено выставлять только PID
// СОБСТВЕННОГО ребёнка (process_set_foreground() проверяет ppid) — не
// чужой процесс.
#define SYS_SET_FOREGROUND 31

// SYS_SU (32): username_ptr, password_ptr — заменяет мёртвый
// kernel-native command_su() (kernel/shell/commands/users.c, недостижим
// после перехода на shell.elf) настоящим syscall'ом, т.к. shell.elf — уже
// не сам ядро, и менять current_process->uid/gid напрямую из userspace
// нельзя. Проверка пароля ВНУТРИ ядра (не доверяем userspace звать это
// только "после успешной проверки") — root (uid==0) может стать кем
// угодно без пароля, иначе users_check_password() обязателен. При успехе
// мутирует current_process->uid/gid (никакого отдельного syscall/сессии —
// шелл сам и есть current_process, как и раньше). cwd НЕ трогает — cd в
// домашнюю папку делает сам shell.elf после успешного вызова (см. builtin
// su в userspace/base/shell.c) — это уже новое поведение по сравнению со
// старым command_su (которое cwd никогда не меняло).
#define SYS_SU 32

// SYS_MOUNT (33): prefix_ptr ("/mnt/usb0" и т.п.), usb_index — v0.7 план,
// этап 5, под-этап 6 (VFS-интеграция монтирования). Монтирует указанное
// USB MSD устройство как FAT под заданным VFS-путём: обычные open/read/
// write/mkdir/unlink/readdir (SYS_OPEN и т.д.) начинают видеть файлы под
// этим префиксом напрямую, без отдельных mount-специфичных syscall'ов —
// см. kernel/fs/fat/fat_mount.h про границы (только корневой уровень
// каждого монтирования) и про real-time синк на флешку после каждой
// записи. Возвращает >=0 (слот) при успехе; при ошибке — один из
// отрицательных кодов vfs_fat_mount() (см. fat_mount.c): -1 плохой
// префикс, -2 префикс уже занят, -3 нет свободных слотов (максимум 2),
// -4 нет такого USB-устройства, -5 неподдерживаемый размер блока, -6
// устройство слишком большое, -7 не хватило памяти, -8 ошибка чтения,
// -9 не FAT.
#define SYS_MOUNT 33

// SYS_UNMOUNT (34): prefix_ptr. Синкает "грязные" секторы на флешку и
// снимает монтирование. 0 при успехе, -1 если такого монтирования нет.
#define SYS_UNMOUNT 34

// SYS_REBOOT (35) / SYS_SHUTDOWN (36) — v0.7 план, этап 5, продолжение
// ("как можно больше команд из ядра в пакеты"): переносят command_reboot()/
// command_shutdown() (kernel/shell/commands/system.c, мёртвый код) в
// настоящие syscall'ы. Требуют root (uid==0) — необратимое действие для
// всей системы. Синкают LufiraFS (lufirafs_flush()) перед собственно
// reset/shutdown, как и делал старый command_reboot()/command_shutdown().
// Не возвращаются при успехе.
#define SYS_REBOOT 35
#define SYS_SHUTDOWN 36

// SYS_DEVMODE (37): 0 — прочитать состояние (возвращает 0/1), 1 —
// включить, 2 — выключить. Тонкая обёртка над devmode_set()/
// devmode_is_enabled() (devmode.h) — то же самое, что раньше делал
// мёртвый command_devmode().
#define SYS_DEVMODE 37

// SYS_USERADD (38): username_ptr, password_ptr, group_ptr (0 — своей
// группы с именем пользователя нет, завести новую с тем же именем, как
// настоящий Linux useradd по умолчанию). Root-only. Тонкая обёртка над
// users_add()/groups_add() (users.h, оба уже пишут и файл, и g_users[]/
// g_groups[] в памяти атомарно) — перенос command_useradd() (kernel/shell/
// commands/users.c, мёртвый код) без изменения самой логики, кроме
// создания домашнего каталога (своя копия ensure_home_dir() прямо в
// syscall.c — тот файл мёртвый и не экспортирует свои статические
// хелперы, см. тот же приём уже у fat_mount.c с mount.c).
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

// SYS_POLL (45): fds_ptr (массив lufira_pollfd_t, см. ниже), nfds,
// timeout_ms (0 — опросить и вернуться сразу, >0 — ждать не больше
// стольки мс, <0 — ждать неограниченно). v0.8-мост, пункт 3: раньше
// единственный способ дождаться готовности сразу нескольких fd (ввод +
// несколько клиентских pipe, как нужно будущему WM) — отдельный процесс
// на каждое соединение, т.к. блокирующий SYS_READ умеет ждать только
// один fd за раз. Реализация — опрос с шагом в один тик (10мс,
// PIT_FREQUENCY) через уже существующий process_sleep(), а не честное
// пробуждение по событию (которое потребовало бы регистрировать
// текущий процесс сразу в НЕСКОЛЬКИХ independent waiter-слотах — pipe_t/
// console_input_waiter и т.д., см. их комментарии — и снимать
// регистрацию при первом же срабатывании): для ввода с клавиатуры или
// общения с несколькими процессами задержка до 10мс незаметна, а реализация
// заметно проще и без риска гонки между регистрацией на N объектах сразу.
// Возвращает число fd с ненулевым revents, 0 при таймауте, иначе -errno.
#define SYS_POLL 45

// SYS_SIGACTION (46): sig, handler_ptr (0 — вернуть действие по умолчанию).
// Разрешены только SIGINT/SIGTERM (см. комментарий у SIGINT в process.h) —
// SIGKILL/SIGSTOP/SIGCONT нельзя поймать, как и в настоящем POSIX; любой
// другой номер тоже отклоняется. v0.8-мост, пункт 5.
#define SYS_SIGACTION 46

// SYS_SIGRETURN (47): без аргументов — обрабатывается отдельно в
// syscall_handler() (как и SYS_FORK), т.к. нужен frame_ptr, а не обычные
// 5 аргументов (см. process_sigreturn(), process.c). Обязательный способ
// ЗАВЕРШИТЬ обработчик сигнала (SYS_SIGACTION выше) — вместо обычного C
// "return" (контракт этого ABI, см. sys_signal() в userspace libc/include/
// lufira/syscall.h) — восстанавливает ТОЧНОЕ состояние процесса на момент,
// когда сигнал его прервал.
#define SYS_SIGRETURN 47

// SYS_ALARM (48): milliseconds (0 — снять уже взведённый будильник, если
// есть). v0.8-мост, пункт 6: раньше единственный способ "подождать" —
// блокирующий SYS_SLEEP, непригодный для анимации/частоты кадров (нужно
// делать и другую работу, не просто спать). SIGALRM доставляется ОДИН
// РАЗ, не периодически (как настоящий POSIX alarm(), не setitimer()) —
// для повтора вызывающий сам переустанавливает его внутри обработчика.
// Возвращает 0.
#define SYS_ALARM 48

// SYS_GET_FOREGROUND (49): без аргументов, возвращает текущий foreground_pid
// (0, если не выставлен). v0.8-мост, пункт 7 — спутник к
// SYS_SET_FOREGROUND(31), который сам раньше не давал userspace способа
// ПРОЧИТАТЬ значение, только выставить/снять. См. также генерализацию
// самого process_set_foreground() в process.h/process.c (пункт 7 этого же
// моста): владение теперь проверяется по всей цепочке потомков, не только
// по прямому ребёнку.
#define SYS_GET_FOREGROUND 49

// ===== v0.8 (GUI+WM) =====
// Этап 3: WM вынесен из ядра в обычный userspace-процесс (lufira-packages/
// src/apps/wm.c) — см. kernel/system/ipc/wm_protocol.h про архитектуру.
// Сигнатуры SYS_WIN_* НЕ ИЗМЕНИЛИСЬ с первого среза (клиентский код —
// gui_widgets.c, все приложения — не тронут ни строкой): внутри syscall.c
// каждый из них теперь тонкая RPC-обёртка (упаковать запрос, mailbox_send()
// зарегистрированному WM pid, дождаться ответа), а не прямой кернел-вызов.
// Без иконок (прямое указание пользователя) — только примитивы
// (прямоугольники, текст битмап-шрифтом).

// SYS_WIN_CREATE (50): x, y, w, h, title_ptr (C-строка, может быть NULL).
// Создаёт окно размером w x h (КЛИЕНТСКАЯ область, без рамки/титлбара) с
// верхним левым углом в (x, y) и делает его активным/сфокусированным.
// Отказывает (-1), если WM-процесс не зарегистрирован (SYS_WM_REGISTER) —
// то есть GUI-подсистема вообще не запущена. Возвращает id окна (>=0) или -1.
#define SYS_WIN_CREATE 50

// SYS_WIN_DESTROY (51): window_id. Закрывает окно (разрешено только
// владельцу — проверяет сам WM). Когда закрывается ПОСЛЕДНЕЕ окно в
// системе, GUI-режим выключается и экран возвращается к обычной текстовой
// консоли автоматически (console_redraw_from_history(), зовёт сам WM).
#define SYS_WIN_DESTROY 51

// SYS_WIN_FILL (52): window_id, color (0xRRGGBB). Заливает всю
// клиентскую область окна одним цветом.
#define SYS_WIN_FILL 52

// SYS_WIN_DRAW_RECT (53): window_id, x, y, w, h, color — заполненный
// прямоугольник В КЛИЕНТСКИХ координатах окна (0,0 — левый верхний угол
// клиентской области, НЕ экрана).
#define SYS_WIN_DRAW_RECT 53

// SYS_WIN_DRAW_TEXT (54): window_id, x, y, text_ptr, color — битмап-шрифт
// 8x8 (тот же, что у текстовой консоли), позиция — произвольные пиксели
// клиентской области, не клетки. text_ptr обрезается до 63 байт при
// релее через mailbox (wm_request_t.str, wm_protocol.h) — все реальные
// вызывающие (gui_widgets.c, демо-приложения) короче этого предела.
#define SYS_WIN_DRAW_TEXT 54

// SYS_WIN_POLL_EVENT (55): window_id, event_buf_ptr (lufira_gui_event_t*,
// см. ниже). НЕблокирующая с точки зрения приложения проверка (сам RPC
// к WM всё же синхронный и короткий) — 1, если событие было и записано в
// event_buf, 0, если очередь пуста.
#define SYS_WIN_POLL_EVENT 55

// SYS_WIN_MOVE (56): window_id, x, y — программное перемещение окна
// (помимо перетаскивания титлбара мышью самим WM).
#define SYS_WIN_MOVE 56

// ===== v0.8 (GUI+WM), этап 3: generic IPC + привилегированные syscall'ы WM =====

// SYS_IPC_SEND (57): dest_pid, msg_ptr, len (<= MAX_IPC_MSG_PAYLOAD,
// mailbox.h). Копирует len байт из msg_ptr в почтовый ящик процесса
// dest_pid. Это ОБЩИЙ примитив, не привязанный к GUI — протокол WM
// (wm_protocol.h) просто один из возможных потребителей. 0 при успехе,
// -EINVAL (нет такого pid, либо len слишком большой), -EAGAIN (ящик
// адресата полон — на практике не ожидается при нынешней глубине очереди).
#define SYS_IPC_SEND 57

// SYS_IPC_RECV (58): msg_out_ptr (lufira_ipc_msg_t*, см. ниже), timeout_ms
// (signed). Забирает следующее сообщение СВОЕГО почтового ящика целиком
// (sender_pid+len+payload). timeout_ms<0 — ждать НЕОГРАНИЧЕННО (настоящая
// блокировка, нулевая стоимость CPU, пока пусто); 0 — вернуть 0
// немедленно, если пусто; >0 — ждать не больше стольки мс (короткий опрос
// с шагом в тик, та же гранулярность, что у SYS_POLL выше — нужен WM,
// чтобы ненадолго ждать следующий sys_win_draw_*() той же пачки
// клиентской перерисовки перед тем, как пересобирать кадр целиком, см.
// lufira-packages/apps/wm.c). Возвращает 1, если сообщение получено.
#define SYS_IPC_RECV 58

// SYS_WM_REGISTER (59): без аргументов. Делает вызывающего ЕДИНСТВЕННЫМ
// оконным сервером системы (process_get_wm_pid()) — все последующие
// SYS_WIN_*/входящие события ввода адресуются его mailbox'у, и только ему
// становятся доступны SYS_FB_INFO/SYS_FB_PRESENT ниже. 0 при успехе, -1
// если WM уже зарегистрирован кем-то другим (ровно один на систему).
#define SYS_WM_REGISTER 59

// SYS_FB_INFO (60): out_ptr (lufira_fb_info_t*). Разрешён любому процессу
// (просто чтение размеров экрана) — нужен WM при старте, чтобы выделить
// compositor-буфер нужного размера и реплицировать convert_color() у себя
// (pixel_format). 0 при успехе, -EFAULT при плохом указателе.
#define SYS_FB_INFO 60

// SYS_FB_PRESENT (61): buf_ptr, w, h, dirty_xy, dirty_wh. buf_ptr — ВСЕГДА
// полный кадр (w*h*4 байт, уже в финальном пиксельном формате экрана, см.
// SYS_FB_INFO.pixel_format; w/h обязаны точно совпадать с текущим
// разрешением экрана, это проверяется как и раньше), но реально в
// реальный framebuffer копируется ТОЛЬКО под-прямоугольник dirty_xy/
// dirty_wh этого буфера (переиспользует gfx_blit(), graphics2d.c — та же
// double-buffering инфраструктура, что и у текстовой консоли, см.
// console_tick_present(), pit.c).
//
// НАЙДЕННЫЙ БАГ (жалоба пользователя: курсор WM заметно "тормозит" в
// реальном использовании) — раньше presentился ВСЕГДА целый кадр, даже
// когда реально поменялся только маленький силуэт курсора поверх уже
// готовой сцены; копия в framebuffer (часто MMIO/VRAM, ощутимо дороже
// RAM) всего кадра на каждое шевеление мыши (до 100 раз/сек, см.
// console_tick_present()) и создавала заметную задержку. dirty_wh==0
// (двойной сентинел: обе половины отдельно упакованных слов нулевые) —
// "дырявого прямоугольника нет, презентовать кадр целиком" — ровно
// поведение старого вызова sys_fb_present(buf,w,h), который передаёт 0 в
// оставшиеся аргументы, так что старые места вызова (если такие
// остались бы) продолжают работать без изменений.
//
// Упаковка (нет места на 2 новых int32-пары в 5 generic long-регистрах
// syscall'а, см. __syscall5()): dirty_xy = (x:int32 << 32) | (y:int32 &
// 0xFFFFFFFF), dirty_wh = (w:uint32 << 32) | (h:uint32). См.
// sys_fb_present_rect() в syscall.h (libc) для готовой обёртки.
//
// Доступен ТОЛЬКО зарегистрированному SYS_WM_REGISTER pid — любой другой
// вызывающий получает -EPERM. -EINVAL при несовпадении размера буфера,
// -EFAULT при плохом буфере/указателе.
#define SYS_FB_PRESENT 61

// SYS_FB_FONT (62): out_ptr, max_bytes. Копирует битмап-шрифт 8x8
// (full_font_data, console.c — static, недостижим для userspace иначе)
// в out_ptr, не больше max_bytes байт. Разрешён любому процессу (просто
// константные данные) — нужен WM, чтобы рисовать титлбар/кнопку закрытия
// у себя в compositor-буфере без обращения к ядру на каждый символ.
// Возвращает реальный размер шрифта в байтах (console_get_font_size()) —
// может быть БОЛЬШЕ max_bytes, если буфер мал (тогда скопирована только
// первая max_bytes часть) — так вызывающий узнаёт, нужно ли перевызвать
// с большим буфером.
#define SYS_FB_FONT 62

// SYS_CONSOLE_INJECT (63) / SYS_CONSOLE_REDRAW (64) — УДАЛЕНЫ, этап 4.
// Были нужны только пока рабочий стол WM рисовался лишь при открытом окне
// (см. git-историю) — WM возвращал клавиши в текстовую консоль, пока окон
// было 0, и просил перерисовать её, когда закрывалось последнее окно.
// Теперь рабочий стол (фон + ярлыки + таскбар, lufira-packages/apps/wm.c)
// рисуется ПОСТОЯННО с момента SYS_WM_REGISTER, так что оба случая больше
// не возникают — назад к текстовой консоли теперь ведёт кнопка "Exit" в
// таскбаре (обычный sys_exit() самого WM; console_redraw_from_history()
// при этом зовётся кернелом напрямую, см. process_exit(), process.c —
// отдельный syscall для этого никогда не был нужен). Номера 63/64
// намеренно оставлены неиспользуемыми, не переназначены.

// SYS_DUP2 (65): oldfd, newfd. Тонкая обёртка над уже существующим
// vfs_dup2() (fs/vfs/vfs.h) — тем же приёмом, которым process_fork() сам
// дублирует fd-таблицу при fork(), только теперь доступен userspace
// напрямую. Нужен терминальному GUI-приложению (lufira-packages/apps/
// terminal.c, этап 4): перед exec()'ом дочернего /bin/shell.elf
// подменяет его fd 0/1/2 на концы двух pipe() — так GUI-окно становится
// настоящим терминалом вокруг настоящего shell.elf, а не отдельной
// переимплементацией парсера команд. Возвращает newfd при успехе, -1 при
// плохом oldfd/newfd (тот же диапазон проверки, что и у vfs_dup2()).
#define SYS_DUP2 65

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

// Флаги flags для sys_mmap (значения как в Linux — незачем изобретать свои,
// пригодится для совместимости с будущей libc). sys_mmap требует
// MAP_ANONYMOUS (файловый mmap не поддерживается) и отвергает MAP_FIXED
// (свой адрес вызывающего в этой версии не учитывается вообще — молча
// игнорировать было бы хуже отказа, вызывающий решил бы, что адрес учли).
#define MAP_SHARED     0x01
#define MAP_PRIVATE    0x02
#define MAP_FIXED      0x10
#define MAP_ANONYMOUS  0x20

// Коды ошибок — подмножество POSIX/Linux errno (те же числа, незачем
// изобретать свои — пригодится будущей libc). Возвращаются из syscall'ов
// как (uint64_t)-CODE, тем же соглашением, что уже использовалось для
// (uint64_t)-1 везде в этом файле. Только то, что реально различают новые
// проверки user-указателей и sys_getcwd()/sys_chdir() — остальные
// (VFS-уровня) сбои пока остаются простым -1, см. syscall.c.
#define EPERM    1
#define ENOENT   2
#define EAGAIN   11 // v0.8 (GUI+WM), этап 3: SYS_IPC_SEND — почтовый ящик адресата полон
#define EACCES   13
#define EFAULT   14
#define ENOTDIR  20
#define EINVAL   22
#define ERANGE   34

// Структуры для SYS_STATFS/SYS_MEMINFO/SYS_CPULOAD (v0.7, этап 1) — ровно
// те же данные, что уже печатают kernel-native command_df()/command_free()/
// command_cpuload() (kernel/shell/commands/{filesystem,system}.c) напрямую
// из lufirafs.sb/pmm_get_*()/heap_get_stats()/pit_get_*(), только через
// буфер в пользовательском пространстве вместо printf() внутри ядра —
// первый шаг выноса du/df/free/cpuload в отдельные пакеты (v0.7 план).
// Мирроятся байт-в-байт в libc/include/lufira/syscall.h, как и
// vfs_dirent_t/struct lufira_dirent для SYS_READDIR выше.
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

// Одна запись снимка SYS_PSLIST (30) — v0.7 план, этап 5, под-этап 4: то же,
// что process_ps() (process.c) уже печатает построчно из process_list, плюс
// ppid/uid/cpu_ticks (те использует command_cpuload() для per-process
// разбивки — process_t.cpu_ticks уже накапливается планировщиком, см.
// комментарий у этого поля в process.h). state — сырое значение
// process_state_t; имени состояния (process_state_name()) на этой стороне
// нет, программа сама мапит числа в строки, см. userspace/user/ps.c.
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

// SYS_IPC_RECV (58) — тот же layout, что и ipc_msg_t (mailbox.h): ядро
// просто memcpy() этой структуры целиком между своим kernel-side mailbox
// и пользовательским буфером (см. sys_ipc_recv(), syscall.c) — не
// переизобретает отдельную "клиентскую" раскладку с другими отступами.
typedef struct __attribute__((packed)) {
    uint32_t sender_pid;
    uint32_t len;
    uint8_t payload[MAX_IPC_MSG_PAYLOAD];
} lufira_ipc_msg_t;

// SYS_FB_INFO (60). pixel_format — 0/1, та же величина, что и внутреннее
// console.c::pixel_format (0 = PixelRedGreenBlueReserved8BitPerColor, 1 =
// PixelBlueGreenRedReserved8BitPerColor, см. convert_color() там же) — WM
// реплицирует ЭТУ ЖЕ логику byte-swap у себя в userspace (не может
// позвать convert_color() напрямую, это больше не его адресное
// пространство), см. lufira-packages/src/apps/wm.c.
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
// Используется и SYS_EXEC, и командой shell "exec". Забирает владение
// argv/envp (форма "kmalloc на каждую строку + kmalloc на сам массив",
// NULL допустим у обоих) — освобождает их сама на любом пути, успех или
// нет (см. free_argv_envp() в elf.h).
int do_exec(const char *filename, char *argv[], char *envp[]);