/* The draw engine's staging arena: recorded into a pooled shared MTLBuffer,
 * kept across growth, handed to the command buffer at commit and returned to
 * the pool when the GPU completes it. Needs a Metal device; no window.
 *
 *   clang -fobjc-arc -I include -I libs/video libs/video/tests/test_metal_stage_pool.m \
 *     -Wl,-dead_strip -framework Metal -framework Cocoa -framework QuartzCore \
 *     -o /tmp/test_metal_stage_pool && /tmp/test_metal_stage_pool
 */
#include "../rsx_metal_backend.m"
#include <assert.h>
#include <unistd.h>

float cellAvconfExt_display_gamma(void) { return 1.0f; }

static NSUInteger pool_count(void)
{
    pthread_mutex_lock(&s_eng_stage_pool_lock);
    const NSUInteger n = [s_eng_stage_pool count];
    pthread_mutex_unlock(&s_eng_stage_pool_lock);
    return n;
}

int main(void)
{
    @autoreleasepool {
        s_dev = MTLCreateSystemDefaultDevice();
        assert(s_dev);
        s_queue = [s_dev newCommandQueue];
        assert(s_queue);

        /* Staged bytes live in a shared MTLBuffer's contents. */
        u8 bytes[256];
        for (u32 i = 0; i < sizeof bytes; i++) bytes[i] = (u8)(i * 7u + 3u);
        u32 off = ~0u;
        assert(eng_stage_copy(bytes, sizeof bytes, &off));
        id<MTLBuffer> first = s_eng_stage_buf;
        assert(first && s_eng_stage == (u8*)[first contents]);
        assert(memcmp((u8*)[first contents] + off, bytes, sizeof bytes) == 0);

        /* Growing the arena before a commit keeps what was recorded. */
        const u32 grow = s_eng_stage_cap + 1u;
        u32 off2 = 0;
        assert(eng_stage_reserve(grow, &off2));
        assert(s_eng_stage_buf != first && s_eng_stage_cap >= off2 + grow);
        assert(memcmp(s_eng_stage + off, bytes, sizeof bytes) == 0);
        assert(pool_count() == 1);        /* the outgrown arena, never committed */
        id<MTLBuffer> grown = s_eng_stage_buf;

        /* A commit hands the arena to the command buffer and starts afresh;
         * the GPU's completion returns it to the pool. */
        eng_encode_and_commit(nil);
        assert(!s_eng_stage_buf && !s_eng_stage && !s_eng_stage_used && !s_eng_stage_cap);
        for (int i = 0; i < 1000 && pool_count() < 2; i++) usleep(1000);
        assert(pool_count() == 2);

        /* The next recording takes a pooled buffer instead of a new one. */
        assert(eng_stage_copy(bytes, sizeof bytes, &off));
        assert(s_eng_stage_buf == first || s_eng_stage_buf == grown);
        assert(pool_count() == 1);

        /* The pool keeps a bounded number of buffers. */
        for (int i = 0; i < ENG_STAGE_POOL_KEEP + 4; i++)
            eng_stage_buffer_release([s_dev newBufferWithLength:4096
                                                        options:MTLResourceStorageModeShared]);
        assert(pool_count() == ENG_STAGE_POOL_KEEP);

        eng_shutdown(NULL);
        assert(!s_eng_stage_buf && !s_eng_stage && pool_count() == 0);
        puts("The Metal staging arena records into pooled buffers");
    }
}
