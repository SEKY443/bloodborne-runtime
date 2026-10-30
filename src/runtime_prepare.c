/* The game's executable prepared for the loader, in memory (runtime_prepare.h): the port of
 * scripts/prepare.py's boot.bin, checked byte for byte against it (tools/bb_prepare.c). Every
 * offset is checked before use. */
#include "runtime_prepare.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The executable's image this port runs (scripts/game_check.py's SUPPORTED_IMAGE). */
static const char SUPPORTED_IMAGE[] = "071df19c8880086d97182dbc057bc8cb37badaca57d9112683836b24a0444c0a";
/* The NID of _init_env (SHA-1 of the name and the PS4's salt; scripts/prepare.py's nid()). */
static const char INIT_ENV_NID[] = "bzQExy189ZI";

enum { PT_LOAD = 1, PT_DYNAMIC = 2, PT_SCE_DYNLIBDATA = 0x61000000, PT_SCE_RELRO = 0x61000010 };
enum { MAX_IMAGE = 512u << 20 };

/* ---- SHA-256 (FIPS 180-4) ---- */

typedef struct { uint32_t h[8]; uint64_t length; uint8_t block[64]; size_t used; } Sha256;

static const uint32_t SHA256_K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static void sha256_block(Sha256 *s, const uint8_t *p) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; ++i) {
        const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];
    for (int i = 0; i < 64; ++i) {
        const uint32_t t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + SHA256_K[i] + w[i];
        const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
    }
    s->h[0] += a, s->h[1] += b, s->h[2] += c, s->h[3] += d, s->h[4] += e, s->h[5] += f, s->h[6] += g, s->h[7] += h;
}

void bb_sha256_hex(const void *data, size_t size, char out[65]) {
    Sha256 s = {{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19}, 0, {0}, 0};
    const uint8_t *p = data;
    s.length = (uint64_t)size * 8;
    for (; size >= 64; p += 64, size -= 64) sha256_block(&s, p);
    uint8_t last[128] = {0};
    memcpy(last, p, size);
    last[size] = 0x80;
    const size_t blocks = size < 56 ? 1 : 2;
    for (int i = 0; i < 8; ++i) last[blocks * 64 - 1 - i] = (uint8_t)(s.length >> (8 * i));
    for (size_t i = 0; i < blocks; ++i) sha256_block(&s, last + 64 * i);
    for (int i = 0; i < 8; ++i) snprintf(out + 8 * i, 9, "%08x", s.h[i]);
}

/* ---- reading ---- */

typedef struct { const uint8_t *data; size_t size; } Bytes;

static int fail(char *error, size_t error_size, const char *format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(error, error_size, format, args);
    va_end(args);
    return -1;
}

/* Bounds: [offset, offset + size) inside `bytes`. */
static int within(Bytes bytes, uint64_t offset, uint64_t size) {
    return offset <= bytes.size && size <= bytes.size - offset;
}
static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t u32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t u64(const uint8_t *p) { return (uint64_t)u32(p) | (uint64_t)u32(p + 4) << 32; }

static int read_file(const char *path, Bytes *out) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    uint8_t *data = NULL;
    size_t size = 0, capacity = 0;
    for (;;) {
        if (size == capacity) {
            capacity = capacity ? capacity * 2 : 1u << 20;
            uint8_t *grown = realloc(data, capacity);
            if (!grown) { free(data); fclose(f); return -1; }
            data = grown;
        }
        const size_t got = fread(data + size, 1, capacity - size, f);
        size += got;
        if (got == 0) break;
    }
    fclose(f);
    out->data = data, out->size = size;
    return 0;
}

typedef struct { uint32_t type, flags; uint64_t offset, vaddr, filesz, memsz; } Phdr;
typedef struct { uint8_t *elf; size_t elf_size; uint64_t entry; Phdr ph[256]; int count; } Elf;

static int read_phdrs(Bytes data, uint64_t base, Elf *out, char *error, size_t error_size) {
    if (!within(data, base, 64)) return fail(error, error_size, "out of bounds: the ELF header");
    const uint8_t *h = data.data + base;
    if (memcmp(h, "\x7f" "ELF\x02\x01\x01", 7) || u16(h + 18) != 62)
        return fail(error, error_size, "expected little-endian x86-64 ELF");
    const uint64_t phoff = u64(h + 32);
    const unsigned phentsize = u16(h + 54), phnum = u16(h + 56);
    if (phentsize != 56 || phnum == 0 || phnum >= 256) return fail(error, error_size, "unsupported program headers");
    if (!within(data, base + phoff, (uint64_t)phnum * 56)) return fail(error, error_size, "out of bounds: program headers");
    out->entry = u64(h + 24);
    out->count = (int)phnum;
    for (unsigned i = 0; i < phnum; ++i) {
        const uint8_t *p = data.data + base + phoff + i * 56;
        out->ph[i] = (Phdr){u32(p), u32(p + 4), u64(p + 8), u64(p + 16), u64(p + 32), u64(p + 40)};
    }
    return 0;
}

/* scripts/prepare.py's parse_self: a plaintext SELF (or a plain ELF) as the ELF file it wraps. */
static int parse_self(Bytes data, Elf *out, char *error, size_t error_size) {
    memset(out, 0, sizeof(*out));
    if (!within(data, 0, 4)) return fail(error, error_size, "out of bounds: the file is empty");
    if (!memcmp(data.data, "\x7f" "ELF", 4)) {
        if (read_phdrs(data, 0, out, error, error_size)) return -1;
        const uint8_t *h = data.data;
        uint64_t end = u64(h + 32) + (uint64_t)out->count * 56;
        for (int i = 0; i < out->count; ++i)
            if (out->ph[i].offset + out->ph[i].filesz > end) end = out->ph[i].offset + out->ph[i].filesz;
        if (end > MAX_IMAGE) return fail(error, error_size, "probe image exceeds 512 MiB limit");
        if (!within(data, 0, end)) return fail(error, error_size, "out of bounds: the ELF's segments");
        out->elf = malloc(end ? end : 1);
        if (!out->elf) return fail(error, error_size, "out of memory");
        memcpy(out->elf, data.data, end);
        out->elf_size = end;
        return 0;
    }
    if (memcmp(data.data, "O\x15=\x1d", 4))
        return fail(error, error_size, "expected PS4 SELF or ELF (eboot.bin is neither: an encrypted or damaged dump?)");
    if (!within(data, 24, 2)) return fail(error, error_size, "out of bounds: the SELF header");
    const unsigned count = u16(data.data + 24);
    const uint64_t base = 32 + (uint64_t)count * 32;
    if (read_phdrs(data, base, out, error, error_size)) return -1;
    uint64_t end = 0;
    for (int i = 0; i < out->count; ++i)
        if (out->ph[i].offset + out->ph[i].filesz > end) end = out->ph[i].offset + out->ph[i].filesz;
    if (end > MAX_IMAGE) return fail(error, error_size, "probe image exceeds 512 MiB limit");
    const uint64_t header_end = u64(data.data + base + 32) + (uint64_t)out->count * 56;
    if (header_end > end) end = header_end;
    out->elf = calloc(1, end ? end : 1);
    if (!out->elf) return fail(error, error_size, "out of memory");
    out->elf_size = end;
    memcpy(out->elf, data.data + base, header_end);
    uint64_t covered[257][2] = {{0, header_end}};
    int covered_count = 1;
    for (unsigned i = 0; i < count; ++i) {
        const uint8_t *s = data.data + 32 + i * 32;
        const uint64_t flags = u64(s), offset = u64(s + 8), size = u64(s + 16), mem = u64(s + 24);
        if (!(flags & 0x800)) continue;
        if (flags & 10) return fail(error, error_size, "encrypted/compressed SELF segment is unsupported");
        const unsigned index = (unsigned)(flags >> 20) & 4095;
        if (index >= (unsigned)out->count) return fail(error, error_size, "invalid segment index");
        const Phdr *p = &out->ph[index];
        if (size != p->filesz || size != mem) return fail(error, error_size, "unsupported blocked segment layout");
        if (!within(data, offset, size)) return fail(error, error_size, "out of bounds: a SELF segment");
        memcpy(out->elf + p->offset, data.data + offset, size);
        if (covered_count < 257) covered[covered_count][0] = p->offset, covered[covered_count++][1] = p->offset + size;
    }
    for (int i = 0; i < out->count; ++i) {
        const Phdr *p = &out->ph[i];
        if (!p->filesz) continue;
        int found = 0;
        for (int c = 0; c < covered_count && !found; ++c)
            found = covered[c][0] <= p->offset && p->offset + p->filesz <= covered[c][1];
        if (!found && (p->type == PT_LOAD || p->type == PT_DYNAMIC || p->type == PT_SCE_DYNLIBDATA || p->type == PT_SCE_RELRO))
            return fail(error, error_size, "required segment %d is unavailable", i);
    }
    return 0;
}

static const Phdr *first_of(const Elf *elf, uint32_t type) {
    for (int i = 0; i < elf->count; ++i)
        if (elf->ph[i].type == type) return &elf->ph[i];
    return NULL;
}

/* The dynamic tags (later ones win, as a Python dict built from them); `stop_at_null`: up to DT_NULL. */
typedef struct { uint64_t tag, value; } Tag;
static uint64_t tag_value(const Tag *tags, size_t count, uint64_t tag, int *found) {
    *found = 0;
    uint64_t value = 0;
    for (size_t i = 0; i < count; ++i)
        if (tags[i].tag == tag) value = tags[i].value, *found = 1;
    return value;
}

static int read_tags(const Elf *elf, int stop_at_null, Tag **out, size_t *count, char *error, size_t error_size) {
    const Phdr *dp = first_of(elf, PT_DYNAMIC);
    if (!dp) return fail(error, error_size, "no dynamic segment");
    const Bytes e = {elf->elf, elf->elf_size};
    if (!within(e, dp->offset, dp->filesz)) return fail(error, error_size, "out of bounds: the dynamic segment");
    Tag *tags = malloc((dp->filesz / 16 + 1) * sizeof(*tags));
    if (!tags) return fail(error, error_size, "out of memory");
    size_t n = 0;
    for (uint64_t pos = dp->offset; pos + 16 <= dp->offset + dp->filesz; pos += 16) {
        const uint64_t tag = u64(elf->elf + pos), value = u64(elf->elf + pos + 8);
        if (stop_at_null && tag == 0) break;
        tags[n++] = (Tag){tag, value};
    }
    *out = tags, *count = n;
    return 0;
}

typedef struct { Bytes strings, syms; } SymbolTables;

static int symbol_tables(const Elf *elf, const Tag *tags, size_t count, SymbolTables *out, char *error, size_t error_size) {
    const Phdr *lib = first_of(elf, PT_SCE_DYNLIBDATA);
    if (!lib) return fail(error, error_size, "no SCE dynamic library data");
    const Bytes e = {elf->elf, elf->elf_size};
    if (!within(e, lib->offset, lib->filesz)) return fail(error, error_size, "out of bounds: the dynamic library data");
    const Bytes blob = {elf->elf + lib->offset, lib->filesz};
    int a, b, c, d;
    const uint64_t str_off = tag_value(tags, count, 0x61000035, &a), str_size = tag_value(tags, count, 0x61000037, &b);
    const uint64_t sym_off = tag_value(tags, count, 0x61000039, &c), sym_size = tag_value(tags, count, 0x6100003f, &d);
    if (!a || !b || !c || !d) return fail(error, error_size, "missing symbol table tags");
    if (!within(blob, str_off, str_size) || !within(blob, sym_off, sym_size))
        return fail(error, error_size, "out of bounds: the symbol tables");
    out->strings = (Bytes){blob.data + str_off, str_size};
    out->syms = (Bytes){blob.data + sym_off, sym_size};
    return 0;
}

/* A NUL-terminated name at `offset` of the string table, or NULL. */
static const char *name_at(Bytes strings, uint32_t offset) {
    if (offset >= strings.size) return NULL;
    const void *end = memchr(strings.data + offset, 0, strings.size - offset);
    return end ? (const char *)strings.data + offset : NULL;
}

/* scripts/prepare.py's inspect_libc: whether libc.prx's _init_env is exactly `ret`. */
static int init_env_is_ret(const char *game_dir, int *result, char *error, size_t error_size) {
    char path[4096];
    snprintf(path, sizeof(path), "%s/sce_module/libc.prx", game_dir);
    Bytes data;
    if (read_file(path, &data)) return fail(error, error_size, "cannot read %s", path);
    Elf elf;
    int rc = parse_self(data, &elf, error, error_size);
    free((void *)data.data);
    if (rc) return -1;
    Tag *tags = NULL;
    size_t count = 0;
    SymbolTables tables;
    rc = read_tags(&elf, 0, &tags, &count, error, error_size) || symbol_tables(&elf, tags, count, &tables, error, error_size);
    *result = 0;
    for (size_t pos = 0; !rc && pos + 24 <= tables.syms.size; pos += 24) {
        const uint8_t *s = tables.syms.data + pos;
        const char *name = name_at(tables.strings, u32(s));
        if (!name) { rc = fail(error, error_size, "libc.prx: a symbol name out of bounds"); break; }
        const size_t key = strcspn(name, "#");
        const uint8_t info = s[4];
        const uint16_t section = u16(s + 6);
        const uint64_t value = u64(s + 8), size = u64(s + 16);
        if (key != strlen(INIT_ENV_NID) || strncmp(name, INIT_ENV_NID, key) || !section || (info & 15) != 2) continue;
        for (int i = 0; i < elf.count; ++i) {
            const Phdr *p = &elf.ph[i];
            if (p->type != PT_LOAD || !(p->flags & 1) || value < p->vaddr || value >= p->vaddr + p->filesz) continue;
            const Bytes e = {elf.elf, elf.elf_size};
            const uint64_t at = p->offset + value - p->vaddr;
            if (!within(e, at, size)) { rc = fail(error, error_size, "libc.prx: _init_env out of bounds"); break; }
            *result = size == 1 && elf.elf[at] == 0xc3;
            break;
        }
    }
    free(tags);
    free(elf.elf);
    return rc ? -1 : 0;
}

/* ---- the boot image ---- */

void bb_boot_free(BbBoot *boot) {
    free(boot->loads), free(boot->names), free(boot->relocs), free(boot->image);
    memset(boot, 0, sizeof(*boot));
}

/* [addr, addr + size) inside one loadable segment's memory. */
static int mapped(const Phdr *const *loads, int count, uint64_t addr, uint64_t size) {
    for (int i = 0; i < count; ++i)
        if (loads[i]->vaddr <= addr && addr + size <= loads[i]->vaddr + loads[i]->memsz) return 1;
    return 0;
}

static int push_reloc(BbBoot *boot, size_t *capacity, BbBootReloc reloc) {
    if (boot->reloc_count == *capacity) {
        *capacity = *capacity ? *capacity * 2 : 1024;
        BbBootReloc *grown = realloc(boot->relocs, *capacity * sizeof(*grown));
        if (!grown) return -1;
        boot->relocs = grown;
    }
    boot->relocs[boot->reloc_count++] = reloc;
    return 0;
}

int bb_boot_prepare(const char *game_dir, BbBoot *boot, char *error, size_t error_size) {
    memset(boot, 0, sizeof(*boot));
    char path[4096];
    snprintf(path, sizeof(path), "%s/eboot.bin", game_dir);
    Bytes source;
    if (read_file(path, &source)) return fail(error, error_size, "cannot read %s", path);
    Elf elf;
    int rc = parse_self(source, &elf, error, error_size);
    free((void *)source.data);
    if (rc) return -1;
    Tag *tags = NULL;
    size_t tag_count = 0;
    int32_t *import_of = NULL; /* by symbol index: its import's index, or -1 */
    size_t reloc_capacity = 0;
    rc = -1;

    /* The loadable segments where they load. */
    const Phdr *loads[256];
    int load_count = 0;
    uint64_t size = 0;
    for (int i = 0; i < elf.count; ++i)
        if (elf.ph[i].type == PT_LOAD || elf.ph[i].type == PT_SCE_RELRO) {
            loads[load_count++] = &elf.ph[i];
            if (elf.ph[i].vaddr + elf.ph[i].memsz > size) size = elf.ph[i].vaddr + elf.ph[i].memsz;
        }
#define MAPPED(addr, n) mapped(loads, load_count, (addr), (n))
    int entry_mapped = 0;
    for (int i = 0; i < load_count; ++i)
        entry_mapped |= loads[i]->vaddr <= elf.entry && elf.entry + 1 <= loads[i]->vaddr + loads[i]->memsz;
    if (size > MAX_IMAGE || !entry_mapped) { fail(error, error_size, "invalid memory image"); goto done; }
    boot->image = calloc(1, size ? size : 1);
    boot->loads = calloc((size_t)load_count, sizeof(*boot->loads));
    if (!boot->image || !boot->loads) { fail(error, error_size, "out of memory"); goto done; }
    boot->size = size, boot->entry = elf.entry, boot->load_count = (size_t)load_count;
    const Bytes e = {elf.elf, elf.elf_size};
    for (int i = 0; i < load_count; ++i) {
        const Phdr *p = loads[i];
        if (p->filesz > p->memsz) { fail(error, error_size, "segment file size exceeds memory size"); goto done; }
        if (!within(e, p->offset, p->filesz)) { fail(error, error_size, "out of bounds: a loadable segment"); goto done; }
        memcpy(boot->image + p->vaddr, elf.elf + p->offset, p->filesz);
        boot->loads[i] = (BbBootLoad){p->vaddr, p->memsz, p->flags};
    }
    /* Only the supported executable runs (scripts/game_check.py); others fail in the game's code. */
    bb_sha256_hex(boot->image, size, boot->image_sha256);
    const char *skip = getenv("BB_SKIP_GAME_CHECK");
    if (!(skip && skip[0] == '1') && strcmp(boot->image_sha256, SUPPORTED_IMAGE)) {
        fail(error, error_size, "this is not Bloodborne 1.09's executable (image %.12s...): only update 1.09 runs",
             boot->image_sha256);
        goto done;
    }

    /* Relocations: base-relative, or against the executable's own or imported symbols. */
    SymbolTables tables;
    if (read_tags(&elf, 1, &tags, &tag_count, error, error_size) ||
        symbol_tables(&elf, tags, tag_count, &tables, error, error_size))
        goto done;
    if (tables.syms.size % 24) { fail(error, error_size, "bad symbol table size"); goto done; }
    const size_t symbol_count = tables.syms.size / 24;
    for (size_t i = 0; i < symbol_count; ++i)
        if (!name_at(tables.strings, u32(tables.syms.data + i * 24))) { fail(error, error_size, "out of bounds: a symbol name"); goto done; }
    import_of = malloc((symbol_count ? symbol_count : 1) * sizeof(*import_of));
    if (!import_of) { fail(error, error_size, "out of memory"); goto done; }
    for (size_t i = 0; i < symbol_count; ++i) import_of[i] = -1;
    static const uint64_t TABLES[2][2] = {{0x61000029, 0x6100002d}, {0x6100002f, 0x61000031}};
    const Phdr *lib = first_of(&elf, PT_SCE_DYNLIBDATA);
    const Bytes blob = {elf.elf + lib->offset, lib->filesz};
    for (int t = 0; t < 2; ++t) {
        int a, b;
        const uint64_t offset = tag_value(tags, tag_count, TABLES[t][0], &a), length = tag_value(tags, tag_count, TABLES[t][1], &b);
        if (!a || !b || !within(blob, offset, length)) { fail(error, error_size, "out of bounds: a relocation table"); goto done; }
        if (length % 24) { fail(error, error_size, "bad relocation table size"); goto done; }
        for (uint64_t pos = offset; pos < offset + length; pos += 24) {
            const uint64_t target = u64(blob.data + pos), info = u64(blob.data + pos + 8);
            const int64_t addend = (int64_t)u64(blob.data + pos + 16);
            const uint32_t kind = (uint32_t)info, sym = (uint32_t)(info >> 32);
            if (!MAPPED(target, 8)) { fail(error, error_size, "relocation outside mapped segments: %#llx", (unsigned long long)target); goto done; }
            if (kind == 8) { /* R_X86_64_RELATIVE */
                if (push_reloc(boot, &reloc_capacity, (BbBootReloc){target, 0, addend, 0})) { fail(error, error_size, "out of memory"); goto done; }
            } else if (kind == 1 || kind == 6 || kind == 7) { /* R_X86_64_64, GLOB_DAT, JUMP_SLOT */
                if (sym >= symbol_count) { fail(error, error_size, "bad symbol index"); goto done; }
                const uint8_t *s = tables.syms.data + (size_t)sym * 24;
                const uint16_t section = u16(s + 6);
                const uint64_t value = u64(s + 8);
                if (section == 0xfff1) { /* absolute: written now */
                    const uint64_t v = value + (uint64_t)addend;
                    memcpy(boot->image + target, &v, 8);
                } else if (section) {
                    if (push_reloc(boot, &reloc_capacity, (BbBootReloc){target, 0, (int64_t)(value + (uint64_t)addend), 0})) { fail(error, error_size, "out of memory"); goto done; }
                } else {
                    if (import_of[sym] < 0) import_of[sym] = (int32_t)boot->name_count++;
                    const uint64_t import_kind = (s[4] & 15) == 1 ? 2 : 1; /* STT_OBJECT: data */
                    if ((import_kind == 1 && addend) || addend < 0 || addend >= 4096) { fail(error, error_size, "unsupported import addend"); goto done; }
                    if (push_reloc(boot, &reloc_capacity, (BbBootReloc){target, import_kind, import_of[sym], addend})) { fail(error, error_size, "out of memory"); goto done; }
                }
            } else {
                fail(error, error_size, "unsupported relocation %u", kind);
                goto done;
            }
        }
    }
    boot->names = calloc(boot->name_count ? boot->name_count : 1, sizeof(*boot->names));
    if (!boot->names) { fail(error, error_size, "out of memory"); goto done; }
    for (size_t i = 0; i < symbol_count; ++i) {
        if (import_of[i] < 0) continue;
        const char *name = name_at(tables.strings, u32(tables.syms.data + i * 24));
        if (strlen(name) >= 128) { fail(error, error_size, "import name too long"); goto done; }
        strcpy(boot->names[import_of[i]], name);
    }
    if (init_env_is_ret(game_dir, &boot->init_env_is_ret, error, error_size)) goto done;
    rc = 0;
done:
#undef MAPPED
    free(tags);
    free(import_of);
    free(elf.elf);
    if (rc) bb_boot_free(boot);
    return rc;
}

int bb_boot_write(const BbBoot *boot, const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    uint8_t header[56];
    const uint64_t fields[6] = {boot->size, boot->entry, boot->load_count, boot->reloc_count, boot->name_count,
                                (uint64_t)boot->init_env_is_ret};
    memcpy(header, "BBPROBE2", 8);
    for (int i = 0; i < 6; ++i)
        for (int b = 0; b < 8; ++b) header[8 + 8 * i + b] = (uint8_t)(fields[i] >> (8 * b));
    int ok = fwrite(header, 1, sizeof(header), f) == sizeof(header);
    for (size_t i = 0; ok && i < boot->load_count; ++i) {
        uint8_t load[24];
        const uint64_t v[3] = {boot->loads[i].vaddr, boot->loads[i].memsz, boot->loads[i].flags};
        for (int k = 0; k < 3; ++k)
            for (int b = 0; b < 8; ++b) load[8 * k + b] = (uint8_t)(v[k] >> (8 * b));
        ok = fwrite(load, 1, sizeof(load), f) == sizeof(load);
    }
    for (size_t i = 0; ok && i < boot->name_count; ++i) ok = fwrite(boot->names[i], 1, 128, f) == 128;
    for (size_t i = 0; ok && i < boot->reloc_count; ++i) {
        uint8_t r[32];
        const uint64_t v[4] = {boot->relocs[i].target, boot->relocs[i].kind, (uint64_t)boot->relocs[i].value,
                               (uint64_t)boot->relocs[i].addend};
        for (int k = 0; k < 4; ++k)
            for (int b = 0; b < 8; ++b) r[8 * k + b] = (uint8_t)(v[k] >> (8 * b));
        ok = fwrite(r, 1, sizeof(r), f) == sizeof(r);
    }
    ok = ok && fwrite(boot->image, 1, boot->size, f) == boot->size;
    return fclose(f) == 0 && ok ? 0 : -1;
}

/* ---- the AppContent profile (scripts/content_profile.py) ---- */

/* param.sfo's integer value for `key`, or 0 when absent; -1 when the file is damaged. */
static int sfo_int(Bytes sfo, const char *key, uint32_t *value) {
    *value = 0;
    if (!within(sfo, 0, 20) || memcmp(sfo.data, "\0PSF", 4)) return -1;
    const uint32_t keys = u32(sfo.data + 8), values = u32(sfo.data + 12), count = u32(sfo.data + 16);
    for (uint32_t i = 0; i < count; ++i) {
        if (!within(sfo, 20 + (uint64_t)i * 16, 16)) return -1;
        const uint8_t *entry = sfo.data + 20 + i * 16;
        const uint32_t name = keys + u16(entry), size = u32(entry + 4), offset = values + u32(entry + 12);
        const uint16_t format = u16(entry + 2);
        if (name >= sfo.size || !memchr(sfo.data + name, 0, sfo.size - name)) return -1;
        if (strcmp((const char *)sfo.data + name, key)) continue;
        if (format != 0x404 || size != 4 || !within(sfo, offset, 4)) return -1; /* not an integer */
        *value = u32(sfo.data + offset);
    }
    return 0;
}

int bb_content_profile(const char *game_dir, int full, BbContentProfile *out, char *error, size_t error_size) {
    char path[4096];
    snprintf(path, sizeof(path), "%s/sce_sys/param.sfo", game_dir);
    Bytes sfo;
    if (read_file(path, &sfo)) return fail(error, error_size, "cannot read %s", path);
    out->sku = full ? 3 : 1;
    int rc = 0;
    for (int i = 0; i < 4 && !rc; ++i) {
        char key[32];
        snprintf(key, sizeof(key), "USER_DEFINED_PARAM_%d", i + 1);
        if (sfo_int(sfo, key, &out->user_params[i])) rc = fail(error, error_size, "invalid user-defined parameter");
    }
    free((void *)sfo.data);
    return rc;
}

int bb_content_write(const BbContentProfile *profile, const char *path) {
    uint8_t bytes[28];
    memcpy(bytes, "BBCONT01", 8);
    const uint32_t fields[5] = {profile->sku, profile->user_params[0], profile->user_params[1],
                                profile->user_params[2], profile->user_params[3]};
    for (int i = 0; i < 5; ++i)
        for (int b = 0; b < 4; ++b) bytes[8 + 4 * i + b] = (uint8_t)(fields[i] >> (8 * b));
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    const int ok = fwrite(bytes, 1, sizeof(bytes), f) == sizeof(bytes);
    return fclose(f) == 0 && ok ? 0 : -1;
}
