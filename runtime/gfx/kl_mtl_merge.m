// One Metal render encoder across Vulkan dynamic-rendering suspend/resume
// segments, above stock MoltenVK, plus the guest's GPU time per frame.
//
// Adapted from Glovebox (MIT, github.com/glovebox-vr/glovebox)
// klepton/runtime/gfx/kl_gpu_time.m (KL_VK_MERGE, KL_VK_MERGE_VIS, the [ml]
// memoryless guard and the per-frame GPU time).
//
// Alyx records its stereo scene pass as ONE dynamic-rendering instance in four
// segments (SUSPENDING / RESUMING, in different VkCommandBuffers). MoltenVK
// ends the Metal render encoder at every segment end, storing the whole 4x MSAA
// colour + D32S8, and loads it all back at the next resume: three store+load
// round trips a frame that a tiler never needed (perf audit 2026-10-07, #1).
// Per Vulkan the segments are one render pass instance, so here the Metal
// encoder is kept open across them: MoltenVK's endEncoding on a guest MSAA
// encoder is deferred ("held"), its store actions are cached, and when the next
// render pass on the SAME command buffer is a resume of exactly the same
// attachments (every one loaded) the held encoder is handed back instead of a
// new one. Anything else that touches the command buffer (another encoder, an
// event, commit) ends the held encoder first with its latest store actions,
// which is exactly what MoltenVK would have encoded: the fallback is stock.
//
// What this adds over Glovebox's version (all for stock MoltenVK 1.4.2):
//  - A resume is CONFIRMED, not inferred. MoltenVK also ends and restarts a
//    Metal pass mid-segment (kMVKCommandUseRestartSubpass: Alyx reads its MSAA
//    colour as an input attachment ~1.5 times a frame, which MoltenVK samples
//    as a texture, so the pass MUST be stored there). Such a restart looks
//    exactly like a resume at the Metal level (same attachments, all Load).
//    kl_vulkan.c therefore records an empty debug group named
//    KL_MTL_MERGE_RESUME_MARK right before every RESUMING vkCmdBeginRendering;
//    MoltenVK encodes it on the command buffer between the segments, the hook
//    here swallows it (Metal never sees it) and only then may the next pass
//    continue the held encoder. A restart has no marker and stays stock.
//    KL_VK_MERGE_ANY=1 drops that requirement (Glovebox's behaviour; diagnostic).
//  - No pipeline barrier between the segments (a second marker, KL_MTL_MERGE_BARRIER_MARK, before
//    every vkCmdPipelineBarrier/2). Alyx records a GENERAL->GENERAL barrier on its MSAA colour
//    (dst access incl. SHADER_READ) before EVERY resume and its next segment reads that colour as
//    a texture: merged across it, nothing was stored and the picture shows ring banding (Mac,
//    2026-10-11). So for Alyx this rule leaves every boundary stock. KL_VK_MERGE_BARRIERS=1 merges
//    across barriers anyway (Glovebox's behaviour) — a bandwidth MEASUREMENT with a wrong picture.
//  - The held encoder must have ended with plain Store on every attachment
//    (a SUSPENDING end); a resolve means the pass was over.
//  - Fence waits MoltenVK encodes at the start of a continued encoder are
//    dropped (with argument buffers + residency sets MoltenVK fences encoders;
//    the continued encoder would wait on a fence it updates itself). Nothing
//    else was encoded on the command buffer since the held encoder began, so
//    every wait it could need was already taken at its real start.
//  - A resume that brings no visibility buffer may continue one that has it.
//  - KL_VK_MERGE_AB=<s>: in-process A/B (merge ON / stock every s seconds,
//    per guest frame), with GPU ms per frame per phase.
//
// Nothing here is installed unless a knob asks for it (kl_mtl_merge_wanted).
#pragma clang diagnostic ignored "-Wdeprecated-declarations"   // MTLStoreActionOptions (no-op on Apple GPUs, still forwarded)
#include <os/lock.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <mach/mach_time.h>

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>

#include "kl_mtl_merge.h"
#include "kl_env.h"

// ---- knobs -----------------------------------------------------------------------
static pthread_once_t g_knobs_once = PTHREAD_ONCE_INIT;
static int g_merge, g_merge_vis, g_merge_any, g_merge_bar, g_gpu_time, g_ml, g_dbg;
static double g_ab;                      // KL_VK_MERGE_AB period in seconds (0 = off)
static uint32_t g_tr_at;                 // KL_VK_MERGE_DEBUG=2: trace every merge event of 2 guest frames from this one
static double g_log_every = 5.0;

static void knobs(void) {
    g_merge = kl_env_on("KL_VK_MERGE", 0);
    g_merge_vis = g_merge && kl_env_on("KL_VK_MERGE_VIS", 0);
    g_merge_any = g_merge && kl_env_on("KL_VK_MERGE_ANY", 0);
    g_merge_bar = g_merge && kl_env_on("KL_VK_MERGE_BARRIERS", 0);
    g_ab = g_merge ? kl_env_float("KL_VK_MERGE_AB", 0) : 0;
    if (g_ab < 0) g_ab = 0;
    g_gpu_time = kl_env_on("KL_VK_GPU_TIME", 0) || kl_env_on("KL_MVK_GPU_TIME", 0) || g_ab > 0;
    g_ml = kl_env_on("KL_VK_MSAA_MEMORYLESS", 0);
    g_dbg = kl_env_int("KL_VK_MERGE_DEBUG", 0);
    g_tr_at = (uint32_t)kl_env_int("KL_VK_MERGE_TRACE_AT", 3000);
    float e = kl_env_float("KL_VK_GPU_TIME_LOG", 5.0f);
    if (e > 0) g_log_every = e;
}
int kl_mtl_merge_wanted(void) { pthread_once(&g_knobs_once, knobs); return g_merge || g_gpu_time; }
int kl_mtl_merge_on(void) { pthread_once(&g_knobs_once, knobs); return g_merge; }

static double now_s(void) {   // the clock MTLCommandBuffer GPUStartTime uses (mach absolute time)
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return (double)mach_absolute_time() * tb.numer / tb.denom * 1e-9;
}

// ---- watched queues, frames, A/B phase ---------------------------------------------
enum { kRing = 32, kMaxQueues = 8 };
typedef struct {
    uint32_t frame, committed, completed;
    double busy, last_end;               // union of the frame's command buffers' GPU intervals
    int phase, settled, done;            // A/B phase of the frame (0 merged, 1 stock); not just after a flip
} slot;
static os_unfair_lock g_lk = OS_UNFAIR_LOCK_INIT;
static void *g_queues[kMaxQueues];
static int g_nqueues;
static uint32_t g_frame = 1;
static slot g_slots[kRing];
#define TR(...) do { if (g_dbg >= 2 && g_frame >= g_tr_at && g_frame < g_tr_at + 2) { \
    fprintf(stderr, "[merge-tr] f%u ", g_frame); fprintf(stderr, __VA_ARGS__); } } while (0)
static const char *label_of(void *o) { NSString *l = o ? [(__bridge id)o label] : nil; return l ? l.UTF8String : "-"; }
static _Atomic int g_phase;              // 0 = merge on this frame, 1 = stock (A/B off phase)
static double g_t0;

static slot *slot_of(uint32_t f) {       // g_lk held
    slot *s = &g_slots[f % kRing];
    if (s->frame != f) { memset(s, 0, sizeof *s); s->frame = f; }
    return s;
}
static int watched(void *q) {            // g_lk held
    for (int i = 0; i < g_nqueues; i++) if (g_queues[i] == q) return 1;
    return 0;
}
static int cb_watched(id<MTLCommandBuffer> cb) {
    void *q = (__bridge void *)[cb commandQueue];
    os_unfair_lock_lock(&g_lk);
    int w = watched(q);
    os_unfair_lock_unlock(&g_lk);
    return w;
}
static void on_complete(uint32_t f, double start, double end) {
    os_unfair_lock_lock(&g_lk);
    slot *s = &g_slots[f % kRing];
    if (s->frame == f) {
        if (end > start && start > 0) {
            double from = start > s->last_end ? start : s->last_end;
            if (end > from) s->busy += end - from;
            if (end > s->last_end) s->last_end = end;
        }
        s->completed++;
    }
    os_unfair_lock_unlock(&g_lk);
}

// ---- the merge ---------------------------------------------------------------------
typedef struct {
    void *enc, *cb;                      // retained encoder; its command buffer (not retained)
    void *tex[10], *res[10];             // colour 0..7, depth, stencil and their resolve targets
    NSUInteger slice[10], level[10], w, h, layers;
    void *vis;                           // visibility result buffer (occlusion queries)
    int held;                            // MoltenVK has ended it; the real end is pending
    int resumed;                         // the resume marker came while it was held
    int barrier;                         // a pipeline barrier was encoded while it was held
    int continued;                       // handed back at least once (its later fence waits are dropped)
    int st_set[10]; MTLStoreAction st[10];
    int opt_set[10]; MTLStoreActionOptions opt[10];
} kl_merge_t;
enum { kMergeMax = 16 };
static kl_merge_t g_mg[kMergeMax];
static os_unfair_lock g_mg_lk = OS_UNFAIR_LOCK_INIT;
static IMP g_commit, g_rce, g_push, g_pop;
static IMP g_end, g_scsa, g_sdsa, g_sssa, g_scso, g_sdso, g_ssso, g_wff;
static IMP g_m_cce0, g_m_cce, g_m_cced, g_m_bce, g_m_bced, g_m_prce, g_m_sig, g_m_wait, g_m_pd, g_m_pdt, g_m_pdd;
// Counters since the last report.
static _Atomic unsigned c_held, c_cont, c_restart, c_barrier, c_mismatch, c_flush_other, c_fence, c_vis, c_untracked;
static __thread void *t_swallow_pop;     // the command buffer whose next popDebugGroup is our marker's

static void mg_fill(kl_merge_t *m, MTLRenderPassDescriptor *d) {
    for (int i = 0; i < 8; i++) {
        MTLRenderPassColorAttachmentDescriptor *a = d.colorAttachments[i];
        m->tex[i] = (__bridge void *)a.texture; m->res[i] = (__bridge void *)a.resolveTexture;
        m->slice[i] = a.slice; m->level[i] = a.level;
    }
    m->tex[8] = (__bridge void *)d.depthAttachment.texture; m->res[8] = (__bridge void *)d.depthAttachment.resolveTexture;
    m->slice[8] = d.depthAttachment.slice; m->level[8] = d.depthAttachment.level;
    m->tex[9] = (__bridge void *)d.stencilAttachment.texture; m->res[9] = (__bridge void *)d.stencilAttachment.resolveTexture;
    m->slice[9] = d.stencilAttachment.slice; m->level[9] = d.stencilAttachment.level;
    m->w = d.renderTargetWidth; m->h = d.renderTargetHeight; m->layers = d.renderTargetArrayLength;
    m->vis = (__bridge void *)d.visibilityResultBuffer;
}

// KL_VK_MSAA_MEMORYLESS (kl_vulkan.c): a memoryless attachment lives only inside one encoder, so no encoder may load
// or store one. A resume that did not merge into the held encoder would load it: that is counted ("unmerged load",
// the number that must stay zero for a correct picture) and made legal (DontCare), and a store becomes DontCare or a
// plain resolve, so Metal never sees an illegal action.
static _Atomic unsigned g_ml_enc, g_ml_loads, g_ml_stores;
static int ml_is(void *tex) { return g_ml && tex && ((__bridge id<MTLTexture>)tex).storageMode == MTLStorageModeMemoryless; }
static MTLStoreAction ml_store(MTLStoreAction a) {
    if (a == MTLStoreActionStore) { atomic_fetch_add(&g_ml_stores, 1); return MTLStoreActionDontCare; }
    if (a == MTLStoreActionStoreAndMultisampleResolve) { atomic_fetch_add(&g_ml_stores, 1); return MTLStoreActionMultisampleResolve; }
    return a;
}
static int ml_resume(MTLRenderPassDescriptor *d) {   // MoltenVK turned the memoryless attachments' Load into DontCare
    for (int i = 0; i < 8; i++) if (d.colorAttachments[i].texture && d.colorAttachments[i].loadAction == MTLLoadActionLoad) return 1;
    if (d.depthAttachment.texture && d.depthAttachment.loadAction == MTLLoadActionLoad) return 1;
    return 0;
}
static void ml_guard(MTLRenderPassDescriptor *d) {
    if (!g_ml || !d) return;
    int any = 0;
    for (int i = 0; i < 10; i++) {
        MTLRenderPassAttachmentDescriptor *a = i < 8 ? (MTLRenderPassAttachmentDescriptor *)d.colorAttachments[i]
                                             : i == 8 ? (MTLRenderPassAttachmentDescriptor *)d.depthAttachment
                                                      : (MTLRenderPassAttachmentDescriptor *)d.stencilAttachment;
        if (!ml_is((__bridge void *)a.texture)) continue;
        any = 1;
        if (a.loadAction == MTLLoadActionLoad || (a.loadAction == MTLLoadActionDontCare && ml_resume(d))) {
            unsigned n = atomic_fetch_add(&g_ml_loads, 1) + 1;
            if (n <= 8) fprintf(stderr, "[ml] UNMERGED load of memoryless attachment %d (%lux%lu) -> DontCare (#%u)\n", i,
                                (unsigned long)a.texture.width, (unsigned long)a.texture.height, n);
            a.loadAction = MTLLoadActionDontCare;
        }
        a.storeAction = ml_store(a.storeAction);
    }
    if (any) {
        unsigned n = atomic_fetch_add(&g_ml_enc, 1) + 1;
        if (n == 1 || n % 2000 == 0)
            fprintf(stderr, "[ml] %u memoryless MSAA encoders, %u unmerged loads, %u stores dropped\n", n,
                    atomic_load(&g_ml_loads), atomic_load(&g_ml_stores));
    }
}

// 0 = continues the held pass; otherwise why not (1 attachments/size, 2 not all loaded, 3 visibility, 4 held end was not a plain store).
static int mg_continues(const kl_merge_t *m, MTLRenderPassDescriptor *d) {
    kl_merge_t n; memset(&n, 0, sizeof n); mg_fill(&n, d);
    for (int i = 0; i < 10; i++)
        if (n.tex[i] != m->tex[i] || n.res[i] != m->res[i] || n.slice[i] != m->slice[i] || n.level[i] != m->level[i]) return 1;
    if (n.w != m->w || n.h != m->h || n.layers != m->layers || d.rasterizationRateMap) return 1;
    // A memoryless attachment is never loaded: MoltenVK turns a resume's Load into DontCare for it.
    #define MG_LOADS(a) ((a).loadAction == MTLLoadActionLoad || \
                         ((a).loadAction == MTLLoadActionDontCare && ml_is((__bridge void *)(a).texture)))
    for (int i = 0; i < 8; i++) if (d.colorAttachments[i].texture && !MG_LOADS(d.colorAttachments[i])) return 2;
    if (d.depthAttachment.texture && !MG_LOADS(d.depthAttachment)) return 2;
    if (d.stencilAttachment.texture && !MG_LOADS(d.stencilAttachment)) return 2;
    #undef MG_LOADS
    // The resume's queries need its buffer; one without queries may run in an encoder that has one.
    if (n.vis && n.vis != m->vis) return 3;
    for (int i = 0; i < 10; i++)
        if (m->tex[i] && (!m->st_set[i] || (m->st[i] != MTLStoreActionStore && !ml_is(m->tex[i])))) return 4;
    return 0;
}
static kl_merge_t *mg_of_enc(void *e) { for (int i = 0; i < kMergeMax; i++) if (g_mg[i].enc == e) return &g_mg[i]; return NULL; }
static kl_merge_t *mg_held_on(void *cb) { for (int i = 0; i < kMergeMax; i++) if (g_mg[i].enc && g_mg[i].cb == cb && g_mg[i].held) return &g_mg[i]; return NULL; }

// The real end of a held encoder: its latest store actions, then endEncoding. Call without g_mg_lk.
static void mg_finish(kl_merge_t c) {
    id enc = (__bridge id)c.enc;
    for (int i = 0; i < 10; i++) if (c.st_set[i] && ml_is(c.tex[i])) c.st[i] = ml_store(c.st[i]);
    for (int i = 0; i < 8; i++) {
        if (c.st_set[i]) ((void (*)(id, SEL, MTLStoreAction, NSUInteger))g_scsa)(enc, @selector(setColorStoreAction:atIndex:), c.st[i], (NSUInteger)i);
        if (c.opt_set[i] && g_scso) ((void (*)(id, SEL, MTLStoreActionOptions, NSUInteger))g_scso)(enc, @selector(setColorStoreActionOptions:atIndex:), c.opt[i], (NSUInteger)i);
    }
    if (c.st_set[8]) ((void (*)(id, SEL, MTLStoreAction))g_sdsa)(enc, @selector(setDepthStoreAction:), c.st[8]);
    if (c.opt_set[8] && g_sdso) ((void (*)(id, SEL, MTLStoreActionOptions))g_sdso)(enc, @selector(setDepthStoreActionOptions:), c.opt[8]);
    if (c.st_set[9]) ((void (*)(id, SEL, MTLStoreAction))g_sssa)(enc, @selector(setStencilStoreAction:), c.st[9]);
    if (c.opt_set[9] && g_ssso) ((void (*)(id, SEL, MTLStoreActionOptions))g_ssso)(enc, @selector(setStencilStoreActionOptions:), c.opt[9]);
    ((void (*)(id, SEL))g_end)(enc, @selector(endEncoding));
    CFRelease(c.enc);
}
// End whatever is held on this command buffer, before any other use of it.
static void mg_flush_why(void *cb, const char *why) {
    if (!g_merge) return;
    os_unfair_lock_lock(&g_mg_lk);
    kl_merge_t *m = mg_held_on(cb), c;
    if (m) { c = *m; memset(m, 0, sizeof *m); }
    os_unfair_lock_unlock(&g_mg_lk);
    if (!m) return;
    TR("flush enc %p (%s) on cb %p by %s\n", c.enc, label_of(c.enc), cb, why ? why : "render pass/commit");
    if (why) {
        atomic_fetch_add(&c_flush_other, 1);
        static _Atomic unsigned said;
        if (g_dbg && atomic_fetch_add(&said, 1) < 12) fprintf(stderr, "[merge] held pass ended by %s\n", why);
    }
    mg_finish(c);
}

static void kl_end(id self, SEL sel) {
    os_unfair_lock_lock(&g_mg_lk);
    kl_merge_t *m = mg_of_enc((__bridge void *)self);
    if (m) {
        if (!m->held) { m->held = 1; m->resumed = 0; m->barrier = 0; atomic_fetch_add(&c_held, 1);
            TR("hold enc %p (%s) st c0 %lu z %lu s %lu\n", (__bridge void *)self, label_of((__bridge void *)self),
               (unsigned long)m->st[0], (unsigned long)m->st[8], (unsigned long)m->st[9]); }
        os_unfair_lock_unlock(&g_mg_lk);
        return;                          // ended for real by mg_flush_why
    }
    os_unfair_lock_unlock(&g_mg_lk);
    ((void (*)(id, SEL))g_end)(self, sel);
}
#define MG_CACHE(idx, field, flag, val) do { \
    os_unfair_lock_lock(&g_mg_lk); \
    kl_merge_t *m = mg_of_enc((__bridge void *)self); \
    if (m) { m->field[idx] = (val); m->flag[idx] = 1; os_unfair_lock_unlock(&g_mg_lk); return; } \
    os_unfair_lock_unlock(&g_mg_lk); } while (0)
static void kl_scsa(id self, SEL sel, MTLStoreAction a, NSUInteger i) {
    if (i < 8) MG_CACHE(i, st, st_set, a);
    ((void (*)(id, SEL, MTLStoreAction, NSUInteger))g_scsa)(self, sel, a, i);
}
static void kl_sdsa(id self, SEL sel, MTLStoreAction a) { MG_CACHE(8, st, st_set, a); ((void (*)(id, SEL, MTLStoreAction))g_sdsa)(self, sel, a); }
static void kl_sssa(id self, SEL sel, MTLStoreAction a) { MG_CACHE(9, st, st_set, a); ((void (*)(id, SEL, MTLStoreAction))g_sssa)(self, sel, a); }
static void kl_scso(id self, SEL sel, MTLStoreActionOptions o, NSUInteger i) {
    if (i < 8) MG_CACHE(i, opt, opt_set, o);
    ((void (*)(id, SEL, MTLStoreActionOptions, NSUInteger))g_scso)(self, sel, o, i);
}
static void kl_sdso(id self, SEL sel, MTLStoreActionOptions o) { MG_CACHE(8, opt, opt_set, o); ((void (*)(id, SEL, MTLStoreActionOptions))g_sdso)(self, sel, o); }
static void kl_ssso(id self, SEL sel, MTLStoreActionOptions o) { MG_CACHE(9, opt, opt_set, o); ((void (*)(id, SEL, MTLStoreActionOptions))g_ssso)(self, sel, o); }
#undef MG_CACHE
// A continued encoder already took its waits at its real start (see the header comment).
static void kl_wff(id self, SEL sel, id fence, MTLRenderStages st) {
    os_unfair_lock_lock(&g_mg_lk);
    kl_merge_t *m = mg_of_enc((__bridge void *)self);
    int drop = m && m->continued;
    os_unfair_lock_unlock(&g_mg_lk);
    if (drop) { atomic_fetch_add(&c_fence, 1); return; }
    ((void (*)(id, SEL, id, MTLRenderStages))g_wff)(self, sel, fence, st);
}

// Everything else a command buffer can do ends the held encoder first.
static id kl_m_cce0(id self, SEL sel) { mg_flush_why((__bridge void *)self, "computeCommandEncoder"); return ((id (*)(id, SEL))g_m_cce0)(self, sel); }
static id kl_m_cce(id self, SEL sel, MTLDispatchType t) { mg_flush_why((__bridge void *)self, "computeCommandEncoderWithDispatchType"); return ((id (*)(id, SEL, MTLDispatchType))g_m_cce)(self, sel, t); }
static id kl_m_cced(id self, SEL sel, id d) { mg_flush_why((__bridge void *)self, "computeCommandEncoderWithDescriptor"); return ((id (*)(id, SEL, id))g_m_cced)(self, sel, d); }
static id kl_m_bce(id self, SEL sel) { mg_flush_why((__bridge void *)self, "blitCommandEncoder"); return ((id (*)(id, SEL))g_m_bce)(self, sel); }
static id kl_m_bced(id self, SEL sel, id d) { mg_flush_why((__bridge void *)self, "blitCommandEncoderWithDescriptor"); return ((id (*)(id, SEL, id))g_m_bced)(self, sel, d); }
static id kl_m_prce(id self, SEL sel, id d) { mg_flush_why((__bridge void *)self, "parallelRenderCommandEncoder"); return ((id (*)(id, SEL, id))g_m_prce)(self, sel, d); }
static void kl_m_sig(id self, SEL sel, id ev, uint64_t v) { mg_flush_why((__bridge void *)self, "encodeSignalEvent"); ((void (*)(id, SEL, id, uint64_t))g_m_sig)(self, sel, ev, v); }
static void kl_m_wait(id self, SEL sel, id ev, uint64_t v) { mg_flush_why((__bridge void *)self, "encodeWaitForEvent"); ((void (*)(id, SEL, id, uint64_t))g_m_wait)(self, sel, ev, v); }
static void kl_m_pd(id self, SEL sel, id dr) { mg_flush_why((__bridge void *)self, "presentDrawable"); ((void (*)(id, SEL, id))g_m_pd)(self, sel, dr); }
static void kl_m_pdt(id self, SEL sel, id dr, CFTimeInterval t) { mg_flush_why((__bridge void *)self, "presentDrawable:atTime"); ((void (*)(id, SEL, id, CFTimeInterval))g_m_pdt)(self, sel, dr, t); }
static void kl_m_pdd(id self, SEL sel, id dr, CFTimeInterval t) { mg_flush_why((__bridge void *)self, "presentDrawable:afterMinimumDuration"); ((void (*)(id, SEL, id, CFTimeInterval))g_m_pdd)(self, sel, dr, t); }

// The resume marker (kl_vulkan.c, before every RESUMING vkCmdBeginRendering): swallowed, push and pop.
static void kl_push(id self, SEL sel, NSString *s) {
    void *cb = (__bridge void *)self;
    int res = s && [s isEqualToString:@KL_MTL_MERGE_RESUME_MARK];
    if (res || (s && [s isEqualToString:@KL_MTL_MERGE_BARRIER_MARK])) {
        os_unfair_lock_lock(&g_mg_lk);
        kl_merge_t *m = mg_held_on(cb);
        if (m && res) m->resumed = 1;
        if (m && !res) m->barrier = 1;
        os_unfair_lock_unlock(&g_mg_lk);
        TR("%s marker on cb %p (held %p)\n", res ? "resume" : "barrier", cb, m ? m->enc : NULL);
        t_swallow_pop = cb;
        return;
    }
    mg_flush_why(cb, "pushDebugGroup");
    ((void (*)(id, SEL, NSString *))g_push)(self, sel, s);
}
static void kl_pop(id self, SEL sel) {
    void *cb = (__bridge void *)self;
    if (t_swallow_pop == cb) { t_swallow_pop = NULL; return; }
    mg_flush_why(cb, "popDebugGroup");
    ((void (*)(id, SEL))g_pop)(self, sel);
}

static void hook(Class c, SEL s, IMP mine, IMP *orig) {
    Method m = class_getInstanceMethod(c, s);
    if (!m) { if (g_dbg) fprintf(stderr, "[merge] %s has no %s (not hooked)\n", class_getName(c), sel_getName(s)); return; }
    *orig = method_getImplementation(m);
    if (!class_addMethod(c, s, mine, method_getTypeEncoding(m))) method_setImplementation(m, mine);
}

// KL_VK_MERGE_VIS=1: the scene's first segment issues no occlusion queries and so gets no visibility-result buffer,
// while the segment that resumes it does; Metal fixes that buffer per encoder, so the two could not merge. The buffer
// a resumed MSAA scene segment brought (retained) is attached to the next first segment on the same attachments up
// front: with no visibility mode set, that segment writes nothing to it. A different buffer next time means no merge.
static void *g_vis_tex0, *g_vis_buf;

static id kl_rce(id self, SEL sel, MTLRenderPassDescriptor *d) {
    void *cb = (__bridge void *)self;
    int on = atomic_load(&g_phase) == 0;
    if (d) {
        id<MTLTexture> c0 = d.colorAttachments[0].texture;
        int msaa = c0 && c0.sampleCount > 1;
        if (on && g_merge_vis && msaa && !d.visibilityResultBuffer && g_vis_buf && (__bridge void *)c0 == g_vis_tex0) {
            d.visibilityResultBuffer = (__bridge id<MTLBuffer>)g_vis_buf;
            atomic_fetch_add(&c_vis, 1);
        }
        os_unfair_lock_lock(&g_mg_lk);
        kl_merge_t *m = mg_held_on(cb);
        int why = m ? mg_continues(m, d) : -1;
        if (msaa || m) TR("render pass on cb %p: c0 %p x%lu ld %lu z ld %lu vis %p; held %p resumed %d why %d\n", cb,
                          (__bridge void *)c0, (unsigned long)(c0 ? c0.sampleCount : 0), (unsigned long)d.colorAttachments[0].loadAction,
                          (unsigned long)d.depthAttachment.loadAction, (__bridge void *)d.visibilityResultBuffer,
                          m ? m->enc : NULL, m ? m->resumed : 0, why);
        if (m && !why && (m->resumed || g_merge_any) && (!m->barrier || g_merge_bar)) {
            m->held = 0; m->resumed = 0; m->continued = 1;
            id enc = (__bridge id)m->enc;
            os_unfair_lock_unlock(&g_mg_lk);
            atomic_fetch_add(&c_cont, 1);
            return enc;
        }
        if (m) {
            // No resume marker: a MoltenVK pass restart (the input-attachment read), not a resume.
            if (!m->resumed && !g_merge_any) atomic_fetch_add(&c_restart, 1);
            else if (!why && m->barrier) atomic_fetch_add(&c_barrier, 1);
            else atomic_fetch_add(&c_mismatch, 1);
            if (why == 3 && g_merge_vis && msaa && (__bridge void *)c0 == m->tex[0]) {   // remember it for the next frame
                void *nb = (__bridge void *)d.visibilityResultBuffer;
                if (nb != g_vis_buf) {
                    if (g_vis_buf) CFRelease(g_vis_buf);
                    g_vis_buf = (void *)CFBridgingRetain(d.visibilityResultBuffer);
                    g_vis_tex0 = m->tex[0];
                }
            }
            static int said;
            if (g_dbg && said < 12 && msaa) {
                said++;
                fprintf(stderr, "[merge] not continued: %s (load c0 %lu z %lu, marker %d)\n",
                        why == 0 ? "no resume marker (restart)" : why == 1 ? "other attachments/size" :
                        why == 2 ? "not every attachment loaded" : why == 3 ? "other visibility buffer" :
                        "held end was not a plain store", (unsigned long)d.colorAttachments[0].loadAction,
                        (unsigned long)d.depthAttachment.loadAction, m->resumed);
            }
        }
        os_unfair_lock_unlock(&g_mg_lk);
        if (m) mg_flush_why(cb, NULL);
    }
    ml_guard(d);
    id enc = ((id (*)(id, SEL, MTLRenderPassDescriptor *))g_rce)(self, sel, d);
    if (on && enc && d && d.colorAttachments[0].texture && d.colorAttachments[0].texture.sampleCount > 1 &&
        cb_watched((id<MTLCommandBuffer>)self)) {
        static Class hooked;
        Class ec = object_getClass(enc);
        if (!hooked) {           // the render encoder class: endEncoding, the store actions, fence waits
            hooked = ec;
            hook(ec, @selector(endEncoding), (IMP)kl_end, &g_end);
            hook(ec, @selector(setColorStoreAction:atIndex:), (IMP)kl_scsa, &g_scsa);
            hook(ec, @selector(setDepthStoreAction:), (IMP)kl_sdsa, &g_sdsa);
            hook(ec, @selector(setStencilStoreAction:), (IMP)kl_sssa, &g_sssa);
            hook(ec, @selector(setColorStoreActionOptions:atIndex:), (IMP)kl_scso, &g_scso);
            hook(ec, @selector(setDepthStoreActionOptions:), (IMP)kl_sdso, &g_sdso);
            hook(ec, @selector(setStencilStoreActionOptions:), (IMP)kl_ssso, &g_ssso);
            hook(ec, @selector(waitForFence:beforeStages:), (IMP)kl_wff, &g_wff);
            fprintf(stderr, "[merge] deferring the end of guest MSAA render encoders (%s)\n", class_getName(ec));
        }
        if (ec != hooked || !g_end || !g_scsa || !g_sdsa || !g_sssa) {
            atomic_fetch_add(&c_untracked, 1);
        } else {
            os_unfair_lock_lock(&g_mg_lk);
            for (int i = 0; i < kMergeMax; i++)
                if (!g_mg[i].enc) {
                    memset(&g_mg[i], 0, sizeof g_mg[i]);
                    g_mg[i].enc = (void *)CFBridgingRetain(enc);
                    g_mg[i].cb = cb;
                    mg_fill(&g_mg[i], d);
                    TR("track enc %p on cb %p\n", g_mg[i].enc, cb);
                    break;
                }
            os_unfair_lock_unlock(&g_mg_lk);
        }
    }
    return enc;
}

static void kl_commit(id self, SEL sel) {
    TR("commit cb %p\n", (__bridge void *)self);
    mg_flush_why((__bridge void *)self, NULL);
    if (g_gpu_time) {
        id<MTLCommandBuffer> cb = (id<MTLCommandBuffer>)self;
        void *q = (__bridge void *)[cb commandQueue];
        os_unfair_lock_lock(&g_lk);
        int w = watched(q);
        uint32_t f = g_frame;
        if (w) slot_of(f)->committed++;
        os_unfair_lock_unlock(&g_lk);
        if (w) [cb addCompletedHandler:^(id<MTLCommandBuffer> done) { on_complete(f, done.GPUStartTime, done.GPUEndTime); }];
    }
    ((void (*)(id, SEL))g_commit)(self, sel);
}

static void install(id<MTLCommandQueue> q) {
    Class c, cu;
    @autoreleasepool {
        c = object_getClass([q commandBuffer]);
        cu = object_getClass([q commandBufferWithUnretainedReferences]);
    }
    if (cu != c)
        fprintf(stderr, "[merge] WARNING: unretained-reference command buffers are %s, not %s; those are not hooked\n",
                class_getName(cu), class_getName(c));
    hook(c, @selector(commit), (IMP)kl_commit, &g_commit);
    if (g_merge) {
        hook(c, @selector(renderCommandEncoderWithDescriptor:), (IMP)kl_rce, &g_rce);
        hook(c, @selector(computeCommandEncoder), (IMP)kl_m_cce0, &g_m_cce0);
        hook(c, @selector(computeCommandEncoderWithDispatchType:), (IMP)kl_m_cce, &g_m_cce);
        hook(c, @selector(computeCommandEncoderWithDescriptor:), (IMP)kl_m_cced, &g_m_cced);
        hook(c, @selector(blitCommandEncoder), (IMP)kl_m_bce, &g_m_bce);
        hook(c, @selector(blitCommandEncoderWithDescriptor:), (IMP)kl_m_bced, &g_m_bced);
        hook(c, @selector(parallelRenderCommandEncoderWithDescriptor:), (IMP)kl_m_prce, &g_m_prce);
        hook(c, @selector(encodeSignalEvent:value:), (IMP)kl_m_sig, &g_m_sig);
        hook(c, @selector(encodeWaitForEvent:value:), (IMP)kl_m_wait, &g_m_wait);
        hook(c, @selector(presentDrawable:), (IMP)kl_m_pd, &g_m_pd);
        hook(c, @selector(presentDrawable:atTime:), (IMP)kl_m_pdt, &g_m_pdt);
        hook(c, @selector(presentDrawable:afterMinimumDuration:), (IMP)kl_m_pdd, &g_m_pdd);
        hook(c, @selector(pushDebugGroup:), (IMP)kl_push, &g_push);
        hook(c, @selector(popDebugGroup), (IMP)kl_pop, &g_pop);
        fprintf(stderr, "[merge] on (KL_VK_MERGE%s%s%s): one Metal encoder across resumed MSAA segments (%s)\n",
                g_merge_vis ? ", _VIS" : "", g_merge_any ? ", _ANY" : "", g_ab > 0 ? ", _AB" : "", class_getName(c));
        if (g_ab > 0) fprintf(stderr, "[merge] A/B: merged / stock every %.1f s of guest frames\n", g_ab);
    }
    if (g_gpu_time) fprintf(stderr, "[vkgpu] timing the guest queue's command buffers (%s)\n", class_getName(c));
}

void kl_mtl_merge_watch_queue(void *mtl_queue) {
    if (!mtl_queue || !kl_mtl_merge_wanted()) return;
    os_unfair_lock_lock(&g_lk);
    int have = watched(mtl_queue), first = !g_commit && !g_nqueues;
    if (!have && g_nqueues < kMaxQueues) g_queues[g_nqueues++] = mtl_queue;
    if (first) g_t0 = now_s();
    os_unfair_lock_unlock(&g_lk);
    if (first) install((__bridge id<MTLCommandQueue>)mtl_queue);
}

// ---- per-frame accounting and the report -------------------------------------------
static double ab_since_flip(double now, int *phase) {
    double t = now - g_t0;
    long k = (long)(t / g_ab);
    *phase = (int)(k & 1);
    return t - (double)k * g_ab;
}

void kl_mtl_merge_frame_end(void) {
    if (!g_commit) return;
    double now = now_s();
    int phase = 0, settled = 1;
    if (g_ab > 0) settled = ab_since_flip(now, &phase) >= 1.0;
    atomic_store(&g_phase, phase);
    // Close this frame, open the next with the phase its encoders will use.
    static double sum, worst, ab_busy[2];
    static unsigned n, ab_n[2];
    os_unfair_lock_lock(&g_lk);
    g_frame++;
    slot *s = slot_of(g_frame);
    s->phase = phase; s->settled = settled;
    // Every earlier frame whose command buffers have all completed is final.
    for (int i = 0; i < kRing; i++) {
        slot *o = &g_slots[i];
        if (!o->frame || o->done || o->frame >= g_frame) continue;
        if (o->committed && o->completed < o->committed) continue;
        o->done = 1;
        if (!o->committed) continue;
        double ms = o->busy * 1e3;
        sum += ms; n++; if (ms > worst) worst = ms;
        if (g_ab > 0 && o->settled) { ab_busy[o->phase] += ms; ab_n[o->phase]++; }
    }
    os_unfair_lock_unlock(&g_lk);
    static double since;
    if (!since) since = now;
    if (now - since < g_log_every) return;
    double span = now - since;
    since = now;
    time_t tt = time(NULL);
    struct tm lt;
    localtime_r(&tt, &lt);
    if (g_gpu_time && n)
        fprintf(stderr, "[vkgpu] %02d:%02d:%02d GPU time per frame: avg %.2f / worst %.2f ms over %u frames (%.1f fps)%s\n",
                lt.tm_hour, lt.tm_min, lt.tm_sec, sum / n, worst, n, n / span,
                g_ab > 0 ? (phase ? " [phase: stock]" : " [phase: merged]") : "");
    sum = worst = 0; n = 0;
    if (g_merge) {
        fprintf(stderr, "[merge] last %.1f s: %u held, %u continued, %u MoltenVK restarts kept apart, %u resumes after a barrier kept apart, %u resumes not continued, "
                "%u real ends by other work, %u fence waits dropped, %u visibility buffers given, %u untracked\n", span,
                atomic_exchange(&c_held, 0), atomic_exchange(&c_cont, 0), atomic_exchange(&c_restart, 0),
                atomic_exchange(&c_barrier, 0), atomic_exchange(&c_mismatch, 0), atomic_exchange(&c_flush_other, 0), atomic_exchange(&c_fence, 0),
                atomic_exchange(&c_vis, 0), atomic_exchange(&c_untracked, 0));
    }
    if (g_ab > 0 && ab_n[0] && ab_n[1]) {
        double a = ab_busy[0] / ab_n[0], b = ab_busy[1] / ab_n[1];
        fprintf(stderr, "[merge] A/B so far: merged %.3f GPU ms/frame over %u frames | stock %.3f over %u | merged %+.1f%% GPU time\n",
                a, ab_n[0], b, ab_n[1], b > 0 ? 100.0 * (a - b) / b : 0.0);
    }
}
