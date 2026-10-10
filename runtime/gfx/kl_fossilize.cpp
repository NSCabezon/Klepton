// kl_fossilize.cpp — the guest's Vulkan pipelines, recorded and replayed with Valve's Fossilize.
//
// Adapted from Glovebox (MIT, github.com/glovebox-vr/glovebox) klepton/runtime/linux/kl_fossilize.cpp
// Fossilize: github.com/ValveSoftware/Fossilize (MIT), fetched by `make fossilize` into
// vendor-fossilize/ at a pinned revision; not part of this repository.
//
// Source 2 creates its pipelines lazily, one at a time as new views appear, and through MoltenVK
// a pipeline the Metal compiler has not seen costs 330-580 ms (a walk into a new area froze the
// frame for up to 9 s). The OS Metal cache keeps compiled pipelines across launches, so the fix
// is to have compiled everything BEFORE the guest asks:
//
//   KL_FOZ_RECORD=<file.foz>   record every sampler, set/pipeline layout, shader module, render
//       pass and pipeline the guest creates (its own create infos), appending to a Fossilize
//       stream archive. Flushed per record (KL_FOZ_RECORD_SYNC=0: a background writer).
//   KL_FOZ_REPLAY=<file.foz|dir>[:...]   at vkCreateDevice, replay those databases on
//       KL_FOZ_THREADS threads (default 4) through the layer's own entry points, so each
//       pipeline goes through the same fix-ups and MoltenVK state as the guest's and produces
//       the same MSL, which is what the Metal cache is keyed on. Each replayed pipeline is
//       destroyed at once; only the warm cache is kept. In the background by default;
//       KL_FOZ_REPLAY_BLOCK=1 holds vkCreateDevice until it is done.
//
// Neither set: kl_fossilize_install installs nothing and the layer's hooks stay NULL.
#include "fossilize.hpp"
#include "fossilize_db.hpp"
#include <vulkan/vulkan.h>
#include "kl_vk_pipe_hooks.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <dirent.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

using namespace Fossilize;

namespace {

double now_s() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

bool knob_on(const char *v) { return v && *v && strcmp(v, "0"); }

// ---- recording ---------------------------------------------------------------------------
StateRecorder *g_rec;
DatabaseInterface *g_rec_db;
std::atomic<unsigned> g_rec_pipes{0}, g_rec_fail{0};

// Replay progress for a loading screen (kl_foz_progress): pipelines listed so far, pipelines
// compiled (or failed) so far, and the state: 0 no replay, 1 replaying, 2 done.
std::atomic<unsigned> g_prog_total{0}, g_prog_done{0};
std::atomic<int> g_prog_state{0};
std::atomic<double> g_prog_t0{0};

void rec_init(const char *path) {
    g_rec_db = create_stream_archive_database(path, DatabaseMode::Append);
    if (!g_rec_db || !g_rec_db->prepare()) {
        fprintf(stderr, "  [foz] cannot open %s for recording; recording off\n", path);
        delete g_rec_db;
        g_rec_db = nullptr;
        return;
    }
    g_rec = new StateRecorder;
    g_rec->set_database_enable_compression(true);
    g_rec->set_database_enable_checksum(true);
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr, "hlvr", 1, "Source2", 1, VK_API_VERSION_1_1 };
    if (!g_rec->record_application_info(app)) fprintf(stderr, "  [foz] application info not recorded\n");
    // Synchronized by default: each record is written and flushed before the create returns, so
    // a crash loses nothing (Glovebox lost queued records to server crashes with the thread).
    bool sync = !(getenv("KL_FOZ_RECORD_SYNC") && !strcmp(getenv("KL_FOZ_RECORD_SYNC"), "0"));
    if (sync) g_rec->init_recording_synchronized(g_rec_db);
    else g_rec->init_recording_thread(g_rec_db);
    fprintf(stderr, "  [foz] recording the guest's pipelines to %s (%s)\n", path,
            sync ? "synchronized: flushed per record" : "recording thread");
}

void note_pipe(bool ok) {
    unsigned n = ok ? ++g_rec_pipes : g_rec_pipes.load();
    if (!ok) ++g_rec_fail;
    if (ok && (n & (n - 1)) == 0 && n >= 64)
        fprintf(stderr, "  [foz] %u pipelines recorded (%u refused)\n", n, g_rec_fail.load());
}

void h_sampler(VkSampler h, const VkSamplerCreateInfo *ci) { if (g_rec) (void)g_rec->record_sampler(h, *ci); }
void h_set_layout(VkDescriptorSetLayout h, const VkDescriptorSetLayoutCreateInfo *ci) { if (g_rec) (void)g_rec->record_descriptor_set_layout(h, *ci); }
void h_pipeline_layout(VkPipelineLayout h, const VkPipelineLayoutCreateInfo *ci) { if (g_rec) (void)g_rec->record_pipeline_layout(h, *ci); }
void h_shader_module(VkShaderModule h, const VkShaderModuleCreateInfo *ci) { if (g_rec) (void)g_rec->record_shader_module(h, *ci); }
void h_render_pass(VkRenderPass h, const VkRenderPassCreateInfo *ci) { if (g_rec) (void)g_rec->record_render_pass(h, *ci); }
void h_render_pass2(VkRenderPass h, const VkRenderPassCreateInfo2 *ci) { if (g_rec) (void)g_rec->record_render_pass2(h, *ci); }
void h_graphics_pipeline(VkPipeline h, const VkGraphicsPipelineCreateInfo *ci) {
    if (g_rec) note_pipe(g_rec->record_graphics_pipeline(h, *ci, nullptr, 0));
}
void h_compute_pipeline(VkPipeline h, const VkComputePipelineCreateInfo *ci) {
    if (g_rec) note_pipe(g_rec->record_compute_pipeline(h, *ci, nullptr, 0));
}
// The guest's device features, in the recording's application info.
void h_device_info(const VkDeviceCreateInfo *ci) {
    if (!g_rec) return;
    bool ok = ci->pEnabledFeatures ? g_rec->record_physical_device_features(*ci->pEnabledFeatures)
                                   : g_rec->record_physical_device_features(ci->pNext);
    if (!ok) fprintf(stderr, "  [foz] device features not recorded\n");
}

// ---- replay ------------------------------------------------------------------------------
struct Fns {
    PFN_vkCreateSampler CreateSampler;
    PFN_vkDestroySampler DestroySampler;
    PFN_vkCreateDescriptorSetLayout CreateDescriptorSetLayout;
    PFN_vkDestroyDescriptorSetLayout DestroyDescriptorSetLayout;
    PFN_vkCreatePipelineLayout CreatePipelineLayout;
    PFN_vkDestroyPipelineLayout DestroyPipelineLayout;
    PFN_vkCreateShaderModule CreateShaderModule;
    PFN_vkDestroyShaderModule DestroyShaderModule;
    PFN_vkCreateRenderPass CreateRenderPass;
    PFN_vkCreateRenderPass2 CreateRenderPass2;
    PFN_vkDestroyRenderPass DestroyRenderPass;
    PFN_vkCreateGraphicsPipelines CreateGraphicsPipelines;
    PFN_vkCreateComputePipelines CreateComputePipelines;
    PFN_vkDestroyPipeline DestroyPipeline;
};

struct Job { bool graphics; const void *ci; };

// Creates the dependency objects inline (the parse thread) and queues the pipelines for the
// compile threads. Fossilize keeps every parsed create info alive while the StateReplayer lives.
struct Creator : StateCreatorInterface {
    VkDevice dev;
    const Fns &f;
    std::vector<VkSampler> samplers;
    std::vector<VkDescriptorSetLayout> set_layouts;
    std::vector<VkPipelineLayout> layouts;
    std::vector<VkShaderModule> modules;
    std::vector<VkRenderPass> passes;
    std::mutex mu;
    std::condition_variable cv;
    std::deque<Job> queue;
    bool closed = false;
    unsigned queued = 0, compiled = 0, failed = 0, slow = 0;
    double compile_s = 0;
    unsigned dep_fails = 0;

    Creator(VkDevice d, const Fns &fns) : dev(d), f(fns) {}
    void dep_fail(const char *what, VkResult r) {
        if (dep_fails++ < 4) fprintf(stderr, "  [foz] replay: creating a %s failed (VkResult %d)\n", what, (int)r);
    }

    bool enqueue_create_sampler(Hash, const VkSamplerCreateInfo *ci, VkSampler *out) override {
        *out = VK_NULL_HANDLE;
        if (VkResult r = f.CreateSampler(dev, ci, nullptr, out)) { dep_fail("sampler", r); return false; }
        samplers.push_back(*out);
        return true;
    }
    bool enqueue_create_descriptor_set_layout(Hash, const VkDescriptorSetLayoutCreateInfo *ci, VkDescriptorSetLayout *out) override {
        *out = VK_NULL_HANDLE;
        if (VkResult r = f.CreateDescriptorSetLayout(dev, ci, nullptr, out)) { dep_fail("set_layout", r); return false; }
        set_layouts.push_back(*out);
        return true;
    }
    bool enqueue_create_pipeline_layout(Hash, const VkPipelineLayoutCreateInfo *ci, VkPipelineLayout *out) override {
        *out = VK_NULL_HANDLE;
        if (VkResult r = f.CreatePipelineLayout(dev, ci, nullptr, out)) { dep_fail("layout", r); return false; }
        layouts.push_back(*out);
        return true;
    }
    bool enqueue_create_shader_module(Hash, const VkShaderModuleCreateInfo *ci, VkShaderModule *out) override {
        *out = VK_NULL_HANDLE;
        if (VkResult r = f.CreateShaderModule(dev, ci, nullptr, out)) { dep_fail("module", r); return false; }
        modules.push_back(*out);
        return true;
    }
    bool enqueue_create_render_pass(Hash, const VkRenderPassCreateInfo *ci, VkRenderPass *out) override {
        *out = VK_NULL_HANDLE;
        if (VkResult r = f.CreateRenderPass(dev, ci, nullptr, out)) { dep_fail("pass", r); return false; }
        passes.push_back(*out);
        return true;
    }
    bool enqueue_create_render_pass2(Hash, const VkRenderPassCreateInfo2 *ci, VkRenderPass *out) override {
        *out = VK_NULL_HANDLE;
        if (!f.CreateRenderPass2 || f.CreateRenderPass2(dev, ci, nullptr, out) != VK_SUCCESS) return false;
        passes.push_back(*out);
        return true;
    }
    void push(bool graphics, const void *ci) {
        std::lock_guard<std::mutex> l(mu);
        queue.push_back({ graphics, ci });
        queued++;
        cv.notify_one();
    }
    bool enqueue_create_graphics_pipeline(Hash, const VkGraphicsPipelineCreateInfo *ci, VkPipeline *out) override {
        *out = VK_NULL_HANDLE;
        push(true, ci);
        return true;
    }
    bool enqueue_create_compute_pipeline(Hash, const VkComputePipelineCreateInfo *ci, VkPipeline *out) override {
        *out = VK_NULL_HANDLE;
        push(false, ci);
        return true;
    }
    bool enqueue_create_raytracing_pipeline(Hash, const VkRayTracingPipelineCreateInfoKHR *, VkPipeline *out) override {
        *out = VK_NULL_HANDLE;
        return true;
    }

    void worker() {
        klvk_set_replay_thread(1);
        for (;;) {
            Job j;
            {
                std::unique_lock<std::mutex> l(mu);
                cv.wait(l, [&] { return closed || !queue.empty(); });
                if (queue.empty()) return;
                j = queue.front();
                queue.pop_front();
            }
            double t0 = now_s();
            VkPipeline p = VK_NULL_HANDLE;
            VkResult r = j.graphics
                ? f.CreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, (const VkGraphicsPipelineCreateInfo *)j.ci, nullptr, &p)
                : f.CreateComputePipelines(dev, VK_NULL_HANDLE, 1, (const VkComputePipelineCreateInfo *)j.ci, nullptr, &p);
            double dt = now_s() - t0;
            if (p) f.DestroyPipeline(dev, p, nullptr);
            std::lock_guard<std::mutex> l(mu);
            compiled++;
            g_prog_done.fetch_add(1, std::memory_order_relaxed);
            compile_s += dt;
            if (dt > 0.1) slow++;
            if (r != VK_SUCCESS) failed++;
        }
    }
    void destroy_objects() {
        for (auto h : modules) f.DestroyShaderModule(dev, h, nullptr);
        for (auto h : layouts) f.DestroyPipelineLayout(dev, h, nullptr);
        for (auto h : set_layouts) f.DestroyDescriptorSetLayout(dev, h, nullptr);
        for (auto h : passes) f.DestroyRenderPass(dev, h, nullptr);
        for (auto h : samplers) f.DestroySampler(dev, h, nullptr);
    }
};

std::vector<std::string> replay_files() {
    std::vector<std::string> out;
    const char *e = getenv("KL_FOZ_REPLAY");
    if (!knob_on(e)) return out;
    std::string list = e;
    size_t pos = 0;
    while (pos <= list.size()) {
        size_t c = list.find(':', pos);
        std::string item = list.substr(pos, c == std::string::npos ? std::string::npos : c - pos);
        pos = c == std::string::npos ? list.size() + 1 : c + 1;
        if (item.empty()) continue;
        struct stat st;
        if (stat(item.c_str(), &st) != 0) {
            fprintf(stderr, "  [foz] replay: %s does not exist; skipped\n", item.c_str());
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            if (DIR *d = opendir(item.c_str())) {
                while (struct dirent *de = readdir(d)) {
                    size_t n = strlen(de->d_name);
                    if (n > 4 && !strcmp(de->d_name + n - 4, ".foz")) out.push_back(item + "/" + de->d_name);
                }
                closedir(d);
            }
        } else {
            out.push_back(item);
        }
    }
    return out;
}

void replay_db(VkDevice dev, const Fns &f, const std::string &path, unsigned nthreads) {
    DatabaseInterface *db = create_stream_archive_database(path.c_str(), DatabaseMode::ReadOnly);
    if (!db || !db->prepare()) {
        fprintf(stderr, "  [foz] replay: cannot open %s\n", path.c_str());
        delete db;
        return;
    }
    double t0 = now_s();
    StateReplayer replayer;
    Creator creator(dev, f);
    std::vector<std::thread> workers;
    for (unsigned i = 0; i < nthreads; i++) workers.emplace_back([&] {
        pthread_setname_np("klfz replay");
        pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
        creator.worker();
    });
    unsigned parse_fail = 0;
    std::vector<uint8_t> buf;
    // Dependencies first: the replayer resolves pipeline layouts and render passes only from
    // objects already replayed (shader modules and samplers it can also pull from the database).
    for (ResourceTag tag : { RESOURCE_SAMPLER, RESOURCE_DESCRIPTOR_SET_LAYOUT, RESOURCE_PIPELINE_LAYOUT,
                             RESOURCE_RENDER_PASS, RESOURCE_SHADER_MODULE,
                             RESOURCE_GRAPHICS_PIPELINE, RESOURCE_COMPUTE_PIPELINE }) {
        size_t n = 0;
        if (!db->get_hash_list_for_resource_tag(tag, &n, nullptr)) continue;
        std::vector<Hash> hashes(n);
        if (!db->get_hash_list_for_resource_tag(tag, &n, hashes.data())) continue;
        if (tag == RESOURCE_GRAPHICS_PIPELINE || tag == RESOURCE_COMPUTE_PIPELINE)
            g_prog_total.fetch_add((unsigned)n, std::memory_order_relaxed);
        for (Hash h : hashes) {
            size_t sz = 0;
            if (!db->read_entry(tag, h, &sz, nullptr, PAYLOAD_READ_NO_FLAGS)) { parse_fail++; continue; }
            buf.resize(sz);
            if (!db->read_entry(tag, h, &sz, buf.data(), PAYLOAD_READ_NO_FLAGS)) { parse_fail++; continue; }
            if (!replayer.parse(creator, db, buf.data(), sz)) parse_fail++;
        }
    }
    {
        std::lock_guard<std::mutex> l(creator.mu);
        creator.closed = true;
        creator.cv.notify_all();
    }
    for (auto &t : workers) t.join();
    creator.destroy_objects();
    delete db;
    fprintf(stderr, "  [foz] replay %s: %u pipelines (%u failed, %u unparsed) in %.1f s on %u threads; "
                    "%.1f s of compiles, %u over 100 ms\n",
            path.c_str(), creator.compiled, creator.failed, parse_fail, now_s() - t0, nthreads,
            creator.compile_s, creator.slow);
}

void h_device_created(VkDevice dev, PFN_vkGetDeviceProcAddr gdpa) {
    std::vector<std::string> files = replay_files();
    if (files.empty()) return;
    static bool once;
    if (once) return;   // the guest's first device only
    once = true;
    static Fns f;
#define L(n) f.n = (PFN_vk##n)gdpa(dev, "vk" #n)
    L(CreateSampler); L(DestroySampler); L(CreateDescriptorSetLayout); L(DestroyDescriptorSetLayout);
    L(CreatePipelineLayout); L(DestroyPipelineLayout); L(CreateShaderModule); L(DestroyShaderModule);
    L(CreateRenderPass); L(CreateRenderPass2); L(DestroyRenderPass);
    L(CreateGraphicsPipelines); L(CreateComputePipelines); L(DestroyPipeline);
#undef L
    unsigned nthreads = 4;
    if (const char *t = getenv("KL_FOZ_THREADS")) nthreads = (unsigned)atoi(t);
    if (nthreads < 1) nthreads = 1;
    bool block = knob_on(getenv("KL_FOZ_REPLAY_BLOCK"));
    fprintf(stderr, "  [foz] replaying %zu database(s) on %u threads to warm the Metal cache%s\n",
            files.size(), nthreads, block ? " (holding device creation until done)" : " in the background");
    auto run = [dev, files, nthreads, block] {
        pthread_setname_np("klfz replay main");
        klvk_set_replay_thread(1);
        // Behind the guest's own loading: the utility class, not the render path's.
        if (!block) pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
        double t0 = now_s();
        g_prog_t0.store(t0); g_prog_state.store(1);
        for (const auto &p : files) replay_db(dev, f, p, nthreads);
        g_prog_state.store(2);
        fprintf(stderr, "  [foz] replay done in %.1f s\n", now_s() - t0);
        klvk_set_replay_thread(0);
    };
    if (block) run();
    else std::thread(run).detach();
}

const klvk_pipe_hooks g_hooks = {
    h_device_created, h_sampler, h_set_layout, h_pipeline_layout, h_shader_module,
    h_render_pass, h_render_pass2, h_graphics_pipeline, h_compute_pipeline, h_device_info,
};

} // namespace

// Replay progress for a loading screen: 0 (no replay this run), 1 (replaying) or 2 (done);
// `done`/`total` are pipelines compiled and listed so far, `elapsed` seconds since it began.
extern "C" int kl_foz_progress(unsigned *done, unsigned *total, double *elapsed) {
    int st = g_prog_state.load();
    if (done) *done = g_prog_done.load();
    if (total) *total = g_prog_total.load();
    if (elapsed) *elapsed = st ? now_s() - g_prog_t0.load() : 0;
    return st;
}

extern "C" void kl_fossilize_install(void) {
    static bool done;
    if (done) return;
    done = true;
    const char *r = getenv("KL_FOZ_RECORD");
    bool rec = knob_on(r), rep = knob_on(getenv("KL_FOZ_REPLAY"));
    if (!rec && !rep) return;
    if (rec) rec_init(r);
    klvk_set_pipe_hooks(&g_hooks);
}
