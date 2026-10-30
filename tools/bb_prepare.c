/* tools/bb_prepare.c GAME_DIR OUT: boot.bin written by the runtime's own preparation
 * (src/runtime_prepare.c), to compare byte for byte with scripts/prepare.py's. */
#include <stdio.h>
#include "../src/runtime_prepare.h"

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s GAME_DIR OUT_BOOT_BIN\n", argv[0]);
        return 2;
    }
    BbBoot boot;
    char error[512];
    if (bb_boot_prepare(argv[1], &boot, error, sizeof(error))) {
        fprintf(stderr, "prepare failed: %s\n", error);
        return 1;
    }
    printf("entry %#llx, image %llu bytes (SHA-256 %s), %zu loads, %zu imports, %zu relocations, _init_env ret %d\n",
           (unsigned long long)boot.entry, (unsigned long long)boot.size, boot.image_sha256, boot.load_count,
           boot.name_count, boot.reloc_count, boot.init_env_is_ret);
    const int rc = bb_boot_write(&boot, argv[2]);
    bb_boot_free(&boot);
    return rc ? 1 : 0;
}
