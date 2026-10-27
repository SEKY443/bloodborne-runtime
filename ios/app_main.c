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
/* In libSystem on iOS too, but not in the iOS SDK headers. */
int proc_regionfilename(int pid, uint64_t address, void *buffer, uint32_t buffersize);
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

/* The log's pipe, file and console (the first initializer sets them before its thread reads). */
static volatile int tee_fds[3] = {-1, -1, -1};
static void *tee_log(void *unused) {
    (void)unused;
    while (tee_fds[0] < 0) usleep(1000);
    char buffer[16384];
    for (;;) {
        const ssize_t n = read(tee_fds[0], buffer, sizeof(buffer));
        if (n <= 0) break;
        write(tee_fds[1], buffer, (size_t)n);
        write(tee_fds[2], buffer, (size_t)n);
    }
    return NULL;
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
    /* Into the log file and to the original console too (devicectl ... --console streams it). */
    static int pipe_fds[2], log_fd, console_fd;
    log_fd = open(log, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    console_fd = dup(2);
    if (log_fd >= 0 && console_fd >= 0 && !pipe(pipe_fds)) {
        dup2(pipe_fds[1], 1);
        dup2(pipe_fds[1], 2);
        setvbuf(stdout, NULL, _IOLBF, 0);
        pthread_t tee;
        if (!pthread_create(&tee, NULL, tee_log, NULL)) pthread_detach(tee);
        tee_fds[0] = pipe_fds[0]; tee_fds[1] = log_fd; tee_fds[2] = console_fd;
    }
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

/* Every 10 s: the address space in use outside the game's ranges, by memory tag, and the largest
 * free gap: what is left for Metal, IOKit and malloc (they fail when it runs out). */
static void *address_space_report(void *unused) {
    (void)unused;
    for (;;) {
        sleep(10);
        uint64_t by_tag[256] = {0}, used = 0, largest_gap = 0, previous_end = UINT64_C(0x100000000);
        vm_address_t at = 0;
        for (;;) {
            vm_size_t size = 0;
            natural_t depth = 0;
            vm_region_submap_info_data_64_t info;
            mach_msg_type_number_t count = VM_REGION_SUBMAP_INFO_COUNT_64;
            if (vm_region_recurse_64(mach_task_self(), &at, &size, &depth, (vm_region_recurse_info_t)&info, &count) != KERN_SUCCESS) break;
            if (at >= UINT64_C(0x180000000) && at < UINT64_C(0x1000000000)) { previous_end = at + size; at += size; continue; } /* shared cache */
            if (at >= UINT64_C(0x1000000000)) break; /* the guard and the game's ranges */
            if (at > previous_end && at - previous_end > largest_gap) largest_gap = at - previous_end;
            by_tag[info.user_tag & 255] += size;
            used += size;
            previous_end = at + size;
            at += size;
        }
        printf("Address space: %.0f MiB used below the shared cache, largest free gap %.0f MiB; by tag:", (double)used / (1 << 20),
               (double)largest_gap / (1 << 20));
        for (int tag = 0; tag < 256; ++tag)
            if (by_tag[tag] >= (UINT64_C(16) << 20)) printf(" %d=%.0f", tag, (double)by_tag[tag] / (1 << 20));
        printf(" MiB\n");
        /* The busiest threads (CPU use as Mach reports it, 100% = one core) with their names. */
        thread_act_array_t threads;
        mach_msg_type_number_t thread_count = 0;
        if (task_threads(mach_task_self(), &threads, &thread_count) == KERN_SUCCESS) {
            struct { double use; char name[64]; } busy[10] = {{0}};
            double total = 0;
            for (mach_msg_type_number_t t = 0; t < thread_count; ++t) {
                thread_extended_info_data_t info;
                mach_msg_type_number_t count = THREAD_EXTENDED_INFO_COUNT;
                if (thread_info(threads[t], THREAD_EXTENDED_INFO, (thread_info_t)&info, &count) == KERN_SUCCESS) {
                    const double use = info.pth_cpu_usage / 10.0;
                    total += use;
                    for (int i = 0; i < 10; ++i)
                        if (use > busy[i].use) {
                            memmove(&busy[i + 1], &busy[i], (9 - i) * sizeof(busy[0]));
                            busy[i].use = use;
                            snprintf(busy[i].name, sizeof(busy[i].name), "%s", info.pth_name);
                            break;
                        }
                }
                mach_port_deallocate(mach_task_self(), threads[t]);
            }
            vm_deallocate(mach_task_self(), (vm_address_t)threads, thread_count * sizeof(*threads));
            printf("Threads: %u, %.0f%% CPU in all; busiest:", thread_count, total);
            for (int i = 0; i < 10 && busy[i].use >= 1.0; ++i) printf(" [%s] %.0f%%", busy[i].name[0] ? busy[i].name : "?", busy[i].use);
            printf("\n");
        }
        /* The largest regions below the shared cache, with the file mapped there if any. */
        struct { uint64_t start, size; unsigned tag; } top[8] = {{0}};
        for (vm_address_t a = 0;;) {
            vm_size_t size = 0;
            natural_t depth = 0;
            vm_region_submap_info_data_64_t info;
            mach_msg_type_number_t count = VM_REGION_SUBMAP_INFO_COUNT_64;
            if (vm_region_recurse_64(mach_task_self(), &a, &size, &depth, (vm_region_recurse_info_t)&info, &count) != KERN_SUCCESS || a >= UINT64_C(0x180000000)) break;
            for (int i = 0; i < 8; ++i)
                if (size > top[i].size) {
                    memmove(&top[i + 1], &top[i], (7 - i) * sizeof(top[0]));
                    top[i].start = a; top[i].size = size; top[i].tag = info.user_tag;
                    break;
                }
            a += size;
        }
        for (int i = 0; i < 8 && top[i].size; ++i) {
            char name[1024] = "";
            proc_regionfilename(getpid(), top[i].start, name, sizeof(name));
            printf("  %#llx %.0f MiB tag %u %s\n", (unsigned long long)top[i].start, (double)top[i].size / (1 << 20), top[i].tag, name);
        }
        fflush(stdout);
    }
    return NULL;
}

static char documents[1024];

static void *run_game(void *unused) {
    (void)unused;
    char boot[1100], content[1100], patches[1100], game[1100], user[1100];
    snprintf(boot, sizeof(boot), "%s/boot-linked.bin", documents);
    snprintf(content, sizeof(content), "%s/content.bin", documents);
    snprintf(patches, sizeof(patches), "%s/patches.bin", documents);
    snprintf(game, sizeof(game), "%s/game", documents);
    /* BB_IOS_SAVE=mac-save: the Mac save copied to Documents/mac-save; otherwise Documents/user. */
    const char *save = getenv("BB_IOS_SAVE");
    snprintf(user, sizeof(user), "%s/%s", documents, save && *save && strcmp(save, "0") ? save : "user");
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
    /* 4608 MiB of direct memory instead of the PS4's 5056: the game allocates about 4589 MiB at
     * start (gameplay checked on the Mac at 4608), and the 448 MiB go back to the system's
     * address space, which ran out when gameplay loaded. */
    setenv("BB_DMEM_MB", "4608", 0);
    /* What run.sh sets on Apple Silicon (the native Metal frame, the 0.3 memory model, copies of
     * GPU buffers at submit, write twins, a 60 Hz vblank), the saves and settings in Documents,
     * English, frame statistics in the log, and the test banner every test run shows. */
    char user_dir[1100], config[1100], grab_trigger[1100], grab_dir[1100];
    snprintf(user_dir, sizeof(user_dir), "%s/user", documents);
    snprintf(config, sizeof(config), "%s/bbport.ini", documents);
    /* tools/grab.sh's frame dumps: the game's own frame when Documents/grab.trigger appears. */
    snprintf(grab_trigger, sizeof(grab_trigger), "%s/grab.trigger", documents);
    snprintf(grab_dir, sizeof(grab_dir), "%s/grab", documents);
    char pad_file[1100];
    snprintf(pad_file, sizeof(pad_file), "%s/pad", documents);
    FILE *pad = fopen(pad_file, "w"); /* empty: nothing held */
    if (pad) fclose(pad);
    char clock_file[1100];
    snprintf(clock_file, sizeof(clock_file), "%s/test.clock", documents);
    FILE *clock_out = fopen(clock_file, "w"); /* empty: the banner counts up until trial.sh sets the end */
    if (clock_out) fclose(clock_out);
    mkdir(grab_dir, 0755);
    const char *defaults[][2] = {
        {"BB_METAL_FRAME", "1"}, {"BB_METAL_NATIVE_BOOT", "1"}, {"BB_GFX_BACKEND", "metal"},
        {"BB_PC_MODEL", "0"}, {"BB_GUEST_IN_PLACE", "0"}, {"BB_UFFD", "0"}, {"BB_PREUPLOAD", "1"},
        {"BB_COPY_GPU_BUFFERS", "1"}, {"BB_GPU_WRITE_TWINS", "1"}, {"BB_GPU_WRITE_TWINS_MAX", "65536"},
        {"BB_VBLANK_HZ", "60"}, {"BB_LANGUAGE", "1"}, {"BB_UI_LANGUAGE", "en"}, {"BB_FRAME_STATS", "1"},
        {"BB_GPU_USER_DIR", user_dir}, {"BB_CONFIG", config},
        {"BB_UI_SCALE", "1.75"},
        {"BB_PAD_FILE", pad_file}, /* scripts/ios/press.sh: buttons pressed from the Mac */
        {"BB_TEST_CLOCK", clock_file}, /* scripts/ios/trial.sh: when it closes the app */
        /* One Metal buffer over guest memory: the Mac's overlapping windows (a MoltenVK
         * workaround) pinned 512 MiB twice, and pinned memory got the app killed. */
        {"BB_POOL_SPLIT", "0"},
        /* 960x540 for now (the patches.bin copied over is made for it): a quarter of 1080p's
         * targets, memory and address space. */
        {"BB_RENDER_RES", "960x540"}, {"BB_OUTPUT_RES", "960x540"}, {"BB_AUTO_RENDER_RES", "1"},
        {"BB_PRESENT_DUMP_TRIGGER", grab_trigger}, {"BB_DUMP_DIR", grab_dir},
        {"BB_TEST_TITLE", "iOS PoC P2: Metal on the iPhone"},
        {"BB_TEST_DETAIL", "Native 1080p, recompiled library, no JIT, file-backed guest memory"},
    };
    for (size_t i = 0; i < sizeof(defaults) / sizeof(*defaults); ++i) setenv(defaults[i][0], defaults[i][1], 0);
    /* bbport.ini's starting values: the FPS counter on, large, at the bottom left (clear of the
     * test banner at the top), and the output the patch was made for (the overlay shows it),
     * unless the file already sets the position. */
    FILE *ini = fopen(config, "r");
    char line[256];
    int has_position = 0;
    while (ini && fgets(line, sizeof(line), ini)) has_position |= !strncmp(line, "fps_position=", 13);
    if (ini) fclose(ini);
    if (!has_position && (ini = fopen(config, "a"))) {
        fputs("show_fps=1\nfps_size=big\nfps_position=bottom_left\noutput_res=960x540\n", ini);
        fclose(ini);
    }
    printf("iOS PoC: Documents %s, recompiled library %s\n", documents,
           access(library, R_OK) ? "missing (interpreter only)" : library);
    report_address_space();
    if (switch_on("BB_IOS_GUARD_TEST")) guard_test();
    if (switch_on("BB_IOS_METAL_TEST")) bb_ios_metal_test((unsigned)strtoul(getenv("BB_IOS_METAL_TEST"), NULL, 10));
    if (switch_on("BB_IOS_FILE_TEST")) file_memory_test((unsigned)strtoul(getenv("BB_IOS_FILE_TEST"), NULL, 10));
    if (switch_on("BB_IOS_PROBE_ONLY")) for (;;) { SDL_PumpEvents(); SDL_Delay(100); }
    pthread_t reporter;
    if (!pthread_create(&reporter, NULL, address_space_report, NULL)) pthread_detach(reporter);
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
