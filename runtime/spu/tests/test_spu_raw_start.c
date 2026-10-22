/* Raw SPU start through the real spu_raw.c: writing 1 to SPU_RunCntl runs the
 * lifted program on a host thread of its own, and its stop is published in the
 * window the PPU polls. The host thread used to exist under _WIN32 only, so on
 * macOS and Linux a raw SPU was told to run and never did.
 *
 * The program is streamed into local store by proxy GETs (the path Tornado
 * Outbreak and Resistance use) and found by its fingerprint. The registry, the
 * channel wake-up and the lifted-code runner are stand-ins; the window, the
 * MMIO side effects and the thread are the code under test. */
#include "../spu_raw.c"

#include <pthread.h>
#include <sys/mman.h>
#include <time.h>

/* What the runtime would provide. */
uint8_t* vm_base;
uint32_t ppu_vm_size;
int g_spu_force_ch_block;
void (*g_spu_out_mbox_hook)(uint32_t, uint32_t, int, uint32_t);
PPU_THREAD_LOCAL ppu_context* g_active_ctx;
void spu_ch_wake(spu_context* c) { (void)c; }
void ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*))
{ (void)nid; (void)name; (void)fn; }

/* The registry: FNV-1a-64 of the exact bytes, two programs. */
uint64_t spu_workload_fingerprint(const void* data, size_t n)
{
    const uint8_t* p = (const uint8_t*)data;
    uint64_t h = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001B3ull; }
    return h;
}
static uint8_t s_prog[2][0x400];
static volatile int s_ran[2];
static pthread_t s_ran_on[2];
static void prog_a(spu_context* c) { s_ran_on[0] = pthread_self(); s_ran[0]++; c->stop_code = 0x2000; }
static void prog_b(spu_context* c) { s_ran_on[1] = pthread_self(); s_ran[1]++; c->stop_code = 0x3000; }
spu_lifted_entry_fn spu_workload_find_img(uint64_t fp, int* image_id)
{
    for (int i = 0; i < 2; i++)
        if (fp == spu_workload_fingerprint(s_prog[i], sizeof s_prog[i])) {
            if (image_id) *image_id = i + 1;
            return i ? prog_b : prog_a;
        }
    return NULL;
}
int spu_run_with_halt(void (*fn)(spu_context*), spu_context* c) { fn(c); return 0; }

static int g_fails;
#define CHECK(c, ...) do { if (!(c)) { g_fails++; fprintf(stderr, "FAIL line %d: %s: ", __LINE__, #c); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

#define G_PROG 0x00100000u     /* where the PPU keeps the programs it streams in */

static void w32(uint32_t ea, uint32_t v) { be32_store(ea, v); spu_raw_reg_store(ea, v, 4); }

static uint32_t create(void)
{
    ppu_context ctx; memset(&ctx, 0, sizeof ctx);
    ctx.gpr[3] = 0x1000;
    CHECK(sc_raw_spu_create(&ctx) == CELL_OK, "create");
    return be32_load(0x1000);
}

/* Stream program `p` into raw SPU n's local store at 0, in 0x100-byte GETs. */
static void proxy_load(uint32_t n, int p)
{
    const uint32_t base = SPU_RAW_BASE + n * SPU_RAW_STRIDE;
    for (uint32_t o = 0; o < sizeof s_prog[p]; o += 0x100) {
        w32(base + SPU_RAW_MFC_LSA, o);
        w32(base + SPU_RAW_MFC_EAH, 0);
        w32(base + SPU_RAW_MFC_EAL, G_PROG + (uint32_t)p * 0x1000u + o);
        w32(base + SPU_RAW_MFC_SIZE_TAG, 0x100u << 16);
        w32(base + SPU_RAW_MFC_CLASS_CMD, 0x40u);      /* GET */
    }
}

/* Start raw SPU n and wait for the stop it publishes. */
static uint32_t run(uint32_t n)
{
    const uint32_t base = SPU_RAW_BASE + n * SPU_RAW_STRIDE;
    w32(base + SPU_RAW_NPC, 0);
    w32(base + SPU_RAW_RUNCNTL, RUNCNTL_RUN);
    uint32_t st = 0;
    for (int i = 0; i < 2000; i++) {
        if (s_spu[n].ctx && !__atomic_load_n(&s_spu[n].started, __ATOMIC_ACQUIRE)) {
            st = be32_load(base + SPU_RAW_STATUS);
            if (st & SPU_STATUS_STOPPED_BY_STOP) break;
        }
        struct timespec ts = { 0, 1000000 }; nanosleep(&ts, NULL);
    }
    return st;
}

int main(void)
{
    void* p = mmap(NULL, 0x100010000ull, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) { perror("mmap"); return 1; }
    vm_base = (uint8_t*)p;
    for (int i = 0; i < 2; i++) {
        for (size_t o = 0; o < sizeof s_prog[i]; o++) s_prog[i][o] = (uint8_t)(o * (i + 3) + 1);
        memcpy(vm_base + G_PROG + (uint32_t)i * 0x1000u, s_prog[i], sizeof s_prog[i]);
    }

    /* 1. A proxy-loaded raw SPU runs, off the PPU's thread, and stops with its code. */
    uint32_t a = create();
    proxy_load(a, 0);
    uint32_t st = run(a);
    CHECK(s_ran[0] == 1, "program A ran %d times", s_ran[0]);
    CHECK(s_ran[0] == 0 || !pthread_equal(s_ran_on[0], pthread_self()), "ran on the caller's thread");
    CHECK(st == (SPU_STATUS_STOPPED_BY_STOP | (0x2000u << 16)), "status 0x%08X", st);
    CHECK(s_spu[a].int_stat & 0x2u, "stop-and-signal interrupt bit not set");

    free(s_spu[a].ctx);
    if (g_fails) { fprintf(stderr, "%d check(s) failed\n", g_fails); return 1; }
    puts("PASS: a raw SPU started by SPU_RunCntl runs on its own host thread and publishes its stop");
    return 0;
}
