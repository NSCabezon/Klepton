// GPU time per guest frame, read from Metal (kl_gpu_time.m). KL_GPU_TIME=1|2;
// unset, every call here returns at once and -[MTLCommandBuffer commit] is
// never touched.
// Adapted from Glovebox (MIT, github.com/glovebox-vr/glovebox) runtime/gfx/kl_gpu_time.h.
#ifndef KL_GPU_TIME_H
#define KL_GPU_TIME_H
#ifdef __cplusplus
extern "C" {
#endif

// 0 off, 1 log only, 2 log and report it to the game (GetFrameTiming).
int kl_gpu_time_mode(void);
// Time every command buffer committed on this MTLCommandQueue (the guest's:
// kl_vulkan.c hands over each queue the guest submits on).
void kl_gpu_time_watch(void *mtl_queue);
// The game has submitted both eyes: buffers committed from now on are the next frame's.
void kl_gpu_time_frame_end(void);
// The guest's GPU ms of a finished frame `frames_ago` closed frames back (0 when unknown).
float kl_gpu_time_frame_ms(unsigned frames_ago);

#ifdef __cplusplus
}
#endif
#endif
