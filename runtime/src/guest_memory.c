/* Guest address space, PE image loader, thread block and guest heap. */
#include "guest_memory.h"

#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "recomp_types.h"
#include "winapi/imports.h"

ptrdiff_t g_xbox_mem_offset;
uint32_t g_xbox_code_lo, g_xbox_code_hi;

#define GUEST_SPACE (1ull << 32)

void guest_memory_init(void)
{
    /* Anonymous memory is committed lazily on macOS, so one 4 GB RW reservation
     * costs nothing until touched. Page 0 stays inaccessible to trap NULL. */
    void *base = mmap(NULL, GUEST_SPACE, PROT_READ | PROT_WRITE,
                      MAP_ANON | MAP_PRIVATE, -1, 0);
    if (base == MAP_FAILED) rt_fatal("cannot reserve 4 GB guest address space");
    mprotect(base, 0x10000, PROT_NONE);
    g_xbox_mem_offset = (ptrdiff_t)base;
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

void guest_load_pe(const char *path, GuestImage *out)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) rt_fatal("cannot open %s", path);
    struct stat st;
    fstat(fd, &st);
    uint8_t *f = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (f == MAP_FAILED || st.st_size < 0x400 || f[0] != 'M' || f[1] != 'Z')
        rt_fatal("%s is not a PE file", path);

    const uint8_t *pe = f + rd32(f + 0x3C);
    const uint8_t *opt = pe + 24;
    uint32_t base = rd32(opt + 28);
    if (base != GUEST_IMAGE_BASE) rt_fatal("unexpected image base %08X", base);
    unsigned nsec = rd16(pe + 6);
    uint32_t hdr_size = rd32(opt + 60);
    out->entry = base + rd32(opt + 16);
    out->image_size = rd32(opt + 56);
    out->code_lo = out->code_hi = 0;

    memcpy(GPTR(base), f, hdr_size);
    const uint8_t *sh = opt + rd16(pe + 20);
    for (unsigned i = 0; i < nsec; i++, sh += 40) {
        uint32_t vsize = rd32(sh + 8), va = base + rd32(sh + 12);
        uint32_t rsize = rd32(sh + 16), roff = rd32(sh + 20), chars = rd32(sh + 36);
        uint32_t n = rsize < vsize || !vsize ? rsize : vsize;
        memcpy(GPTR(va), f + roff, n);
        if (chars & 0x20000020u) { /* code */
            if (!out->code_lo || va < out->code_lo) out->code_lo = va;
            if (va + vsize > out->code_hi) out->code_hi = va + vsize;
        }
    }
    munmap(f, (size_t)st.st_size);
    g_xbox_code_lo = out->code_lo;
    g_xbox_code_hi = out->code_hi;
}

void guest_tib_init(void)
{
    /* Win32 TIB: fs:[0] SEH chain, [4] stack base, [8] stack limit, [0x18] self,
     * [0x20] process id, [0x24] thread id, [0x30] PEB. */
    uint32_t t = GUEST_TIB_VA;
    memset(GPTR(t), 0, 0x1000);
    MEM32(t + 0x00) = 0xFFFFFFFFu;
    MEM32(t + 0x04) = GUEST_STACK_HI;
    MEM32(t + 0x08) = GUEST_STACK_LO;
    MEM32(t + 0x18) = t;
    MEM32(t + 0x20) = 0x1000;
    MEM32(t + 0x24) = 0x1004;
    MEM32(t + 0x30) = t + 0x800;  /* zeroed PEB */
    g_fs_base = t;
    g_esp = GUEST_STACK_HI - 64;
}

/* ---- guest heap ------------------------------------------------------------
 * Every block has an 8-byte header {size, magic} just below the returned VA.
 * Small blocks (<= 4 KB) use 16-byte size classes with free lists; larger
 * blocks use a first-fit free list. Fresh space comes from a bump pointer. */

#define HDR 8u
#define MAGIC_USED 0x47544132u /* 'GTA2' */
#define MAGIC_FREE 0x46524545u
#define SMALL_MAX 4096u
#define NCLASS (SMALL_MAX / 16u + 1u)

static uint32_t g_bump = GUEST_HEAP_LO;
static uint32_t g_small_free[NCLASS];
static uint32_t g_large_free; /* singly linked via the first payload word */

static uint32_t round_size(uint32_t size)
{
    if (!size) size = 1;
    if (size <= SMALL_MAX) return (size + 15u) & ~15u;
    return (size + 4095u) & ~4095u;
}

uint32_t guest_alloc(uint32_t size, int zero)
{
    if (size > 0x40000000u) return 0;
    uint32_t sz = round_size(size), p = 0;
    if (sz <= SMALL_MAX) {
        uint32_t c = sz / 16u;
        if (g_small_free[c]) {
            p = g_small_free[c];
            g_small_free[c] = MEM32(p);
        }
    } else {
        uint32_t prev = 0;
        for (uint32_t q = g_large_free; q; prev = q, q = MEM32(q)) {
            if (MEM32(q - HDR) >= sz) {
                if (prev) MEM32(prev) = MEM32(q); else g_large_free = MEM32(q);
                p = q;
                break;
            }
        }
    }
    if (!p) {
        uint32_t start = (g_bump + 15u) & ~15u;
        if ((uint64_t)start + 16u + sz > GUEST_HEAP_HI) return 0;
        p = start + 16u; /* 16-aligned payload; header is the 8 bytes below it */
        g_bump = p + sz;
        MEM32(p - HDR) = sz;
    }
    MEM32(p - 4) = MAGIC_USED;
    if (zero) memset(GPTR(p), 0, MEM32(p - HDR));
    return p;
}

void guest_free(uint32_t p)
{
    if (!p) return;
    if (MEM32(p - 4) != MAGIC_USED) {
        rt_log("heap: free of non-heap or double-freed block %08X", p);
        return;
    }
    MEM32(p - 4) = MAGIC_FREE;
    uint32_t sz = MEM32(p - HDR);
    if (sz <= SMALL_MAX) {
        MEM32(p) = g_small_free[sz / 16u];
        g_small_free[sz / 16u] = p;
    } else {
        MEM32(p) = g_large_free;
        g_large_free = p;
    }
}

uint32_t guest_alloc_size(uint32_t p)
{
    return p && MEM32(p - 4) == MAGIC_USED ? MEM32(p - HDR) : 0;
}

uint32_t guest_realloc(uint32_t p, uint32_t size)
{
    if (!p) return guest_alloc(size, 0);
    uint32_t old = guest_alloc_size(p);
    if (size <= old) return p;
    uint32_t n = guest_alloc(size, 0);
    if (!n) return 0;
    memcpy(GPTR(n), GPTR(p), old);
    guest_free(p);
    return n;
}

uint32_t guest_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    uint32_t p = guest_alloc((uint32_t)n, 0);
    memcpy(GPTR(p), s, n);
    return p;
}
