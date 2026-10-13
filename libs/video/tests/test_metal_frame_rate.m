#include "../rsx_metal_backend.m"
#include <assert.h>

/* Observe Metal independently of the production handler, including dropped
 * frames whose presentedTime is zero. This is fixture-only instrumentation. */
@interface FixturePresentations : NSObject {
@public
 pthread_mutex_t lock;
 unsigned callbacks;
 double times[6];
}
@end
@implementation FixturePresentations
- (instancetype)init {
 if ((self=[super init])) pthread_mutex_init(&lock,NULL);
 return self;
}
- (void)dealloc { pthread_mutex_destroy(&lock); }
@end

static void log_display_environment(void) {
 CGDirectDisplayID displays[16];uint32_t count=0;
 CGError error=CGGetActiveDisplayList(16,displays,&count);
 fprintf(stderr,"[FPS fixture] device=%s active_displays=%u display_error=%d app_active=%d window_visible=%d occlusion_visible=%d layer_size=%.0fx%.0f\n",
     s_dev.name.UTF8String,count,error,NSApp.active,s_window.visible,
     !!(s_window.occlusionState&NSWindowOcclusionStateVisible),
     s_layer.bounds.size.width,s_layer.bounds.size.height);
 for (uint32_t i=0;i<count;i++) fprintf(stderr,
     "[FPS fixture] display=%u online=%d active=%d builtin=%d vendor=%u model=%u pixels=%zux%zu\n",
     displays[i],CGDisplayIsOnline(displays[i]),CGDisplayIsActive(displays[i]),
     CGDisplayIsBuiltin(displays[i]),CGDisplayVendorNumber(displays[i]),
     CGDisplayModelNumber(displays[i]),CGDisplayPixelsWide(displays[i]),CGDisplayPixelsHigh(displays[i]));
 NSArray *windows=CFBridgingRelease(CGWindowListCopyWindowInfo(
     kCGWindowListOptionIncludingWindow,(CGWindowID)s_window.windowNumber));
 NSDictionary *window=windows.firstObject;
 fprintf(stderr,"[FPS fixture] window_number=%ld cg_onscreen=%d cg_layer=%ld cg_bounds=%s\n",
     (long)s_window.windowNumber,[window[(id)kCGWindowIsOnscreen] boolValue],
     [window[(id)kCGWindowLayer] longValue],
     [window[(id)kCGWindowBounds] description].UTF8String);
}

/* Shutdown reaches these only for a fully initialized production engine. */
void rsx_set_backend(rsx_backend* backend) { (void)backend; }
void rsx_draw_engine_shutdown(void) {}
void rsx_draw_engine_set_backend(const rsx_draw_backend* backend) { (void)backend; }
static void* shutdown_worker(void* unused) {
 (void)unused;rsx_metal_backend_shutdown();return NULL;
}

int main(int argc, char **argv) {
 @autoreleasepool {
  assert(argc == 2);
  if (!strcmp(argv[1], "headless_shutdown")) {
   s_headless=1;pthread_t worker;
   assert(pthread_create(&worker,NULL,shutdown_worker,NULL)==0);
   assert(pthread_join(worker,NULL)==0);
  } else if (!strcmp(argv[1], "window") || !strcmp(argv[1], "drawable") || !strcmp(argv[1], "disabled")) {
   setenv("PS3RECOMP_METAL_FPS", "1", 1);
   if (!strcmp(argv[1], "disabled")) unsetenv("PS3RECOMP_METAL_FPS");
   s_dev = MTLCreateSystemDefaultDevice(); assert(s_dev);
   s_width = s_height = 64;
   assert(create_window_impl("FPS fixture") == 0);
   if (!strcmp(argv[1], "disabled")) {
    [[NSRunLoop mainRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:1.2]];
    assert([s_window.title isEqualToString:@"FPS fixture"]);
#ifdef PS3RECOMP_METAL_FRAME_RATE
    assert(!s_fps_session && !s_fps_timer);
#endif
   } else if (!strcmp(argv[1], "drawable")) {
#ifdef PS3RECOMP_METAL_FRAME_RATE
    /* Inspect the callback counters directly in this scenario. Sampling would
     * reset them on the one-second timer while a slower compositor catches up;
     * the separate window scenario verifies that timer. */
    dispatch_source_cancel(s_fps_timer);s_fps_timer=nil;
    s_queue=[s_dev newCommandQueue];assert(s_queue);
    [NSApp finishLaunching];
    [[NSRunLoop mainRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.1]];
    [s_window display];[CATransaction flush];
    log_display_environment();
    FixturePresentations *observed=[FixturePresentations new];
    id<MTLCommandBuffer> flush=[s_queue commandBuffer];[flush commit];[flush waitUntilCompleted];
    assert(s_fps_session->stats.displayed==0); /* GPU completion is not a presentation */
    for (int frame=0;frame<6;frame++) {
    id<CAMetalDrawable> drawable=[s_layer nextDrawable];assert(drawable);
    fps_track_drawable(drawable);
    [drawable addPresentedHandler:^(id<MTLDrawable> shown) {
     pthread_mutex_lock(&observed->lock);
     observed->times[frame]=shown.presentedTime;++observed->callbacks;
     fprintf(stderr,"[FPS fixture] raw_callback frame=%d presented_time=%.9f\n",frame,shown.presentedTime);
     pthread_mutex_unlock(&observed->lock);
    }];
    rsx_metal_backend_note_guest_frame();
    id<MTLCommandBuffer> cb=[s_queue commandBuffer];
    MTLRenderPassDescriptor *pass=[MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture=drawable.texture;
    pass.colorAttachments[0].loadAction=MTLLoadActionClear;
    pass.colorAttachments[0].storeAction=MTLStoreActionStore;
    pass.colorAttachments[0].clearColor=MTLClearColorMake(0.1,0.3,0.5,1);
    id<MTLRenderCommandEncoder> encoder=[cb renderCommandEncoderWithDescriptor:pass];
    assert(encoder);[encoder endEncoding];
    [cb presentDrawable:drawable];[cb commit];[cb waitUntilCompleted];
    [[NSRunLoop mainRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
    }
    /* Completion of command buffers precedes compositor presentation. Wait
     * for its condition with a deadline rather than assuming 50ms is enough. */
    const double deadline=CACurrentMediaTime()+5.0;
    for (;;) {
     pthread_mutex_lock(&s_fps_session->lock);
     const unsigned long long shown=s_fps_session->stats.displayed;
     pthread_mutex_unlock(&s_fps_session->lock);
     if (shown || CACurrentMediaTime()>=deadline) break;
     [[NSRunLoop mainRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
    }
    pthread_mutex_lock(&s_fps_session->lock);
    pthread_mutex_lock(&observed->lock);
    fprintf(stderr,"[FPS fixture] raw_callbacks=%u presented_times=%.9f,%.9f,%.9f,%.9f,%.9f,%.9f\n",
        observed->callbacks,observed->times[0],observed->times[1],observed->times[2],
        observed->times[3],observed->times[4],observed->times[5]);
    pthread_mutex_unlock(&observed->lock);
    fprintf(stderr,"[FPS fixture] drawable displayed=%llu guest=%llu\n",
        s_fps_session->stats.displayed,s_fps_session->stats.guest);
    assert(s_fps_session->stats.displayed>0 && s_fps_session->stats.displayed<=6);
    assert(s_fps_session->stats.guest==6);
    pthread_mutex_unlock(&s_fps_session->lock);
    MetalFrameRateSession *old=s_fps_session;
    fps_stop();fps_start();
    assert(s_fps_session!=old && s_fps_session->stats.displayed==0);
#else
    assert(!"Metal drawable tracking is missing");
#endif
   } else {
   /* No rendering thread is running: the title must still update on a stall. */
   [[NSRunLoop mainRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:1.2]];
   assert([s_window.title containsString:@"display 0.0 FPS"]);
   assert([s_window.title containsString:@"guest 0.0 FPS"]);
   }
#ifdef PS3RECOMP_METAL_FRAME_RATE
   fps_stop();
#endif
   [s_window close];
  } else {
#ifdef PS3RECOMP_METAL_FRAME_RATE
   MetalFrameRate stats = {0};
   metal_frame_rate_reset(&stats, 10.0);
   for (int i=1;i<=60;i++) metal_frame_rate_guest(&stats);
   for (int i=1;i<=10;i++) metal_frame_rate_presented(&stats, 10.0+i*0.1);
   MetalFrameRateSample sample = metal_frame_rate_sample(&stats, 11.0);
   assert(fabs(sample.display_fps-10.0)<0.001);
   assert(fabs(sample.guest_fps-60.0)<0.001);
   assert(fabs(sample.average_ms-100.0)<0.001);
   assert(fabs(sample.p95_ms-100.0)<0.001);
   sample = metal_frame_rate_sample(&stats, 13.0);
   assert(sample.display_fps==0 && sample.guest_fps==0);
   assert(sample.average_ms==0 && sample.p95_ms==0);
   assert(fabs(sample.age_seconds-2.0)<0.001);
   metal_frame_rate_presented(&stats, 13.1);
   metal_frame_rate_presented(&stats, 13.1); /* duplicate time cannot add a frame */
   metal_frame_rate_presented(&stats, 12.9); /* out-of-order callback */
   sample = metal_frame_rate_sample(&stats, 14.0);
   assert(sample.display_fps==1 && sample.guest_fps==0);
   assert(fabs(sample.worst_ms-2100.0)<0.001);
   metal_frame_rate_reset(&stats, 20.0);
   metal_frame_rate_presented(&stats, 20.01);
   for (int i=1;i<=300;i++) metal_frame_rate_presented(&stats, 20.01+i*0.01);
   sample=metal_frame_rate_sample(&stats, 24.0);
   assert(sample.display_fps==75.25);
   assert(fabs(sample.p95_ms-10.0)<0.001);
   assert(fabs(sample.worst_ms-10.0)<0.001);
#else
   assert(!"Metal frame-rate measurement is missing");
#endif
  }
  puts("Metal frame-rate scenario passed");
 }
}
