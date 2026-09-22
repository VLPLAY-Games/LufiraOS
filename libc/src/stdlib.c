#include <stdlib.h>
#include <lufira/syscall.h>

void exit(int code) {
    sys_exit(code);
    for (;;) { } // sys_exit() не возвращается; чисто чтобы компилятор поверил noreturn
}
