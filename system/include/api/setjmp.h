#ifndef _SETJMP_H
#define _SETJMP_H

/* jmp_buf needs to store the registers saved by setjmp.
   16 elements of 64-bit unsigned integers is enough for both AArch64 (13 registers)
   and AMD64 (8 registers). */
typedef unsigned long long jmp_buf[16];

/* returns_twice: setjmp/longjmp are hand-written asm (per-arch syscall.S),
 * not compiler builtins - LUA_CFLAGS passes -fno-builtin, so GCC does NOT
 * special-case the name "setjmp" and treats this as an ordinary function
 * call. Without returns_twice, -O2 is free to cache values across the
 * setjmp() call site in registers/stack slots that a later longjmp() jump
 * back into the same frame will NOT see recomputed - corrupting whatever
 * runs immediately after the first return. This is exactly ldo.c's
 * LUAI_TRY/luaD_rawrunprotected, used by the very first lua_pcall() in
 * main() (wrapping pmain) - a miscompile here crashes before pmain's body
 * (and therefore luaL_openlibs/print_version) ever produces output. */
int setjmp(jmp_buf env) __attribute__((returns_twice));
void longjmp(jmp_buf env, int val) __attribute__((noreturn));

#endif /* _SETJMP_H */
