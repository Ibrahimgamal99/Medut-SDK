#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

// Force old glibc symbol versions — target glibc >= 2.2.5
// This file should be compiled and linked into medut.so

// ── Strategy ────────────────────────────────────────────────────────────────
// On glibc 2.38+, headers redirect fscanf → __isoc23_fscanf, strtol →
// __isoc23_strtol, etc. at the compiler level.  Every .o compiled against
// these headers therefore contains undefined references to __isoc23_*@GLIBC_2.38.
//
// We provide definitions of __isoc23_* that delegate to the old ABI via
// .symver aliases.  The version script keeps them local (not exported) so they
// only satisfy the undefined refs from inside medut.a without polluting the
// global namespace or intercepting calls from other libraries.

__asm__(".symver __old_vfscanf,vfscanf@GLIBC_2.2.5");
__asm__(".symver __old_vsscanf,vsscanf@GLIBC_2.2.5");
__asm__(".symver __old_strtol,strtol@GLIBC_2.2.5");
__asm__(".symver __old_strtoul,strtoul@GLIBC_2.2.5");
__asm__(".symver __old_strtoll,strtoll@GLIBC_2.2.5");
__asm__(".symver __old_strtoull,strtoull@GLIBC_2.2.5");

extern int   __old_vfscanf(FILE *, const char *, va_list);
extern int   __old_vsscanf(const char *, const char *, va_list);
extern long  __old_strtol(const char *, char **, int);
extern unsigned long __old_strtoul(const char *, char **, int);
extern long long __old_strtoll(const char *, char **, int);
extern unsigned long long __old_strtoull(const char *, char **, int);

int __isoc23_fscanf(FILE *stream, const char *format, ...) {
    va_list args;
    va_start(args, format);
    int r = __old_vfscanf(stream, format, args);
    va_end(args);
    return r;
}

int __isoc23_sscanf(const char *str, const char *format, ...) {
    va_list args;
    va_start(args, format);
    int r = __old_vsscanf(str, format, args);
    va_end(args);
    return r;
}

int __isoc23_vfscanf(FILE *stream, const char *format, va_list ap) {
    return __old_vfscanf(stream, format, ap);
}

int __isoc23_vsscanf(const char *str, const char *format, va_list ap) {
    return __old_vsscanf(str, format, ap);
}

long __isoc23_strtol(const char *nptr, char **endptr, int base) {
    return __old_strtol(nptr, endptr, base);
}

unsigned long __isoc23_strtoul(const char *nptr, char **endptr, int base) {
    return __old_strtoul(nptr, endptr, base);
}

long long __isoc23_strtoll(const char *nptr, char **endptr, int base) {
    return __old_strtoll(nptr, endptr, base);
}

unsigned long long __isoc23_strtoull(const char *nptr, char **endptr, int base) {
    return __old_strtoull(nptr, endptr, base);
}
