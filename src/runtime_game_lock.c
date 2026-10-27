/* The game's spin-then-sleep mutex as host code (iOS proof of concept, branch ios-poc).
 * Its heap and several other subsystems lock it; the waiters spin on the lock word until a TSC
 * deadline, then count themselves in and sleep on the mutex's semaphore. On the phone (6 cores, 2
 * of them fast, the game's code without a JIT) eight threads spinning took 57% of the CPU in play
 * (Time Profiler) and starved the holder. Same protocol here, but a short spin with the CPU's
 * yield before sleeping. The lock word (at +0x18): owner thread id in the low 40 bits (all ones:
 * handed to a woken waiter), waiters above; the semaphore handle at +0x10.
 * BB_GAME_LOCK=0 leaves both functions to the game's code. */
#include "runtime.h"
#include "cpu/bbcpu.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__APPLE__) && TARGET_OS_IPHONE && defined(BB_HAVE_BBCPU)
enum {
    LOCK = 0x20368c0, LOCK_SIZE = 0xe8,     /* lock(mutex) -> 0 */
    UNLOCK = 0x2036a40, UNLOCK_SIZE = 0x94, /* unlock(mutex) -> 0 */
    THREAD_ID = 0x2bbfe78,                  /* the import stubs the game's versions call */
    SEMA_WAIT = 0x2bbec78, SEMA_SIGNAL = 0x2bbec88,
    SPINS = 128,
};
#define OWNER 0xffffffffffull
#define WAITER (1ull << 40)

static uint64_t image;
static uint64_t game(uint64_t offset, int count, uint64_t a, uint64_t b) {
    const uint64_t args[3] = {a, b, 0};
    return bbcpu_call((uintptr_t)(image + offset), count, args);
}
static uint64_t self_id(void) {
    static __thread uint64_t id;
    if (!id) id = game(THREAD_ID, 0, 0, 0) & OWNER;
    return id;
}

static ABI int32_t game_lock(uint64_t mutex) {
    uint64_t *word = (uint64_t *)(uintptr_t)(mutex + 0x18);
    const uint64_t self = self_id();
    for (int spin = 0;; ++spin) {
        uint64_t w = __atomic_load_n(word, __ATOMIC_ACQUIRE);
        const uint64_t owner = w & OWNER;
        if (!owner || owner == self) {
            if (__atomic_compare_exchange_n(word, &w, (w & ~OWNER) | self, 0, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED))
                return 0;
        } else if (spin < SPINS) {
            __builtin_arm_yield();
        } else if (__atomic_compare_exchange_n(word, &w, w + WAITER, 0, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED)) {
            /* Woken by unlock, which hands the mutex over (owner all ones). */
            if ((int32_t)game(SEMA_WAIT, 3, *(uint64_t *)(uintptr_t)(mutex + 0x10), 1))
                fprintf(stderr, "Game lock: semaphore wait failed\n");
            return 0;
        }
    }
}

static ABI int32_t game_unlock(uint64_t mutex) {
    uint64_t *word = (uint64_t *)(uintptr_t)(mutex + 0x18);
    uint64_t w = __atomic_load_n(word, __ATOMIC_RELAXED), next;
    do next = w >> 40 ? (w - WAITER) | OWNER : w & ~OWNER;
    while (!__atomic_compare_exchange_n(word, &w, next, 0, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED));
    if (next & OWNER) game(SEMA_SIGNAL, 2, *(uint64_t *)(uintptr_t)(mutex + 0x10), 1);
    return 0;
}

void runtime_game_lock_load(uint64_t image_base) {
    const char *env = getenv("BB_GAME_LOCK");
    if (env && *env == '0') return;
    /* Their first instructions, checked: another build of the game keeps its own. */
    static const uint8_t lock_code[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53, 0x49, 0x89, 0xfe};
    static const uint8_t unlock_code[] = {0x55, 0x48, 0x89, 0xe5, 0x48, 0x8d, 0x57, 0x18};
    if (memcmp((const void *)(uintptr_t)(image_base + LOCK), lock_code, sizeof(lock_code)) ||
        memcmp((const void *)(uintptr_t)(image_base + UNLOCK), unlock_code, sizeof(unlock_code))) {
        printf("Game lock: unexpected code, left to the game\n");
        return;
    }
    image = image_base;
    bbcpu_set_native(image_base + LOCK, (const void *)game_lock);
    bbcpu_set_native(image_base + UNLOCK, (const void *)game_unlock);
    bbcpu_recomp_exclude(image_base + LOCK, LOCK_SIZE);
    bbcpu_recomp_exclude(image_base + UNLOCK, UNLOCK_SIZE);
    printf("Game lock: host version (spin %d, then the game's semaphore)\n", SPINS);
}
#else
void runtime_game_lock_load(uint64_t image_base) { (void)image_base; }
#endif
