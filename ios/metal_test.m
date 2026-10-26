/* iOS proof of concept (branch ios-poc), before milestone P2: can Metal use guest memory as it is
 * on iOS (a mapped file, no second view) without that memory counting against the app's limit?
 * BB_IOS_METAL_TEST=MiB maps an unlinked temporary file of that size, wraps it in one Metal
 * buffer without a copy, has the GPU fill every byte, checks a few pages on the CPU and logs the
 * memory still available to the app at each step. */
#import <Metal/Metal.h>

#include <errno.h>
#include <fcntl.h>
#include <os/proc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static double available_gib(void) { return (double)os_proc_available_memory() / (1 << 30); }

void bb_ios_metal_test(unsigned mib) {
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) { printf("metal test: no Metal device\n"); return; }
        printf("metal test: %s, maxBufferLength %.2f GiB, recommendedMaxWorkingSetSize %.2f GiB, "
               "available %.2f GiB\n", device.name.UTF8String, (double)device.maxBufferLength / (1 << 30),
               (double)device.recommendedMaxWorkingSetSize / (1 << 30), available_gib());
        const char *tmp = getenv("TMPDIR");
        char path[1024];
        snprintf(path, sizeof(path), "%s/bb-metal-test", tmp ? tmp : "/tmp");
        int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
        if (fd < 0) { printf("metal test: open: %s\n", strerror(errno)); return; }
        unlink(path);
        const uint64_t size = (uint64_t)mib << 20;
        if (ftruncate(fd, (off_t)size)) { printf("metal test: ftruncate: %s\n", strerror(errno)); return; }
        /* Where guest memory lives: the start of the game's range, reserved at start (platform.h). */
        void *memory = mmap((void *)0x7020000000, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0);
        if (memory == MAP_FAILED) { printf("metal test: mmap: %s\n", strerror(errno)); return; }
        printf("metal test: %u MiB of file-backed memory at %p\n", mib, memory);
        id<MTLBuffer> buffer = [device newBufferWithBytesNoCopy:memory length:size
                                                        options:MTLResourceStorageModeShared deallocator:nil];
        if (!buffer) { printf("metal test: newBufferWithBytesNoCopy refused %u MiB\n", mib); return; }
        printf("metal test: wrapped in a Metal buffer (gpuAddress %#llx); available %.2f GiB\n",
               (unsigned long long)buffer.gpuAddress, available_gib());
        fflush(stdout);
        id<MTLCommandQueue> queue = [device newCommandQueue];
        id<MTLCommandBuffer> commands = [queue commandBuffer];
        id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
        [blit fillBuffer:buffer range:NSMakeRange(0, size) value:0x5a];
        [blit endEncoding];
        [commands commit];
        [commands waitUntilCompleted];
        if (commands.error) { printf("metal test: GPU error: %s\n", commands.error.description.UTF8String); return; }
        const unsigned char *bytes = memory;
        uint64_t wrong = 0, first_wrong = size;
        for (uint64_t at = 0; at < size; at += size / 64)
            if (bytes[at] != 0x5a) { ++wrong; if (at < first_wrong) first_wrong = at; }
        if (wrong) printf("metal test: first wrong sample at %.2f GiB\n", (double)first_wrong / (1 << 30));
        printf("metal test: the GPU filled %u MiB (%llu of 64 sampled pages wrong); available %.2f GiB\n",
               mib, (unsigned long long)wrong, available_gib());
        fflush(stdout);
    }
}
