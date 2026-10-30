/* tools/bb_prepare.c GAME_DIR OUT_BOOT_BIN [OUT_CONTENT_BIN [OUT_LINKED_BIN]]: boot.bin, content.bin
 * and boot-linked.bin written by the runtime's own preparation (src/runtime_prepare.c), to compare
 * byte for byte with scripts/prepare.py's, content_profile.py's and link_modules.py's. */
#include <stdio.h>
#include "../src/runtime_prepare.h"

int main(int argc, char **argv) {
    if (argc < 3 || argc > 5) {
        fprintf(stderr, "Usage: %s GAME_DIR OUT_BOOT_BIN [OUT_CONTENT_BIN [OUT_LINKED_BIN]]\n", argv[0]);
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
    int rc = bb_boot_write(&boot, argv[2]);
    if (!rc && argc == 5) {
        if (bb_boot_link(argv[1], &boot, error, sizeof(error))) {
            fprintf(stderr, "link failed: %s\n", error);
            bb_boot_free(&boot);
            return 1;
        }
        printf("linked: %zu modules, %zu imports, %zu native bindings, %zu relocations, image %llu bytes\n",
               boot.module_count, boot.name_count, boot.binding_count, boot.reloc_count, (unsigned long long)boot.size);
        rc = bb_boot_write(&boot, argv[4]);
    }
    bb_boot_free(&boot);
    if (!rc && argc >= 4) {
        BbContentProfile profile;
        if (bb_content_profile(argv[1], 1, &profile, error, sizeof(error))) {
            fprintf(stderr, "content profile failed: %s\n", error);
            return 1;
        }
        rc = bb_content_write(&profile, argv[3]);
    }
    return rc ? 1 : 0;
}
