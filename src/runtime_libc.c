/* Host versions of hot C library functions the game calls in its own libc (linked module).
 * iOS (proof of concept, branch ios-poc): there is no JIT, so libc's code runs in bbcpu's
 * interpreter, and these few functions took about 70% of the calls into it in play (counted with
 * BB_RECOMP_TARGETS=1). They are standard C contracts; the PS4's wchar_t is 16 bits (its own
 * wcslen walks 16-bit units), so the wide ones work on uint16_t. Elsewhere the translator runs
 * libc fast enough and these stay unregistered. */
#include "runtime.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#if defined(__APPLE__) && TARGET_OS_IPHONE
typedef uint16_t wchar16;

static ABI char *libc_strchr(const char *s, int c) { return strchr(s, c); }
static ABI uint64_t libc_wcslen(const wchar16 *s) {
    const wchar16 *p = s;
    while (*p) ++p;
    return (uint64_t)(p - s);
}
static ABI wchar16 *libc_wmemcpy(wchar16 *d, const wchar16 *s, uint64_t n) { return memcpy(d, s, n * 2); }
static ABI wchar16 *libc_wcschr(const wchar16 *s, uint32_t c) {
    for (;; ++s) {
        if (*s == (wchar16)c) return (wchar16 *)s;
        if (!*s) return 0;
    }
}
static ABI wchar16 *libc_wmemchr(const wchar16 *s, uint32_t c, uint64_t n) {
    for (; n; --n, ++s)
        if (*s == (wchar16)c) return (wchar16 *)s;
    return 0;
}
static ABI int32_t libc_wmemcmp(const wchar16 *a, const wchar16 *b, uint64_t n) {
    for (; n; --n, ++a, ++b)
        if (*a != *b) return *a < *b ? -1 : 1;
    return 0;
}
static ABI int32_t libc_wcscmp(const wchar16 *a, const wchar16 *b) {
    for (; *a == *b; ++a, ++b)
        if (!*a) return 0;
    return *a < *b ? -1 : 1;
}
static ABI int32_t libc_wcsncmp(const wchar16 *a, const wchar16 *b, uint64_t n) {
    for (; n; --n, ++a, ++b) {
        if (*a != *b) return *a < *b ? -1 : 1;
        if (!*a) return 0;
    }
    return 0;
}
static ABI float libc_expf(float x) { return expf(x); }
/* Dinkumware's _FSin(x, qoff): sin(x + qoff * pi/2), so qoff 0 is the sine and 1 the cosine. */
static ABI float libc_fsin(float x, uint32_t qoff) {
    switch (qoff & 3) {
    case 0: return sinf(x);
    case 1: return cosf(x);
    case 2: return -sinf(x);
    default: return -cosf(x);
    }
}

static const RuntimeExport exports[] = {
    {"strchr", libc_strchr}, {"wcslen", libc_wcslen}, {"wmemcpy", libc_wmemcpy}, {"wcschr", libc_wcschr},
    {"wmemchr", libc_wmemchr}, {"wmemcmp", libc_wmemcmp}, {"wcscmp", libc_wcscmp}, {"wcsncmp", libc_wcsncmp},
    {"expf", libc_expf}, {"_FSin", libc_fsin},
};
uintptr_t runtime_libc_resolve(const char *name) { return RUNTIME_LOOKUP(exports, name); }
#else
uintptr_t runtime_libc_resolve(const char *name) { (void)name; return 0; }
#endif
