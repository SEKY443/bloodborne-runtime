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

#include <dlfcn.h>
#include <errno.h>
#include <execinfo.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <mach/mach.h>
#include <os/proc.h>
#include <stdint.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* A test switch: set and not "0" or empty (devicectl keeps an app's earlier launch variables). */
static int switch_on(const char *name) { const char *v = getenv(name); return v && *v && *v != '0'; }

int bb_probe_main(int argc, char **argv);
void bb_ios_metal_test(unsigned mib); /* metal_test.m */

/* Before anything else allocates (and before platform.c reserves the guest ranges, priority
 * 101): the largest fixed reservation from 0x7000000000, i.e. where the free block ends. */
static uint64_t early_free_end;
/* After platform.c reserved the guest ranges (priority 101): the largest malloc left. */
__attribute__((constructor(102))) static void largest_malloc(void) {
    size_t lo = 0, hi = (size_t)8 << 30;
    while (hi - lo > (1 << 20)) {
        const size_t mid = lo + (hi - lo) / 2;
        void *p = malloc(mid);
        if (p) { free(p); lo = mid; } else hi = mid;
    }
    printf("iOS PoC: after the guest reservation the largest malloc is %.0f MiB\n", (double)lo / (1 << 20));
}

/* An abort before main (an initializer): its backtrace into the log. */
static void abort_backtrace(int signal) {
    void *frames[64];
    const int count = backtrace(frames, 64);
    dprintf(2, "iOS PoC: signal %d; backtrace:\n", signal);
    backtrace_symbols_fd(frames, count, 2);
    _exit(128 + signal);
}

__attribute__((constructor(100))) static void measure_free_block(void) {
    /* The log first, before every other initializer: messages from those (libc++'s "terminating
     * due to ...") go to stderr. */
    const char *home = getenv("HOME");
    char log[1100];
    snprintf(log, sizeof(log), "%s/Documents/bb-ios.log", home ? home : ".");
    if (freopen(log, "w", stdout)) setvbuf(stdout, NULL, _IOLBF, 0);
    dup2(fileno(stdout), fileno(stderr));
    printf("iOS PoC: log open (first initializer)\n");
    signal(SIGABRT, abort_backtrace);
    /* The GPU library's paths are made by a static initializer (path_util.cpp: its user folders
     * under BB_GPU_USER_DIR, else under the working directory, "/" before main): set before it. */
    char user[1100];
    snprintf(user, sizeof(user), "%s/Documents/user", home ? home : ".");
    mkdir(user, 0755);
    setenv("BB_GPU_USER_DIR", user, 0);
    for (uint64_t size = UINT64_C(8) << 30; size >= (UINT64_C(256) << 20); size -= UINT64_C(64) << 20) {
        vm_address_t start = (vm_address_t)UINT64_C(0x7000000000);
        if (vm_allocate(mach_task_self(), &start, size, VM_FLAGS_FIXED) == KERN_SUCCESS) {
            vm_deallocate(mach_task_self(), start, size);
            early_free_end = UINT64_C(0x7000000000) + size;
            return;
        }
    }
}

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
    printf("iOS PoC: at start the free block from 0x7000000000 ends at %#llx (%.2f GiB)\n", (unsigned long long)early_free_end,
           early_free_end ? (double)(early_free_end - UINT64_C(0x7000000000)) / (1 << 30) : 0.0);
    printf("iOS PoC: memory available to this app before it is stopped: %.2f GiB\n", (double)os_proc_available_memory() / (1 << 30));
    fflush(stdout);
}

/* BB_IOS_FILE_TEST=MiB: does memory backed by a mapped file count against the app's memory
 * limit? Maps an unlinked temporary file of that size, writes every page and logs the memory
 * still available after each 256 MiB. */
static void file_memory_test(unsigned mib) {
    const char *tmp = getenv("TMPDIR");
    char path[1024];
    snprintf(path, sizeof(path), "%s/bb-file-test", tmp ? tmp : "/tmp");
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) { printf("file test: open: %s\n", strerror(errno)); return; }
    unlink(path);
    uint64_t size = (uint64_t)mib << 20;
    if (ftruncate(fd, (off_t)size)) { printf("file test: ftruncate: %s\n", strerror(errno)); return; }
    unsigned char *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) { printf("file test: mmap: %s\n", strerror(errno)); return; }
    printf("file test: %u MiB mapped at %p; available %.2f GiB\n", mib, (void *)p, (double)os_proc_available_memory() / (1 << 30));
    fflush(stdout);
    for (uint64_t at = 0; at < size; at += 16384) {
        p[at] = 1;
        if ((at + 16384) % (UINT64_C(256) << 20) == 0) {
            printf("file test: %llu MiB written, available %.2f GiB\n", (unsigned long long)((at + 16384) >> 20),
                   (double)os_proc_available_memory() / (1 << 30));
            fflush(stdout);
        }
    }
    printf("file test: done, all %u MiB written and kept mapped\n", mib);
    fflush(stdout);
}

/* BB_IOS_GUARD_TEST=1: can this app release part of the 384 GiB guard reservation
 * (VM_MEMORY_GUARD, 0x1000000000-0x7000000000) and allocate there? */
static void guard_test(void) {
    const vm_address_t at = UINT64_C(0x2000000000);
    const vm_size_t size = UINT64_C(16) << 30;
    kern_return_t freed = vm_deallocate(mach_task_self(), at, size);
    printf("guard test: vm_deallocate 16 GiB at %#llx: %s\n", (unsigned long long)at, mach_error_string(freed));
    vm_address_t address = at;
    kern_return_t made = vm_allocate(mach_task_self(), &address, size, VM_FLAGS_FIXED);
    printf("guard test: vm_allocate there: %s\n", mach_error_string(made));
    void *p = mmap((void *)at, UINT64_C(1) << 30, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    printf("guard test: mmap MAP_FIXED 1 GiB there: %s\n", p == MAP_FAILED ? strerror(errno) : "ok");
    if (p != MAP_FAILED) { ((volatile unsigned char *)p)[0] = 1; printf("guard test: written\n"); }
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
    const int cpu_only = switch_on("BB_IOS_CPU_ONLY");
    char *argv[] = {"bb-probe", boot, "--content-profile", content, "--patches", patches, "--app0", game,
                    "--user", user, "--timeout", (char *)(timeout ? timeout : "0"), cpu_only ? "--cpu-only" : NULL, NULL};
    printf("iOS PoC: starting the runtime (%s)\n", cpu_only ? "CPU only" : "Metal");
    int argc = 0;
    while (argv[argc]) ++argc;
    int code = bb_probe_main(argc, argv);
    printf("iOS PoC: the runtime returned %d\n", code);
    fflush(NULL);
    exit(code);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    const char *home = getenv("HOME");
    snprintf(documents, sizeof(documents), "%s/Documents", home ? home : ".");
    if (chdir(documents)) return 1;
    /* The recompiled library inside the app; the translator's JIT is compiled out on iOS. */
    const char *base = SDL_GetBasePath();
    char library[1100];
    snprintf(library, sizeof(library), "%sFrameworks/librecomp.dylib", base ? base : "");
    if (!access(library, R_OK)) {
        setenv("BB_RECOMP_LIB", library, 0);
        /* Loaded now, while the address space still has a gap of its size (about 400 MB): the
         * renderer's allocations take the gaps later. The runtime's own dlopen finds it loaded. */
        if (!dlopen(library, RTLD_NOW | RTLD_LOCAL)) printf("iOS PoC: early dlopen failed: %s\n", dlerror());
    }
    /* No extra direct memory past the PS4's 5056 MiB: it would cost address space (platform.h). */
    setenv("BB_DMEM_EXTRA_MB", "0", 0);
    /* What run.sh sets on Apple Silicon (the native Metal frame, the 0.3 memory model, copies of
     * GPU buffers at submit, write twins, a 60 Hz vblank), the saves and settings in Documents,
     * English, frame statistics in the log, and the test banner every test run shows. */
    char user_dir[1100], config[1100], grab_trigger[1100], grab_dir[1100];
    snprintf(user_dir, sizeof(user_dir), "%s/user", documents);
    snprintf(config, sizeof(config), "%s/bbport.ini", documents);
    /* tools/grab.sh's frame dumps: the game's own frame when Documents/grab.trigger appears. */
    snprintf(grab_trigger, sizeof(grab_trigger), "%s/grab.trigger", documents);
    snprintf(grab_dir, sizeof(grab_dir), "%s/grab", documents);
    mkdir(grab_dir, 0755);
    const char *defaults[][2] = {
        {"BB_METAL_FRAME", "1"}, {"BB_METAL_NATIVE_BOOT", "1"}, {"BB_GFX_BACKEND", "metal"},
        {"BB_PC_MODEL", "0"}, {"BB_GUEST_IN_PLACE", "0"}, {"BB_UFFD", "0"}, {"BB_PREUPLOAD", "1"},
        {"BB_COPY_GPU_BUFFERS", "1"}, {"BB_GPU_WRITE_TWINS", "1"}, {"BB_GPU_WRITE_TWINS_MAX", "65536"},
        {"BB_VBLANK_HZ", "60"}, {"BB_LANGUAGE", "1"}, {"BB_UI_LANGUAGE", "en"}, {"BB_FRAME_STATS", "1"},
        {"BB_GPU_USER_DIR", user_dir}, {"BB_CONFIG", config},
        {"BB_PRESENT_DUMP_TRIGGER", grab_trigger}, {"BB_DUMP_DIR", grab_dir},
        {"BB_TEST_TITLE", "iOS PoC P2: Metal on the iPhone"},
        {"BB_TEST_DETAIL", "Native 1080p, recompiled library, no JIT, file-backed guest memory"},
    };
    for (size_t i = 0; i < sizeof(defaults) / sizeof(*defaults); ++i) setenv(defaults[i][0], defaults[i][1], 0);
    /* A first bbport.ini: the FPS counter on, large (the test runs' setting). */
    if (access(config, F_OK)) {
        FILE *ini = fopen(config, "w");
        if (ini) { fputs("show_fps=1\nfps_size=big\n", ini); fclose(ini); }
    }
    printf("iOS PoC: Documents %s, recompiled library %s\n", documents,
           access(library, R_OK) ? "missing (interpreter only)" : library);
    report_address_space();
    if (switch_on("BB_IOS_GUARD_TEST")) guard_test();
    if (switch_on("BB_IOS_METAL_TEST")) bb_ios_metal_test((unsigned)strtoul(getenv("BB_IOS_METAL_TEST"), NULL, 10));
    if (switch_on("BB_IOS_FILE_TEST")) file_memory_test((unsigned)strtoul(getenv("BB_IOS_FILE_TEST"), NULL, 10));
    if (switch_on("BB_IOS_PROBE_ONLY")) for (;;) { SDL_PumpEvents(); SDL_Delay(100); }
    if (!switch_on("BB_IOS_CPU_ONLY")) {
        /* The probe on the main thread: it makes the window there and keeps pumping its events
         * (bbgpu_window_loop), as UIKit requires; the game runs on the probe's own thread. */
        run_game(NULL);
    }
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
