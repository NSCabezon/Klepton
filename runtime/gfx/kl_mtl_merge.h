// One Metal render encoder across Vulkan dynamic-rendering suspend/resume
// segments, done above MoltenVK (stock MoltenVK, no fork), plus GPU time per
// frame read from Metal. All of it is behind knobs; with none set nothing is
// swizzled and nothing changes. See kl_mtl_merge.m and DEBUG_ENV_VARS.md.
#pragma once

// Any knob that needs the Metal hooks (KL_VK_MERGE, KL_VK_GPU_TIME / KL_MVK_GPU_TIME,
// KL_VK_MERGE_AB). Cheap; read once.
int kl_mtl_merge_wanted(void);
// KL_VK_MERGE=1 (kl_vulkan.c then marks every RESUMING vkCmdBeginRendering).
int kl_mtl_merge_on(void);
// The debug-group name kl_vulkan.c records right before a RESUMING
// vkCmdBeginRendering; the hooks swallow it and use it to tell a resume from a
// MoltenVK pass restart.
#define KL_MTL_MERGE_RESUME_MARK "kl-resume"
// ...and right before every vkCmdPipelineBarrier(2): a held encoder that sees it is not continued.
#define KL_MTL_MERGE_BARRIER_MARK "kl-barrier"
// The guest's MTLCommandQueue (id<MTLCommandQueue>, unretained). The first call
// installs the hooks; later calls only add queues to the watched set.
void kl_mtl_merge_watch_queue(void *mtl_queue);
// The guest's frame ended (kl_vulkan_frame_done).
void kl_mtl_merge_frame_end(void);
