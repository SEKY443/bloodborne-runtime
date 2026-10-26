/* iOS proof of concept (branch ios-poc): the app around bb-probe. SDL3 runs the iOS application;
 * the runtime (src/probe.c, compiled with -Dmain=bb_probe_main) runs on its own thread with the
 * files the Mac prepared, all in the app's Documents folder (shared with Finder and Files):
 *   game/                 the game folder (CUSA03173 with 1.09 merged)
 *   boot-linked.bin, content.bin, patches.bin   from run.sh on the Mac (runtime out/)
 *   user/                 saves (created when missing)
 *   bb-ios.log            the runtime's output
 * Milestone P1: --cpu-only, nothing drawn. The recompiled library is the app's
 * Frameworks/librecomp.dylib; with no JIT on iOS the rest of the game's code is interpreted. */
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <errno.h>
#include <mach/mach.h>
#include <os/proc.h>
#include <stdint.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int bb_probe_main(int argc, char **argv);

/* Which parts of the address space iOS lets this app reserve (the guest layout must fit there):
 * a 1 GiB fixed reservation tried every 8 GiB up to 1 TiB, then released. */
static void report_address_space(void) {
    printf("iOS PoC: address space probe (1 GiB at each 8 GiB step):\n");
    for (uint64_t at = UINT64_C(8) << 30; at < (UINT64_C(1) << 40); at += UINT64_C(8) << 30) {
        vm_address_t address = (vm_address_t)at;
        kern_return_t result = vm_allocate(mach_task_self(), &address, UINT64_C(1) << 30, VM_FLAGS_FIXED);
        printf("  %#llx: %s\n", (unsigned long long)at, result == KERN_SUCCESS ? "ok" : mach_error_string(result));
        if (result == KERN_SUCCESS) vm_deallocate(mach_task_self(), address, UINT64_C(1) << 30);
    }
    /* The map itself: every region up to the end of the address space, and the free gaps. */
    printf("iOS PoC: regions (start-end size prot/max tag):\n");
    vm_address_t at = 0, previous_end = 0;
    for (;;) {
        vm_size_t size = 0;
        natural_t depth = 0;
        vm_region_submap_info_data_64_t info;
        mach_msg_type_number_t count = VM_REGION_SUBMAP_INFO_COUNT_64;
        if (vm_region_recurse_64(mach_task_self(), &at, &size, &depth, (vm_region_recurse_info_t)&info, &count) != KERN_SUCCESS) break;
        if (at > previous_end && at - previous_end >= (UINT64_C(256) << 20))
            printf("  free %#llx-%#llx (%.1f GiB)\n", (unsigned long long)previous_end, (unsigned long long)at, (double)(at - previous_end) / (1 << 30));
        if (size >= (UINT64_C(256) << 20))
            printf("  %#llx-%#llx %.1f GiB prot %d/%d tag %u\n", (unsigned long long)at, (unsigned long long)(at + size),
                   (double)size / (1 << 30), info.protection, info.max_protection, info.user_tag);
        previous_end = at + size;
        at += size;
    }
    printf("  end of the map at %#llx\n", (unsigned long long)previous_end);
    printf("iOS PoC: memory available to this app before it is stopped: %.2f GiB\n", (double)os_proc_available_memory() / (1 << 30));
    fflush(stdout);
}

static char documents[1024];

static void *run_game(void *unused) {
    (void)unused;
    char boot[1100], content[1100], patches[1100], game[1100], user[1100];
    snprintf(boot, sizeof(boot), "%s/boot-linked.bin", documents);
    snprintf(content, sizeof(content), "%s/content.bin", documents);
    snprintf(patches, sizeof(patches), "%s/patches.bin", documents);
    snprintf(game, sizeof(game), "%s/game", documents);
    snprintf(user, sizeof(user), "%s/user", documents);
    mkdir(user, 0755);
    const char *timeout = getenv("BB_IOS_TIMEOUT");
    char *argv[] = {"bb-probe", boot, "--cpu-only", "--content-profile", content, "--patches", patches,
                    "--app0", game, "--user", user, "--timeout", (char *)(timeout ? timeout : "120"), NULL};
    printf("iOS PoC: starting the runtime (CPU only)\n");
    int code = bb_probe_main((int)(sizeof(argv) / sizeof(*argv)) - 1, argv);
    printf("iOS PoC: the runtime returned %d\n", code);
    fflush(NULL);
    exit(code);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    const char *home = getenv("HOME");
    snprintf(documents, sizeof(documents), "%s/Documents", home ? home : ".");
    if (chdir(documents)) return 1;
    char log[1100];
    snprintf(log, sizeof(log), "%s/bb-ios.log", documents);
    if (freopen(log, "w", stdout)) setvbuf(stdout, NULL, _IOLBF, 0);
    dup2(fileno(stdout), fileno(stderr));
    /* The recompiled library inside the app; the translator's JIT is compiled out on iOS. */
    const char *base = SDL_GetBasePath();
    char library[1100];
    snprintf(library, sizeof(library), "%sFrameworks/librecomp.dylib", base ? base : "");
    if (!access(library, R_OK)) setenv("BB_RECOMP_LIB", library, 0);
    printf("iOS PoC: Documents %s, recompiled library %s\n", documents,
           access(library, R_OK) ? "missing (interpreter only)" : library);
    report_address_space();
    if (getenv("BB_IOS_PROBE_ONLY")) for (;;) { SDL_PumpEvents(); SDL_Delay(100); }
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 64u << 20);
    pthread_t thread;
    if (pthread_create(&thread, &attr, run_game, NULL)) {
        printf("iOS PoC: no runtime thread (%s)\n", strerror(errno));
        return 1;
    }
    /* The main thread keeps the application responsive. */
    for (;;) {
        SDL_PumpEvents();
        SDL_Delay(100);
    }
}
