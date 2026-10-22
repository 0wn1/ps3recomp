/* depth_as_argb against the store through guest memory: a depth target full of
 * awkward depths (0, 1, the smallest normal and subnormal floats, values one
 * ulp off a rounding tie, a spread of ordinary ones) converted on the GPU must
 * hold, byte for byte, what depth_readback stored as Z24S8 and decoded gives,
 * and its crossbar view must sample as the uploaded texture does. No window or
 * original game data.
 * clang -fobjc-arc -I include -I libs/video
 * libs/video/tests/test_metal_depth_as_argb.m libs/video/rsx_texture_layout.c libs/video/rsx_shader_msl.cpp -lc++
 * -Wl,-dead_strip -framework Metal -framework Cocoa -framework QuartzCore
 * -o /tmp/test_metal_depth_as_argb */
#include "../rsx_metal_backend.m"
#include <math.h>
#include <stdio.h>

float cellAvconfExt_display_gamma(void) { return 1.0f; }

static int failures, checks;
#define CHECK(v, name) do { checks++; if (!(v)) { failures++; fprintf(stderr, "FAIL %s\n", name); } } while (0)
#define W 64u
#define H 32u
static const char* fixture =
"#include <metal_stdlib>\nusing namespace metal;\n"
"struct V { float4 p [[position]]; };\n"
"vertex V vs(uint i [[vertex_id]]) { float2 p[3] = {float2(-1,-1), float2(3,-1), float2(-1,3)}; return {float4(p[i], 0, 1)}; }\n"
"struct D { float d [[depth(any)]]; };\n"
"fragment D depth_fs(V v [[stage_in]], texture2d<float> t [[texture(0)]]) { return {t.read(uint2(v.p.xy)).r}; }\n"
"fragment float4 copy_fs(V v [[stage_in]], texture2d<float> t [[texture(0)]]) { return t.read(uint2(v.p.xy)); }\n";

static int complete(id<MTLCommandBuffer> cb) { [cb commit]; [cb waitUntilCompleted]; return cb.status == MTLCommandBufferStatusCompleted && !cb.error; }

static id<MTLRenderPipelineState> pipeline(id<MTLLibrary> lib, const char* fn, MTLPixelFormat color)
{
    MTLRenderPipelineDescriptor* p = [MTLRenderPipelineDescriptor new];
    p.vertexFunction = [lib newFunctionWithName:@"vs"];
    p.fragmentFunction = [lib newFunctionWithName:[NSString stringWithUTF8String:fn]];
    if (color == MTLPixelFormatInvalid) {
        p.depthAttachmentPixelFormat = MTL_DEPTH_FORMAT;
        p.stencilAttachmentPixelFormat = MTL_DEPTH_FORMAT;
    } else {
        p.colorAttachments[0].pixelFormat = color;
    }
    NSError* error = nil;
    id<MTLRenderPipelineState> r = [s_dev newRenderPipelineStateWithDescriptor:p error:&error];
    CHECK(r != nil, "fixture pipeline");
    return r;
}

/* One full-screen pass of `pso` reading `src` into `dst` (colour) or `depth`. */
static int pass(id<MTLRenderPipelineState> pso, id<MTLTexture> src, id<MTLTexture> dst, id<MTLTexture> depth)
{
    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
    if (depth) {
        rp.depthAttachment.texture = depth; rp.depthAttachment.loadAction = MTLLoadActionClear;
        rp.depthAttachment.storeAction = MTLStoreActionStore;
        rp.stencilAttachment.texture = depth; rp.stencilAttachment.loadAction = MTLLoadActionClear;
        rp.stencilAttachment.storeAction = MTLStoreActionStore;
    } else {
        rp.colorAttachments[0].texture = dst; rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    }
    id<MTLCommandBuffer> cb = [s_queue commandBuffer];
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
    if (depth) {
        MTLDepthStencilDescriptor* ds = [MTLDepthStencilDescriptor new];
        ds.depthCompareFunction = MTLCompareFunctionAlways; ds.depthWriteEnabled = YES;
        [e setDepthStencilState:[s_dev newDepthStencilStateWithDescriptor:ds]];
    }
    [e setRenderPipelineState:pso];
    [e setFragmentTexture:src atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [e endEncoding];
    return complete(cb);
}

static id<MTLTexture> shared_texture(MTLPixelFormat f, MTLTextureUsage usage)
{
    MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:f width:W height:H mipmapped:NO];
    td.usage = usage; td.storageMode = MTLStorageModeShared;
    return [s_dev newTextureWithDescriptor:td];
}

static float bits(u32 b) { float f; memcpy(&f, &b, sizeof f); return f; }

int main(void)
{
    @autoreleasepool {
        s_dev = MTLCreateSystemDefaultDevice(); CHECK(s_dev != nil, "Metal device"); if (!s_dev) return 1;
        s_queue = [s_dev newCommandQueue]; CHECK(eng_init(NULL, W, H) == 0, "backend helpers");
        NSError* error = nil;
        id<MTLLibrary> lib = [s_dev newLibraryWithSource:[NSString stringWithUTF8String:fixture] options:nil error:&error];
        CHECK(lib != nil, "fixture shaders"); if (!lib) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); return 1; }
        CHECK(s_engine_backend.depth_as_argb == eng_depth_as_argb, "backend advertises depth_as_argb");

        /* The depths: edge cases first, then a deterministic spread. */
        float src[W * H];
        const float edges[] = { 0.0f, 1.0f, bits(0x00800000u), bits(0x00000001u), bits(0x3F7FFFFFu),
                                0.5f, 1.0f / 16777215.0f, 0.5f / 16777215.0f, bits(0x33000000u),
                                bits(0x33000001u), 0.25f, 0.75f, 0.999999f, 1e-7f, 3e-8f };
        u32 seed = 12345u;
        for (u32 i = 0; i < W * H; i++) {
            seed = seed * 1664525u + 1013904223u;
            src[i] = i < sizeof edges / sizeof edges[0] ? edges[i]
                   : (i & 1u) ? (float)(seed >> 8) / 16777216.0f
                              : bits(0x30000000u + (seed >> 7) % 0x0F000000u);   /* down to 2^-31 */
        }
        /* Ties of the rounding: k + 0.5 over 0xFFFFFF, as near as a float gets. */
        for (u32 i = 0; i < 64; i++) src[W * (H - 1) + i] = (float)((double)(i * 262139u + 7u) + 0.5) / 16777215.0f;
        id<MTLTexture> values = shared_texture(MTLPixelFormatR32Float, MTLTextureUsageShaderRead);
        [values replaceRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0 withBytes:src bytesPerRow:W * 4];

        const u32 depth = eng_depth_target_create(NULL, W, H);
        CHECK(depth && pass(pipeline(lib, "depth_fs", MTLPixelFormatInvalid), values, nil, eng_obj(depth)),
              "depths written");

        /* The store through guest memory: readback, Z24S8 as eng_zdepth_to_guest lays it out, decode. */
        float back[W * H];
        CHECK(s_engine_backend.depth_readback(NULL, depth, 0, 0, W, H, back, W * 4) == 0, "depth readback");
        u8 packed[W * H * 4], want[W * H * 4];
        for (u32 i = 0; i < W * H; i++) {
            float d = back[i];
            if (!(d > 0.0f)) d = 0.0f;
            if (d > 1.0f) d = 1.0f;
            const u32 v = (u32)((double)d * 16777215.0 + 0.5);
            packed[4 * i] = (u8)(v >> 16); packed[4 * i + 1] = (u8)(v >> 8); packed[4 * i + 2] = (u8)v; packed[4 * i + 3] = 0;
        }
        rsx_tex_layout tl; rsx_texture_layout(0xA5, W, H, &tl);
        rsx_texture_decode(want, W * 4, packed, W, H, &tl, 0);

        /* The GPU conversion, read back through a copy. */
        const u32 argb = eng_depth_as_argb(NULL, depth, W, H);
        CHECK(argb != 0, "depth_as_argb made a texture");
        CHECK(eng_depth_as_argb(NULL, depth, W + 1, H) == 0, "a texture wider than the depth target is refused");
        eng_submit_and_wait(NULL, 0);
        id<MTLTexture> got_tex = shared_texture(MTLPixelFormatRGBA8Unorm, MTLTextureUsageShaderRead);
        id<MTLCommandBuffer> cb = [s_queue commandBuffer];
        id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
        [b copyFromTexture:eng_obj(argb) sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                sourceSize:MTLSizeMake(W, H, 1) toTexture:got_tex destinationSlice:0 destinationLevel:0
         destinationOrigin:MTLOriginMake(0, 0, 0)];
        [b endEncoding];
        CHECK(complete(cb), "copy of the converted texture");
        u8 got[W * H * 4];
        [got_tex getBytes:got bytesPerRow:W * 4 fromRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0];
        u32 bad = 0;
        for (u32 i = 0; i < W * H; i++)
            if (memcmp(got + 4 * i, want + 4 * i, 4)) {
                if (bad++ < 4)
                    fprintf(stderr, "  texel %u depth %a: got %02X %02X %02X %02X want %02X %02X %02X %02X\n", i,
                            back[i], got[4 * i], got[4 * i + 1], got[4 * i + 2], got[4 * i + 3],
                            want[4 * i], want[4 * i + 1], want[4 * i + 2], want[4 * i + 3]);
            }
        CHECK(bad == 0, "every texel is the decode of the Z24S8 bytes");

        /* Through the crossbar a title reads depth bytes with: the view samples as the upload does. */
        const u32 view = eng_surface_view(NULL, argb, 0xAA93, 0xE5);
        const u32 upload = eng_texture_create(NULL, RSX_BE_FMT_R8G8B8A8, W, H, 1, 1, 0xAA93, 0xE5);
        eng_texture_upload(NULL, upload, 0, 0, W, H, want, W * 4, H);
        id<MTLRenderPipelineState> copy = pipeline(lib, "copy_fs", MTLPixelFormatRGBA32Float);
        id<MTLTexture> a = shared_texture(MTLPixelFormatRGBA32Float, MTLTextureUsageRenderTarget);
        id<MTLTexture> c = shared_texture(MTLPixelFormatRGBA32Float, MTLTextureUsageRenderTarget);
        CHECK(view && upload && pass(copy, eng_obj(view), a, nil) && pass(copy, eng_obj(upload), c, nil),
              "both sampled through 0xAA93");
        static float fa[W * H * 4], fc[W * H * 4];
        [a getBytes:fa bytesPerRow:W * 16 fromRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0];
        [c getBytes:fc bytesPerRow:W * 16 fromRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0];
        CHECK(!memcmp(fa, fc, sizeof fa), "the view samples as the uploaded texture");

        eng_obj_release(NULL, upload); eng_obj_release(NULL, argb); eng_obj_release(NULL, depth);
        eng_collect_retired_objects();
        int ok = 1; for (u32 i = 0; i < s_eng_obj_count; i++) ok &= s_eng_obj[i] == nil;
        CHECK(ok, "releasing the texture releases its view");
        printf("%d checks, %d failures\n", checks, failures);
        eng_shutdown(NULL);
        return failures ? 1 : 0;
    }
}
