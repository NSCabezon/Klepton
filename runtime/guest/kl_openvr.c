// An OpenVR runtime of our own, for the glibc door — see kl_openvr.h.
//
// WHICH LAYER
//
// Alyx's libvr.so imports seven entry points from libopenvr_api.so
// (VR_InitInternal2, VR_GetGenericInterface, VR_IsInterfaceVersionValid,
// VR_GetInitToken, VR_ShutdownInternal and the two error-name calls) and then
// talks to the C++ interface objects VR_GetGenericInterface returns — never
// FnTable, never a vrserver of its own. libopenvr_api's half of that is only a
// client that looks for SteamVR (openvrpaths.vrpath) and fails without it. So,
// like the Steam stub, this answers at the export layer: those imports bind
// here through the glibc ABI tier, and the objects handed back are ours, with
// vtables generated from OpenVR's public openvr.h (kl_openvr_vtables.h,
// tools/gen_openvr_vtables.py). Every slot exists; unimplemented ones log
// "IVRx_nnn::Method (#slot)" and return 0, which for the error-returning ones
// is "None". Private versions (IVRInputInternal_002, IVRMailbox_001,
// IVRIPCResourceManagerClient_004) and ones that differ from the public header
// (IVRApplications_007) get 128 logging slots.
//
// WHERE THE ANSWERS COME FROM
//
// kl_ovrp is the one XR state hub in this tree: the frontend (the macOS viewer,
// ARKit + Compositor Services on device, or nothing — a headless run gets a
// head standing at eye height with the hands parked in front) publishes head,
// hands, controller input and the eye frustum there, and kl_openxr already
// reads it back for OpenXR guests. This is a third reader of the same seam:
//
//   IVRSystem      device 0 = HMD, 1 = left hand, 2 = right hand (Touch-style
//                  controllers, "oculus_touch"); eye frusta, eye-to-head and the
//                  render-target size from kl_ovrp_eye_view / _eye_texture_size
//   IVRCompositor  WaitGetPoses = the frame latch; Submit copies the guest's
//                  VkImage (VRVulkanTextureData_t) into an MTLTexture-backed
//                  image of ours on the guest's own queue, and the frame is
//                  filed through exactly the seams kl_openxr files an OpenXR
//                  Vulkan frame through (kl_glfb_note_eye_mtl_texture,
//                  kl_ovrp_frame_end_external, kl_vulkan_frame_done) — so
//                  whatever composites an OpenXR guest composites this one
//   IVRInput       Alyx's own action set (the names are in libclient), bound
//                  by a built-in table to Touch-style inputs: trigger, grip,
//                  stick, A/B/X/Y, menu — see k_bind below
//   IVRChaperone   a 2 x 2 m play area
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdatomic.h>
#include <time.h>

#include "klepton.h"
#include "kl_env.h"
#include "kl_ovrp.h"
#include "kl_glfb.h"
#include "kl_vulkan.h"
#include "kl_openvr.h"

#if __has_include(<vulkan/vulkan.h>)
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#define KLOVR_VULKAN 1
#endif

#define TRACE(...) do { if (kl_env_on("KL_TRACE_OPENVR", 0)) fprintf(stderr, "  [openvr] " __VA_ARGS__); } while (0)

// ---- openvr.h types, the ones crossing the ABI (layouts measured: arm64) -------------
typedef struct { float m[3][4]; } HmdMatrix34_t;
typedef struct { float m[4][4]; } HmdMatrix44_t;
typedef struct {                       // 80 bytes
    HmdMatrix34_t mDeviceToAbsoluteTracking;
    float vVelocity[3], vAngularVelocity[3];
    int32_t eTrackingResult;           // TrackingResult_Running_OK = 200
    uint8_t bPoseIsValid, bDeviceIsConnected, pad[2];
} TrackedDevicePose_t;
typedef struct { const void *pVertexData; uint32_t unTriangleCount; } HiddenAreaMesh_t;
typedef struct { void *handle; int32_t eType, eColorSpace; } Texture_t;           // eType 2 = Vulkan
typedef struct {                       // 64 bytes
    uint64_t m_nImage;
    void *m_pDevice, *m_pPhysicalDevice, *m_pInstance, *m_pQueue;
    uint32_t m_nQueueFamilyIndex, m_nWidth, m_nHeight, m_nFormat, m_nSampleCount;
} VRVulkanTextureData_t;
typedef struct { float uMin, vMin, uMax, vMax; } VRTextureBounds_t;
typedef struct { uint64_t ulActionSet, ulRestrictedToDevice, ulSecondaryActionSet;
                 uint32_t unPadding; int32_t nPriority; } VRActiveActionSet_t;

enum { DEV_HMD = 0, DEV_LEFT = 1, DEV_RIGHT = 2, NDEV = 3 };

// ---- interface objects -------------------------------------------------------------------
typedef struct { const char *ver, *cls; const char *const *slots; } iface_desc;
#define KL_OVR_IFACE(ver, cls, ...) { ver, cls, (const char *const[]){ __VA_ARGS__, NULL } },
static const iface_desc g_desc[] = {
#include "kl_openvr_vtables.h"
// Private SteamVR interfaces, not in openvr.h: slot order reversed from
// libvr.so's call sites (see the implementations below).
KL_OVR_IFACE("IVRMailbox_001", "IVRMailbox (private)",
             "RegisterMailbox", "UndoRegisterMailbox", "SendMessage", "ReadMessage")
};
#undef KL_OVR_IFACE

typedef struct { void **vt; const char *ver; } vobj;
typedef struct { const char *ver, *method; void *fn; } impl;
static vobj *iface(const char *ver);

// ---- pose math -------------------------------------------------------------------------------
static void quat_to_m34(const float q[4], const float p[3], HmdMatrix34_t *o) {
    float x = q[0], y = q[1], z = q[2], w = q[3];
    o->m[0][0] = 1 - 2 * (y * y + z * z); o->m[0][1] = 2 * (x * y - z * w); o->m[0][2] = 2 * (x * z + y * w);
    o->m[1][0] = 2 * (x * y + z * w); o->m[1][1] = 1 - 2 * (x * x + z * z); o->m[1][2] = 2 * (y * z - x * w);
    o->m[2][0] = 2 * (x * z - y * w); o->m[2][1] = 2 * (y * z + x * w); o->m[2][2] = 1 - 2 * (x * x + y * y);
    o->m[0][3] = p[0]; o->m[1][3] = p[1]; o->m[2][3] = p[2];
}
static void quat_conj_mul(const float a[4], const float b[4], float o[4]) {   // conj(a) * b
    float ax = -a[0], ay = -a[1], az = -a[2], aw = a[3];
    o[0] = aw * b[0] + ax * b[3] + ay * b[2] - az * b[1];
    o[1] = aw * b[1] - ax * b[2] + ay * b[3] + az * b[0];
    o[2] = aw * b[2] + ax * b[1] - ay * b[0] + az * b[3];
    o[3] = aw * b[3] - ax * b[0] - ay * b[1] - az * b[2];
}
static void quat_rotate_inv(const float q[4], const float v[3], float o[3]) {  // conj(q) v q
    float c[4] = { -q[0], -q[1], -q[2], q[3] };
    float vx = v[0], vy = v[1], vz = v[2];
    float tx = 2 * (c[1] * vz - c[2] * vy), ty = 2 * (c[2] * vx - c[0] * vz), tz = 2 * (c[0] * vy - c[1] * vx);
    o[0] = vx + c[3] * tx + (c[1] * tz - c[2] * ty);
    o[1] = vy + c[3] * ty + (c[2] * tx - c[0] * tz);
    o[2] = vz + c[3] * tz + (c[0] * ty - c[1] * tx);
}

static int g_origin = 1;                 // TrackingUniverseStanding

// Device pose in the guest's tracking universe. Seated is the standing frame
// lowered by the eye height kl_ovrp's floor origin adds.
static void device_pose(int dev, TrackedDevicePose_t *o) {
    memset(o, 0, sizeof *o);
    float p[3], q[4], v[3] = {0}, a[3] = {0};
    int known = 1;
    if (dev == DEV_HMD) {
        kl_ovrp_get_guest_head_pose(&p[0], &p[1], &p[2], &q[0], &q[1], &q[2], &q[3]);
        kl_ovrp_head_motion(v, a);
    } else if (dev == DEV_LEFT || dev == DEV_RIGHT) {
        known = kl_ovrp_hand_motion(dev == DEV_LEFT ? 0 : 1, p, q, v, a);
        known = 1;                        // the parked default still tracks the head
    } else {
        return;
    }
    if (g_origin == 0) p[1] -= kl_ovrp_eye_height();
    quat_to_m34(q, p, &o->mDeviceToAbsoluteTracking);
    memcpy(o->vVelocity, v, sizeof v);
    memcpy(o->vAngularVelocity, a, sizeof a);
    o->eTrackingResult = 200;
    o->bPoseIsValid = (uint8_t)known;
    o->bDeviceIsConnected = 1;
}

// ---- unimplemented slots ------------------------------------------------------------------------
static long ovr_unimpl(const char *name) {
    static struct { const char *n; unsigned c; } seen[512];
    static unsigned ns;
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&mu);
    unsigned i = 0;
    for (; i < ns; i++) if (seen[i].n == name) break;
    if (i == ns && ns < 512) { seen[ns].n = name; seen[ns].c = 0; ns++; }
    unsigned c = i < 512 ? seen[i].c++ : 99;
    pthread_mutex_unlock(&mu);
    if (c < 3) fprintf(stderr, "  [openvr] %s: not implemented -> 0\n", name);
    return 0;
}

// ---- IVRSystem ------------------------------------------------------------------------------------
// KL_OPENVR_RT_SCALE scales the recommended render target size, and with it
// every fidelity level Alyx derives from it (its levels are 0.78x..1.5x of this
// size) and our stage images. 1 by default; 0.7 halves the pixels.
static void eye_size(uint32_t *w, uint32_t *h) {
    int ew = 0, eh = 0;
    kl_ovrp_eye_texture_size(&ew, &eh);
    if (ew <= 0 || eh <= 0) { ew = kl_env_int("KL_OPENVR_EYE_W", 1832); eh = kl_env_int("KL_OPENVR_EYE_H", 1920); }
    float sc = kl_env_float("KL_OPENVR_RT_SCALE", 1.0f);
    if (sc > 0.2f && sc < 2.0f && sc != 1.0f) {
        ew = (int)(ew * sc + 0.5f) & ~7; eh = (int)(eh * sc + 0.5f) & ~7;
    }
    *w = (uint32_t)ew; *h = (uint32_t)eh;
}
static void sys_rt_size(void *s, uint32_t *w, uint32_t *h) { (void)s; eye_size(w, h); }
static void eye_tangents(int eye, float t[4]) {      // left, right, top, bottom; positive
    float px, py, pz, qx, qy, qz, qw;
    kl_ovrp_eye_view(eye, &px, &py, &pz, &qx, &qy, &qz, &qw, t);
    if (t[0] <= 0 || t[1] <= 0 || t[2] <= 0 || t[3] <= 0) t[0] = t[1] = t[2] = t[3] = 1.0f;
}
static void sys_proj_raw(void *s, int eye, float *l, float *r, float *t, float *b) {
    (void)s;
    float tan[4];
    eye_tangents(eye, tan);
    // OpenVR's raw projection: left and TOP negative (its y points down).
    *l = -tan[0]; *r = tan[1]; *t = -tan[2]; *b = tan[3];
}
static HmdMatrix44_t sys_proj(void *s, int eye, float zn, float zf) {
    float l, r, t, b;
    sys_proj_raw(s, eye, &l, &r, &t, &b);
    HmdMatrix44_t m;
    memset(&m, 0, sizeof m);
    float idx = 1.0f / (r - l), idy = 1.0f / (b - t), sx = r + l, sy = b + t;
    m.m[0][0] = 2 * idx; m.m[0][2] = sx * idx;
    m.m[1][1] = 2 * idy; m.m[1][2] = sy * idy;
    m.m[2][2] = -zf / (zf - zn); m.m[2][3] = -zf * zn / (zf - zn);   // D3D-style, as openvr computes it
    m.m[3][2] = -1.0f;
    return m;
}
static int sys_distortion(void *s, int eye, float u, float v, float *out) {
    (void)s; (void)eye;
    for (int i = 0; i < 3; i++) { out[i * 2] = u; out[i * 2 + 1] = v; }
    return 1;
}
static HmdMatrix34_t sys_eye_to_head(void *s, int eye) {
    (void)s;
    float hp[3], hq[4], ep[3], eq[4], t[4];
    kl_ovrp_get_guest_head_pose(&hp[0], &hp[1], &hp[2], &hq[0], &hq[1], &hq[2], &hq[3]);
    kl_ovrp_eye_view(eye, &ep[0], &ep[1], &ep[2], &eq[0], &eq[1], &eq[2], &eq[3], t);
    float d[3] = { ep[0] - hp[0], ep[1] - hp[1], ep[2] - hp[2] }, lp[3], lq[4];
    quat_rotate_inv(hq, d, lp);
    quat_conj_mul(hq, eq, lq);
    if (fabsf(lp[0]) < 1e-4f && fabsf(lp[1]) < 1e-4f && fabsf(lp[2]) < 1e-4f)   // no frontend yet
        lp[0] = eye ? 0.032f : -0.032f;
    HmdMatrix34_t m;
    quat_to_m34(lq, lp, &m);
    return m;
}
static int sys_vsync(void *s, float *sec, uint64_t *frame) {
    (void)s;
    static uint64_t n;
    if (sec) *sec = 0.0f;
    if (frame) *frame = ++n;
    return 1;
}
static void sys_output_device(void *s, uint64_t *dev, int type, void *instance) {
    (void)s; (void)type;
    if (dev) *dev = (uint64_t)(uintptr_t)kl_vulkan_xr_physical_device(instance);
}
static int sys_false(void *s) { (void)s; return 0; }
static int sys_true(void *s) { (void)s; return 1; }
static void sys_poses(void *s, int origin, float predict, TrackedDevicePose_t *out, uint32_t n) {
    (void)s; (void)predict; (void)origin;
    for (uint32_t i = 0; i < n; i++) device_pose((int)i, &out[i]);
}
static HmdMatrix34_t sys_seated_to_standing(void *s) {
    (void)s;
    float q[4] = { 0, 0, 0, 1 }, p[3] = { 0, kl_ovrp_eye_height(), 0 };
    HmdMatrix34_t m;
    quat_to_m34(q, p, &m);
    return m;
}
static uint32_t sys_sorted(void *s, int cls, uint32_t *out, uint32_t cap, uint32_t rel) {
    (void)s; (void)rel;
    uint32_t n = 0;
    if (cls == 1) { if (cap > n && out) out[n] = DEV_HMD; n++; }
    if (cls == 2) {
        if (cap > n && out) out[n] = DEV_LEFT; n++;
        if (cap > n && out) out[n] = DEV_RIGHT; n++;
    }
    return n;
}
static int sys_activity(void *s, uint32_t i) { (void)s; (void)i; return 1; }       // UserInteraction
static uint32_t sys_index_for_role(void *s, int role) {
    (void)s; return role == 1 ? DEV_LEFT : role == 2 ? DEV_RIGHT : 0xFFFFFFFFu;
}
static int sys_role_for_index(void *s, uint32_t i) { (void)s; return i == DEV_LEFT ? 1 : i == DEV_RIGHT ? 2 : 0; }
static int sys_class(void *s, uint32_t i) { (void)s; return i == DEV_HMD ? 1 : i < NDEV ? 2 : 0; }
static int sys_connected(void *s, uint32_t i) { (void)s; return i < NDEV; }

// Tracked-device properties: the few a game asks to decide what it is talking
// to. Unknown ones answer TrackedProp_UnknownProperty (4), never a guess.
#define PERR_OK 0
#define PERR_WRONGTYPE 1
#define PERR_TOOSMALL 3
#define PERR_UNKNOWN 4
static const char *prop_string(uint32_t dev, int prop) {
    int left = dev == DEV_LEFT;
    switch (prop) {
    case 1000: return "oculus";                                     // TrackingSystemName
    case 1001: return dev == DEV_HMD ? "Klepton Vision Pro" :
                      left ? "Oculus Quest2 (Left Controller)" : "Oculus Quest2 (Right Controller)";
    case 1002: return dev == DEV_HMD ? "KLEPTON-HMD" : left ? "KLEPTON-L" : "KLEPTON-R";
    case 1003: return dev == DEV_HMD ? "generic_hmd" :
                      left ? "oculus_quest2_controller_left" : "oculus_quest2_controller_right";
    case 1005: return dev == DEV_HMD ? "Apple" : "Oculus";           // ManufacturerName
    case 1037: return dev == DEV_HMD ? "" : "{oculus}/input/touch_profile.json";
    case 7000: return dev == DEV_HMD ? "" : "oculus_touch";           // ControllerType
    default:   return NULL;
    }
}
static uint32_t sys_prop_string(void *s, uint32_t dev, int prop, char *buf, uint32_t n, int *err) {
    (void)s;
    const char *v = dev < NDEV ? prop_string(dev, prop) : NULL;
    if (!v) { if (err) *err = PERR_UNKNOWN; if (buf && n) buf[0] = 0; return 0; }
    uint32_t need = (uint32_t)strlen(v) + 1;
    if (buf && n >= need) { memcpy(buf, v, need); if (err) *err = PERR_OK; }
    else if (err) *err = buf ? PERR_TOOSMALL : PERR_OK;
    return need;
}
static float sys_prop_float(void *s, uint32_t dev, int prop, int *err) {
    (void)s; (void)dev;
    float v = 0; int ok = 1;
    switch (prop) {
    case 2001: v = 0.011f; break;                                      // SecondsFromVsyncToPhotons
    case 2002: v = kl_ovrp_display_frequency() > 0 ? kl_ovrp_display_frequency() : 90.0f; break;
    case 2003: v = 0.064f; break;                                      // UserIpdMeters
    case 2014: v = 1.0f; break;                                        // DeviceBatteryPercentage
    default: ok = 0;
    }
    if (err) *err = ok ? PERR_OK : PERR_UNKNOWN;
    return v;
}
static int32_t sys_prop_int(void *s, uint32_t dev, int prop, int *err) {
    (void)s;
    int32_t v = 0; int ok = 1;
    switch (prop) {
    case 1029: v = dev == DEV_HMD ? 1 : 2; break;                      // DeviceClass
    case 3007: v = dev == DEV_LEFT ? 1 : dev == DEV_RIGHT ? 2 : 0; break;   // ControllerRoleHint
    default: ok = 0;
    }
    if (err) *err = ok ? PERR_OK : PERR_UNKNOWN;
    return v;
}
static int sys_prop_bool(void *s, uint32_t dev, int prop, int *err) {
    (void)s; (void)dev; (void)prop;
    if (err) *err = PERR_UNKNOWN;
    return 0;
}
static uint64_t sys_prop_u64(void *s, uint32_t dev, int prop, int *err) {
    (void)s; (void)dev; (void)prop;
    if (err) *err = PERR_UNKNOWN;
    return 0;
}
static HmdMatrix34_t sys_prop_m34(void *s, uint32_t dev, int prop, int *err) {
    (void)s; (void)dev; (void)prop;
    HmdMatrix34_t m;
    memset(&m, 0, sizeof m);
    if (err) *err = PERR_UNKNOWN;
    return m;
}
static uint32_t sys_prop_array(void *s, uint32_t dev, int prop, uint32_t tag, void *buf, uint32_t n, int *err) {
    (void)s; (void)dev; (void)prop; (void)tag; (void)buf; (void)n;
    if (err) *err = PERR_UNKNOWN;
    return 0;
}
static const char *sys_prop_err_name(void *s, int e) { (void)s; return e ? "TrackedProp_UnknownProperty" : "TrackedProp_Success"; }
// Events: what a freshly started SteamVR session tells a scene app once —
// every device activated, the user interacting with the headset (worn), and
// nobody else holding input focus or the dashboard. VREvent_t is packed 4 on
// Linux: type, device, age, then the data union at +12.
static const struct { uint32_t type, dev; } k_start_events[] = {
    { 100, DEV_HMD }, { 100, DEV_LEFT }, { 100, DEV_RIGHT },        // TrackedDeviceActivated
    { 103, DEV_HMD },                                                // TrackedDeviceUserInteractionStarted
    { 401, 0xFFFFFFFFu },                                            // InputFocusReleased
    { 503, 0xFFFFFFFFu },                                            // DashboardDeactivated
};
static _Atomic int g_ev_next;
static int sys_poll(void *s, void *ev, uint32_t n) {
    (void)s;
    if (!ev || n < 12) return 0;
    int i = atomic_fetch_add(&g_ev_next, 1);
    if (i >= (int)(sizeof k_start_events / sizeof *k_start_events)) {
        atomic_store(&g_ev_next, 64);
        return 0;
    }
    memset(ev, 0, n);
    uint32_t *e = ev;
    e[0] = k_start_events[i].type; e[1] = k_start_events[i].dev;
    TRACE("PollNextEvent -> %u (device %d)\n", e[0], (int)e[1]);
    return 1;
}
static int sys_poll_pose(void *s, int o, void *ev, uint32_t n, void *p) { (void)o; (void)p; return sys_poll(s, ev, n); }
// The hidden-area mesh: what of the eye texture no lens shows. There is no such
// area on a Vision Pro at our frustum, but answering an EMPTY mesh is not "no
// mask" to Source 2 — with no mesh it builds a mask of its own, and builds the
// same one for both eyes: left eye's (on the canted Vision Pro frustum, the
// right ~11% of the texture plus a bottom-right block), so the RIGHT eye's
// temporal edge was masked off and left at the render target's clear colour —
// the magenta/pink stripe. One zero-area triangle is a mesh that hides nothing.
// KL_OPENVR_HIDDEN_MESH=0 restores the empty answer.
static HiddenAreaMesh_t sys_hidden(void *s, int eye, int type) {
    (void)s; (void)eye;
    static const float degenerate[6] = { 0, 0, 0, 0, 0, 0 };     // HmdVector2_t x3
    if (!kl_env_on("KL_OPENVR_HIDDEN_MESH", 1)) return (HiddenAreaMesh_t){ NULL, 0 };
    static int said;
    if (said < 6) { said++; TRACE("GetHiddenAreaMesh(eye %d, type %d) -> one degenerate triangle\n", eye, type); }
    return (HiddenAreaMesh_t){ degenerate, 1 };
}
static const char *sys_runtime_version(void *s) { (void)s; return "2.5.1"; }
static int sys_set_sdk(void *s, uint32_t maj, uint32_t min, uint32_t build) {
    (void)s;
    fprintf(stderr, "  [openvr] guest built against OpenVR %u.%u.%u\n", maj, min, build);
    return 0;                                                          // VRInitError_None
}

// ---- IVRCompositor --------------------------------------------------------------------------------
static TrackedDevicePose_t g_last[NDEV];
static float g_frame_pose[7], g_frame_tan[8];
static unsigned long long g_frames;

static void co_set_space(void *s, int o) { (void)s; g_origin = o; }
static int co_get_space(void *s) { (void)s; return g_origin; }
// ---- frame timing, for the game's auto-fidelity ---------------------------------
// Alyx scales its render resolution (fidelity levels 0..8, 0.78x..1.5x of the
// recommended size) from IVRCompositor::GetFrameTiming: frames that take longer
// than a vsync drop a level. Answering "no timing" (false) left it at its
// default level whatever the frame rate — 13 MP per eye at ~20 fps on device,
// with the compositor reprojecting 70-80 ms old frames. So the frame intervals
// are measured here: WaitGetPoses to WaitGetPoses (the client interval) and
// WaitGetPoses to the second Submit returning (Submit waits for the eye copy,
// which is queued behind the game's own GPU work on its queue, so that span is
// the frame's CPU+GPU time). ON by default; KL_OPENVR_FRAME_TIMING=0 answers
// "no timing" again. Measured on device (GPU-bound): 25-31 fps against ~20
// without, at no visible cost. On the Mac (CPU-bound) it is the other way —
// 51 fps against 62 — because the game reads the span as GPU time and its
// auto-fidelity oscillates between levels there; set it to 0 for host work.
typedef struct { uint32_t index; double t_wait, t_submit, interval_ms, work_ms; } frame_time;
#define FT_RING 32
static frame_time g_ft[FT_RING];
static uint32_t g_ft_index;
static double now_s(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}
static void ft_wait(void) {
    double t = now_s();
    frame_time *prev = &g_ft[g_ft_index % FT_RING];
    frame_time *f = &g_ft[(g_ft_index + 1) % FT_RING];
    memset(f, 0, sizeof *f);
    f->index = g_ft_index + 1;
    f->t_wait = t;
    f->interval_ms = prev->t_wait > 0 ? (t - prev->t_wait) * 1000.0 : 0.0;
    __atomic_store_n(&g_ft_index, f->index, __ATOMIC_RELEASE);
}
static void ft_submitted(void) {
    frame_time *f = &g_ft[g_ft_index % FT_RING];
    f->t_submit = now_s();
    f->work_ms = (f->t_submit - f->t_wait) * 1000.0;
    static double win_start; static unsigned win_n; static double win_work;
    if (!win_start) win_start = f->t_submit;
    win_n++; win_work += f->work_ms;
    if (f->t_submit - win_start >= 5.0) {
        fprintf(stderr, "  [openvr] guest %.1f fps, frame work %.1f ms (WaitGetPoses -> Submit), "
                        "display %.0f Hz\n", win_n / (f->t_submit - win_start), win_work / win_n,
                (double)kl_ovrp_display_frequency());
        win_start = f->t_submit; win_n = 0; win_work = 0;
    }
}
// Compositor_FrameTiming, packed 4 on Linux: six uint32, a double at +24,
// sixteen floats from +32 (PreSubmitGpu, PostSubmitGpu, TotalRenderGpu,
// CompositorRenderGpu, CompositorRenderCpu, CompositorIdleCpu,
// ClientFrameInterval, ...), the HMD pose at +96.
static int co_frame_timing(void *s, uint8_t *t, uint32_t frames_ago) {
    (void)s;
    if (!t || !kl_env_on("KL_OPENVR_FRAME_TIMING", 1)) return 0;
    uint32_t idx = __atomic_load_n(&g_ft_index, __ATOMIC_ACQUIRE);
    if (frames_ago + 1 >= FT_RING || idx <= frames_ago + 1) return 0;
    // The newest COMPLETE frame is the one before the frame in flight.
    const frame_time *f = &g_ft[(idx - 1 - frames_ago) % FT_RING];
    if (!f->t_submit) return 0;
    uint32_t size;
    memcpy(&size, t, 4);
    if (size < 96) return 0;
    memset(t + 4, 0, size - 4);
    float hz = kl_ovrp_display_frequency();
    double vsync_ms = 1000.0 / (hz > 0 ? hz : 90.0);
    uint32_t presents = 1, dropped = f->interval_ms > vsync_ms * 1.5
                                   ? (uint32_t)(f->interval_ms / vsync_ms + 0.5) - 1 : 0;
    float work = (float)f->work_ms, interval = (float)f->interval_ms;
    memcpy(t + 4, &f->index, 4);
    memcpy(t + 8, &presents, 4);
    memcpy(t + 16, &dropped, 4);
    memcpy(t + 24, &f->t_submit, 8);
    memcpy(t + 32, &work, 4);            // PreSubmitGpuMs
    memcpy(t + 40, &work, 4);            // TotalRenderGpuMs
    memcpy(t + 56, &interval, 4);        // ClientFrameIntervalMs
    if (size >= 96 + sizeof(TrackedDevicePose_t)) memcpy(t + 96, &g_last[0], sizeof(TrackedDevicePose_t));
    return 1;
}
static uint32_t co_frame_timings(void *s, uint8_t *t, uint32_t n) {
    uint32_t got = 0, size = 0;
    if (!t || !n) return 0;
    memcpy(&size, t, 4);
    for (uint32_t i = 0; i < n; i++) {
        uint8_t *e = t + (size_t)i * size;
        memcpy(e, &size, 4);
        if (!co_frame_timing(s, e, n - 1 - i)) continue;   // oldest first
        got++;
    }
    return got;
}

static int co_wait_get_poses(void *s, TrackedDevicePose_t *render, uint32_t n,
                             TrackedDevicePose_t *game, uint32_t gn) {
    (void)s;
    ft_wait();
    // The frame starts here, exactly as xrWaitFrame starts an OpenXR one: pin
    // the poses this frame will be drawn with.
    kl_ovrp_frame_latch();
    kl_ovrp_frame_begin_external();
    for (int d = 0; d < NDEV; d++) device_pose(d, &g_last[d]);
    for (uint32_t i = 0; render && i < n; i++) {
        if (i < NDEV) render[i] = g_last[i]; else memset(&render[i], 0, sizeof *render);
    }
    for (uint32_t i = 0; game && i < gn; i++) {
        if (i < NDEV) game[i] = g_last[i]; else memset(&game[i], 0, sizeof *game);
    }
    kl_ovrp_get_guest_head_pose(&g_frame_pose[0], &g_frame_pose[1], &g_frame_pose[2],
                                &g_frame_pose[3], &g_frame_pose[4], &g_frame_pose[5], &g_frame_pose[6]);
    eye_tangents(0, g_frame_tan);
    eye_tangents(1, g_frame_tan + 4);
    return 0;
}
static int co_last_poses(void *s, TrackedDevicePose_t *render, uint32_t n,
                         TrackedDevicePose_t *game, uint32_t gn) {
    (void)s;
    for (uint32_t i = 0; render && i < n; i++) { if (i < NDEV) render[i] = g_last[i]; else memset(&render[i], 0, sizeof *render); }
    for (uint32_t i = 0; game && i < gn; i++) { if (i < NDEV) game[i] = g_last[i]; else memset(&game[i], 0, sizeof *game); }
    return 0;
}
static int co_last_pose_idx(void *s, uint32_t i, TrackedDevicePose_t *r, TrackedDevicePose_t *g) {
    (void)s;
    if (i >= NDEV) return 1;
    if (r) *r = g_last[i];
    if (g) *g = g_last[i];
    return 0;
}

// The eye copy: guest image -> an MTLTexture-backed image of ours, per eye per
// stage, recorded and submitted on the guest's queue. OpenVR requires the
// submitted image to be in TRANSFER_SRC_OPTIMAL, so the copy needs no layout
// change on the guest's side.
#define OVR_STAGES 3
static unsigned long long g_frames_for_dump;
#ifdef KLOVR_VULKAN
static struct {
    VkDevice dev;
    VkQueue q;
    uint32_t qf;
    VkCommandPool pool;
    VkCommandBuffer cb[2][OVR_STAGES];
    VkFence fence[2][OVR_STAGES];
    VkFence sync;                         // the zero-copy path's "the game's frame is done"
    uint64_t dst[2][OVR_STAGES];
    uint32_t w[2], h[2], fmt[2];
    PFN_vkCreateCommandPool CreateCommandPool;
    PFN_vkAllocateCommandBuffers AllocateCommandBuffers;
    PFN_vkBeginCommandBuffer BeginCommandBuffer;
    PFN_vkEndCommandBuffer EndCommandBuffer;
    PFN_vkResetCommandBuffer ResetCommandBuffer;
    PFN_vkCmdPipelineBarrier CmdPipelineBarrier;
    PFN_vkCmdCopyImage CmdCopyImage;
    PFN_vkCmdBlitImage CmdBlitImage;
    PFN_vkCmdCopyImageToBuffer CmdCopyImageToBuffer;
    PFN_vkQueueSubmit QueueSubmit;
    PFN_vkCreateFence CreateFence;
    PFN_vkWaitForFences WaitForFences;
    PFN_vkResetFences ResetFences;
} V;

static int vk_setup(const VRVulkanTextureData_t *t) {
    if (V.dev == (VkDevice)t->m_pDevice && V.pool) return 0;
    PFN_vkGetDeviceProcAddr gdpa = (PFN_vkGetDeviceProcAddr)kl_vulkan_sym("vkGetDeviceProcAddr");
    if (!gdpa) return -1;
    memset(&V, 0, sizeof V);
    V.dev = (VkDevice)t->m_pDevice;
    V.q = (VkQueue)t->m_pQueue;
    V.qf = t->m_nQueueFamilyIndex;
#define R(f) V.f = (PFN_vk##f)gdpa(V.dev, "vk" #f)
    R(CreateCommandPool); R(AllocateCommandBuffers); R(BeginCommandBuffer); R(EndCommandBuffer);
    R(ResetCommandBuffer); R(CmdPipelineBarrier); R(CmdCopyImage); R(CmdBlitImage);
    R(QueueSubmit); R(CreateFence); R(WaitForFences); R(ResetFences); R(CmdCopyImageToBuffer);
#undef R
    VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = V.qf };
    if (!V.CreateCommandPool || V.CreateCommandPool(V.dev, &pci, NULL, &V.pool) != VK_SUCCESS) return -1;
    VkCommandBufferAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = V.pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = OVR_STAGES };
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT };
    for (int e = 0; e < 2; e++) {
        V.AllocateCommandBuffers(V.dev, &ai, V.cb[e]);
        for (int k = 0; k < OVR_STAGES; k++) V.CreateFence(V.dev, &fci, NULL, &V.fence[e][k]);
    }
    fprintf(stderr, "  [openvr] Vulkan submit path ready (device %p, queue family %u)\n",
            (void *)V.dev, V.qf);
    return 0;
}

// KL_OPENVR_DUMP=<dir>: write the guest's submitted image (the layer this eye
// reads, whole, before any scaling) and our stage image after the copy, once,
// at submitted frame KL_OPENVR_DUMP_AT (default 300), as quarter-size PPMs —
// eyeN_src.ppm / eyeN_dst.ppm. The ground truth for "is it the picture or the
// compositor": what the game drew versus what the compositor is handed.
static void vk_dump(const VRVulkanTextureData_t *t, VkImage img, uint32_t layer, uint32_t w, uint32_t h,
                    const char *name) {
    const char *dir = getenv("KL_OPENVR_DUMP");
    if (!dir || !*dir || !t->m_pPhysicalDevice) return;
    PFN_vkGetDeviceProcAddr gdpa = (PFN_vkGetDeviceProcAddr)kl_vulkan_sym("vkGetDeviceProcAddr");
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)kl_vulkan_sym("vkGetInstanceProcAddr");
    if (!gdpa || !gipa) return;
#define D(f) PFN_vk##f f = (PFN_vk##f)gdpa(V.dev, "vk" #f)
    D(CreateBuffer); D(GetBufferMemoryRequirements); D(AllocateMemory); D(BindBufferMemory);
    D(MapMemory); D(DestroyBuffer); D(FreeMemory);
#undef D
    PFN_vkGetPhysicalDeviceMemoryProperties gmp = (PFN_vkGetPhysicalDeviceMemoryProperties)
        gipa((VkInstance)t->m_pInstance, "vkGetPhysicalDeviceMemoryProperties");
    if (!CreateBuffer || !gmp) return;
    VkDeviceSize sz = (VkDeviceSize)w * h * 4;
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = sz,
                               .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT };
    VkBuffer buf; VkDeviceMemory mem; VkMemoryRequirements mr;
    if (CreateBuffer(V.dev, &bci, NULL, &buf) != VK_SUCCESS) return;
    GetBufferMemoryRequirements(V.dev, buf, &mr);
    VkPhysicalDeviceMemoryProperties mp; gmp((VkPhysicalDevice)t->m_pPhysicalDevice, &mp);
    uint32_t ti = 0;
    for (; ti < mp.memoryTypeCount; ti++)
        if ((mr.memoryTypeBits & (1u << ti)) &&
            (mp.memoryTypes[ti].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
                == (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) break;
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                                 .memoryTypeIndex = ti };
    if (ti == mp.memoryTypeCount || AllocateMemory(V.dev, &mai, NULL, &mem) != VK_SUCCESS) { DestroyBuffer(V.dev, buf, NULL); return; }
    BindBufferMemory(V.dev, buf, mem, 0);
    VkCommandBuffer cb = V.cb[0][0];
    V.WaitForFences(V.dev, 1, &V.fence[0][0], VK_TRUE, UINT64_MAX);
    V.ResetFences(V.dev, 1, &V.fence[0][0]);
    V.ResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    V.BeginCommandBuffer(cb, &bi);
    VkImageMemoryBarrier bar = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = img, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, layer, 1 } };
    // The guest's image is already TRANSFER_SRC (OpenVR's contract); ours is
    // SHADER_READ after the copy. Say so, or UNDEFINED would let it be discarded.
    bar.oldLayout = name[5] == 's' ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    V.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &bar);
    VkBufferImageCopy reg = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1 },
                              .imageExtent = { w, h, 1 } };
    V.CmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &reg);
    if (name[5] != 's') {                         // give ours back to the compositor as it was
        bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; bar.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        bar.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT; bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        V.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &bar);
    }
    V.EndCommandBuffer(cb);
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
    V.QueueSubmit(V.q, 1, &si, V.fence[0][0]);
    V.WaitForFences(V.dev, 1, &V.fence[0][0], VK_TRUE, UINT64_MAX);
    uint8_t *px = NULL;
    if (MapMemory(V.dev, mem, 0, sz, 0, (void **)&px) == VK_SUCCESS && px) {
        char path[1024];
        snprintf(path, sizeof path, "%s/%s.ppm", dir, name);
        FILE *f = fopen(path, "wb");
        if (f) {
            uint32_t ow = w / 4, oh = h / 4;
            fprintf(f, "P6\n%u %u\n255\n", ow, oh);
            for (uint32_t y = 0; y < oh; y++)
                for (uint32_t x = 0; x < ow; x++) {
                    const uint8_t *p = px + ((size_t)(y * 4) * w + x * 4) * 4;
                    fputc(p[0], f); fputc(p[1], f); fputc(p[2], f);
                }
            fclose(f);
            fprintf(stderr, "  [openvr] dumped %s (%ux%u, layer %u)\n", path, w, h, layer);
        }
    }
    FreeMemory(V.dev, mem, NULL);
    DestroyBuffer(V.dev, buf, NULL);
}

// ---- zero copy -----------------------------------------------------------------------
// The compositor samples the game's own eye image in place: the MTLTexture
// MoltenVK keeps behind the VkImage the game submitted, the eye's array layer
// as the slice. That removes, per eye per frame, a full-resolution blit
// (4608x2880 -> the stage image, on device) — and its read and its write of
// ~50 MB — which is a large share of a GPU-bound frame.
//
// Taken only when nothing would need the copy's help: full bounds (the
// compositor has no per-frame sub-rect for an external frame) and no vertical
// flip. Anything else falls back to the copy, per submit.
//
// Ordering: the game submitted its rendering before Submit, on the queue it
// handed us. One empty submission with a fence on that queue, waited on once
// per frame (after the second eye), is "the picture is complete" — the same
// point the copy's fence used to mark, minus the copy. Reuse: the game rotates
// two eye images (measured — "2 distinct"), so the image a frame was filed with
// is not rewritten until the frame after next, by which time the compositor has
// moved on to the newer one. A game that renders into ONE image would race the
// compositor; that case is detected and sends this back to the copy.
// EXPERIMENTAL, OFF by default (KL_OPENVR_ZEROCOPY=1 turns it on). Not yet
// solid: on the Mac the in-place source showed full-height vertical stripes
// (a slice/array-view or texcoord mismatch in the compositor's sampling of the
// game's 2-layer image, not found yet), and the same-image detector tripped
// after a few hundred frames although the game rotates two images.
static int vk_zero_copy_eye(int eye, int stage, const VRVulkanTextureData_t *t,
                            const VRTextureBounds_t *b, uint32_t layer) {
    static int on = -1;
    if (on < 0) on = kl_env_on("KL_OPENVR_ZEROCOPY", 0);
    if (!on) return -1;
    if (b && (b->uMin != 0.0f || b->uMax != 1.0f || b->vMin != 0.0f || b->vMax != 1.0f)) return -1;
    // One image rewritten every frame: not safe to sample in place.
    static uint64_t prev[2]; static int same[2];
    same[eye & 1] = t->m_nImage == prev[eye & 1] ? same[eye & 1] + 1 : 0;
    prev[eye & 1] = t->m_nImage;
    if (same[eye & 1] >= 2) {
        static int said;
        if (!said) { said = 1;
            fprintf(stderr, "  [openvr] the game submits the SAME eye image every frame — "
                            "zero-copy off, copying (it would race the compositor)\n"); }
        return -1;
    }
    if (vk_setup(t) != 0) return -1;
    void *mtl = kl_vulkan_xr_image_mtl(t->m_nImage);
    if (!mtl) return -1;
    kl_glfb_note_eye_mtl_texture(eye, stage, mtl, (int)layer, (int)t->m_nWidth, (int)t->m_nHeight);
    static int said;
    if (!said) { said = 1;
        fprintf(stderr, "  [openvr] zero-copy: the compositor samples the game's %ux%u eye "
                        "image in place (no per-eye blit)\n", t->m_nWidth, t->m_nHeight); }
    if (eye == 1) {                       // both eyes in: wait for the game's frame
        if (!V.sync) {
            VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
            if (V.CreateFence(V.dev, &fci, NULL, &V.sync) != VK_SUCCESS) return -1;
        }
        VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO };
        if (V.QueueSubmit(V.q, 1, &si, V.sync) != VK_SUCCESS) return -1;
        V.WaitForFences(V.dev, 1, &V.sync, VK_TRUE, UINT64_MAX);
        V.ResetFences(V.dev, 1, &V.sync);
    }
    return 0;
}

static int vk_copy_eye(int eye, int stage, const VRVulkanTextureData_t *t, const VRTextureBounds_t *b,
                       uint32_t layer) {
    if (vk_setup(t) != 0) return -1;
    int32_t x0 = 0, y0 = 0, x1 = (int32_t)t->m_nWidth, y1 = (int32_t)t->m_nHeight;
    if (b) {
        x0 = (int32_t)(b->uMin * t->m_nWidth); x1 = (int32_t)(b->uMax * t->m_nWidth);
        y0 = (int32_t)(fminf(b->vMin, b->vMax) * t->m_nHeight);
        y1 = (int32_t)(fmaxf(b->vMin, b->vMax) * t->m_nHeight);
    }
    // vMin > vMax is OpenVR's way of saying "flipped vertically" (a GL-style
    // bottom-up image); the blit flips it back by reading the rows reversed.
    int vflip = b && b->vMin > b->vMax;
    // The stage images are one fixed size — the render target size we
    // recommend — and whatever the guest submits is BLITTED to fill them. Alyx
    // renders at a dynamic resolution and alternates between textures of
    // different sizes (2688x2880 and 1728x1824 on consecutive frames), and
    // sizing the stage images to the submission re-created and re-exported
    // them every frame.
    uint32_t w, h;
    eye_size(&w, &h);
    if (!w || !h || x1 <= x0 || y1 <= y0) return -1;
    if (V.w[eye] != w || V.h[eye] != h || V.fmt[eye] != t->m_nFormat) {
        for (int k = 0; k < OVR_STAGES; k++) {
            V.dst[eye][k] = kl_vulkan_xr_image(w, h, 1, 1, (long long)t->m_nFormat, 0);
            void *mtl = kl_vulkan_xr_image_mtl(V.dst[eye][k]);
            if (mtl) kl_glfb_note_eye_mtl_texture(eye, k, mtl, 0, (int)w, (int)h);
        }
        V.w[eye] = w; V.h[eye] = h; V.fmt[eye] = t->m_nFormat;
        fprintf(stderr, "  [openvr] eye %d: %ux%u fmt %u — %d stage image(s) exported to the compositor\n",
                eye, w, h, t->m_nFormat, OVR_STAGES);
    }
    VkImage src = (VkImage)(uintptr_t)t->m_nImage, dst = (VkImage)(uintptr_t)V.dst[eye][stage];
    if (!dst) return -1;
    V.WaitForFences(V.dev, 1, &V.fence[eye][stage], VK_TRUE, UINT64_MAX);
    V.ResetFences(V.dev, 1, &V.fence[eye][stage]);
    VkCommandBuffer cb = V.cb[eye][stage];
    V.ResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    V.BeginCommandBuffer(cb, &bi);
    VkImageSubresourceRange rng = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VkImageSubresourceRange srng = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, layer, 1 };
    VkImageMemoryBarrier bar[2] = {
        { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
          .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT, .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
          .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = src, .subresourceRange = srng },
        { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = 0,
          .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
          .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = dst, .subresourceRange = rng },
    };
    V.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, NULL, 0, NULL, 2, bar);
    if (!vflip && x0 == 0 && y0 == 0 && (uint32_t)x1 == w && (uint32_t)y1 == h &&
        t->m_nWidth == w && t->m_nHeight == h) {
        VkImageCopy reg = { .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1 },
                            .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                            .extent = { w, h, 1 } };
        V.CmdCopyImage(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &reg);
    } else {
        VkImageBlit reg = { .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1 },
                            .srcOffsets = { { x0, vflip ? y1 : y0, 0 }, { x1, vflip ? y0 : y1, 1 } },
                            .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                            .dstOffsets = { { 0, 0, 0 }, { (int32_t)w, (int32_t)h, 1 } } };
        V.CmdBlitImage(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &reg, VK_FILTER_LINEAR);
    }
    VkImageMemoryBarrier done = bar[1];
    done.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    done.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    done.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    done.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    V.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         0, 0, NULL, 0, NULL, 1, &done);
    V.EndCommandBuffer(cb);
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
    V.QueueSubmit(V.q, 1, &si, V.fence[eye][stage]);
    // The compositor samples this image the moment the frame is filed, so it
    // has to be complete by then. One small copy; waiting is the honest answer.
    V.WaitForFences(V.dev, 1, &V.fence[eye][stage], VK_TRUE, UINT64_MAX);
    if (g_frames_for_dump == (unsigned long long)kl_env_int("KL_OPENVR_DUMP_AT", 300) && getenv("KL_OPENVR_DUMP")) {
        char nm[32];
        snprintf(nm, sizeof nm, "eye%d_src", eye);
        vk_dump(t, src, layer, t->m_nWidth, t->m_nHeight, nm);
        snprintf(nm, sizeof nm, "eye%d_dst", eye);
        vk_dump(t, dst, 0, w, h, nm);
    }
    return 0;
}
#endif

static unsigned g_submitted_mask;
static int g_stage;
// Submit flags that change what the texture handle points at.
#define SUBMIT_TEXTURE_WITH_POSE   0x08     // Texture_t is a VRTextureWithPose_t (same handle)
#define SUBMIT_VULKAN_ARRAY_DATA   0x40     // handle is a VRVulkanTextureArrayData_t
// VRVulkanTextureArrayData_t: the texture data plus which layer of an array
// image this eye is. Alyx renders both eyes into ONE two-layer image and
// submits it twice with this flag — layer 0 left, layer 1 right — with full
// bounds. Ignoring the flag copied layer 0 into both eyes: two identical
// pictures shown with two different eye poses, which reads as wrong depth and
// a shifted, cropped image in a headset (and is invisible in a one-eye viewer).
typedef struct { VRVulkanTextureData_t t; uint32_t m_unArrayIndex, m_unArraySize; } VRVulkanTextureArrayData_t;

static int co_submit_impl(int eye, const Texture_t *tex, const VRTextureBounds_t *b, int flags,
                          int layer_override) {
    static int seen;
    if (seen++ < 6) {
        const VRVulkanTextureData_t *vt = tex && tex->eType == 2 ? tex->handle : NULL;
        fprintf(stderr, "  [openvr] Submit(eye %d, type %d, color %d, flags %#x%s) texture %ux%u fmt %u, "
                        "bounds u %.3f..%.3f v %.3f..%.3f\n", eye,
                tex ? tex->eType : -1, tex ? (int)tex->eColorSpace : -1, flags,
                vt && (flags & SUBMIT_VULKAN_ARRAY_DATA) ? " array" : "",
                vt ? vt->m_nWidth : 0, vt ? vt->m_nHeight : 0, vt ? vt->m_nFormat : 0,
                b ? b->uMin : 0.f, b ? b->uMax : 1.f, b ? b->vMin : 0.f, b ? b->vMax : 1.f);
    }
    if (eye < 0 || eye > 1 || !tex || !tex->handle) return 101;          // InvalidTexture
    if (tex->eType != 2) {
        static int said;
        if (!said++) fprintf(stderr, "  [openvr] Submit with texture type %d — only Vulkan (2) is served\n", tex->eType);
        return 101;
    }
#ifdef KLOVR_VULKAN
    // Submit_TextureWithDepth (0x10, part of Alyx's 0x258 = TextureWithMotion):
    // the Texture_t is a VRTextureWithPoseAndDepth_t, whose depth info carries
    // the PROJECTION the picture was actually rendered with. Alyx does not render
    // exactly the frustum GetProjectionRaw describes (on device it renders
    // 4608x2880 for a 3072x2465 recommendation — a wider field), so filing the
    // frame with our tangents stretched a wider picture into a narrower frustum:
    // an image that looks rotated/shifted, and an edge of whatever lies past
    // the rendered area. The frame's tangents come from that matrix instead
    // (KL_OPENVR_SUBMIT_PROJ=0 keeps ours). Layout, pack 8: Texture_t 16, pose
    // 48 at +16, depth { handle 8, HmdMatrix44 at +72, range } at +64.
    if ((flags & 0x10) && kl_env_on("KL_OPENVR_SUBMIT_PROJ", 1)) {
        const float *m = (const float *)((const uint8_t *)tex + 72);     // row-major 4x4
        float m00 = m[0], m02 = m[2], m11 = m[5], m12 = m[6];
        if (m00 != 0.0f && m11 != 0.0f) {
            float rl = 2.0f / m00, sx = m02 * rl, bt = 2.0f / m11, sy = m12 * bt;
            float r = (sx + rl) * 0.5f, l = (sx - rl) * 0.5f, b2 = (sy + bt) * 0.5f, t2 = (sy - bt) * 0.5f;
            float tn[4] = { -l, r, -t2, b2 };                 // left, right, up, down; positive
            // OpenVR's raw y points DOWN (see sys_proj_raw); a matrix built the
            // other way round shows up as a negative "down" — flip it then.
            if (tn[2] < 0 && tn[3] < 0) { tn[2] = -tn[2]; tn[3] = -tn[3]; }
            static int said;
            if (said < 4) { said++;
                fprintf(stderr, "  [openvr] eye %d rendered with tangents l=%.4f r=%.4f u=%.4f d=%.4f "
                                "(we offered l=%.4f r=%.4f u=%.4f d=%.4f)\n", eye, tn[0], tn[1], tn[2], tn[3],
                        g_frame_tan[eye * 4 + 0], g_frame_tan[eye * 4 + 1],
                        g_frame_tan[eye * 4 + 2], g_frame_tan[eye * 4 + 3]); }
            if (tn[0] > 0 && tn[1] > 0 && tn[2] > 0 && tn[3] > 0)
                memcpy(g_frame_tan + eye * 4, tn, sizeof tn);
        }
    }
    uint32_t layer = layer_override >= 0 ? (uint32_t)layer_override : 0;
    if (layer_override < 0 && (flags & SUBMIT_VULKAN_ARRAY_DATA))
        layer = ((const VRVulkanTextureArrayData_t *)tex->handle)->m_unArrayIndex;
    {
        static int said;
        if (said < 4) { said++;
            fprintf(stderr, "  [openvr] eye %d <- array layer %u\n", eye, layer); }
        // How many distinct images the game rotates its eyes through — whether
        // the compositor could sample them in place (see the zero-copy path).
        static uint64_t seen[8]; static int nseen, reported;
        uint64_t im = ((const VRVulkanTextureData_t *)tex->handle)->m_nImage;
        int k = 0;
        while (k < nseen && seen[k] != im) k++;
        if (k == nseen && nseen < 8) seen[nseen++] = im;
        if (!reported && g_frames >= 600) {
            reported = 1;
            fprintf(stderr, "  [openvr] the game's eye images: %d distinct over 600 frames\n", nseen);
        }
    }
    if (vk_zero_copy_eye(eye, g_stage, (const VRVulkanTextureData_t *)tex->handle, b, layer) != 0 &&
        vk_copy_eye(eye, g_stage, (const VRVulkanTextureData_t *)tex->handle, b, layer) != 0) return 101;
#else
    (void)b;
    return 101;
#endif
    g_submitted_mask |= 1u << eye;
    if (g_submitted_mask == 3) {
        // Both eyes in: file the frame through the same seams an OpenXR frame
        // uses, with the pose and frusta it was drawn with.
        kl_ovrp_frame_end_external(g_stage, g_frame_pose, g_frame_tan);
        ft_submitted();
#ifdef KLOVR_VULKAN
        g_frames_for_dump++;
#endif
        kl_vulkan_frame_done(g_stage);
        if (!g_frames++ || (g_frames & (g_frames - 1)) == 0)
            fprintf(stderr, "  [openvr] frame %llu submitted (stage %d)\n", g_frames - 1, g_stage);
        g_submitted_mask = 0;
        g_stage = (g_stage + 1) % OVR_STAGES;
    }
    return 0;
}
static int co_submit(void *s, int eye, const Texture_t *tex, const VRTextureBounds_t *b, int flags) {
    (void)s;
    return co_submit_impl(eye, tex, b, flags, -1);
}
static int co_submit_array(void *s, int eye, const Texture_t *tex, uint32_t idx, const VRTextureBounds_t *b, int flags) {
    (void)s;
    return co_submit_impl(eye, tex, b, flags, (int)idx);
}
static void co_void(void *s) { (void)s; }
// Fades and the grid are the compositor's to draw, and there is no compositor
// layer for them here: both read as fully clear. Typed, because a float or an
// HmdColor_t comes back in s0-s3 and a generic stub leaves those as garbage.
typedef struct { float r, g, b, a; } HmdColor_t;
static HmdColor_t co_fade_color(void *s, int background) { (void)s; (void)background; return (HmdColor_t){ 0, 0, 0, 0 }; }
static float co_grid_alpha(void *s) { (void)s; return 0.0f; }
static void co_fade_to(void *s, float t, float r, float g, float b, float a, int bg) {
    (void)s; (void)t; (void)r; (void)g; (void)b; (void)a; (void)bg;
}
static void co_fade_grid(void *s, float t, int in) { (void)s; (void)t; (void)in; }
static void co_suspend(void *s, int on) {
    (void)s;
    TRACE("SuspendRendering(%d)\n", on);
}
static int co_false(void *s) { (void)s; return 0; }
static int co_true(void *s) { (void)s; return 1; }
static float co_time_remaining(void *s) { (void)s; return 0.005f; }
static uint32_t co_vk_inst_ext(void *s, char *buf, uint32_t n) { (void)s; if (buf && n) buf[0] = 0; return 1; }
static uint32_t co_vk_dev_ext(void *s, void *pd, char *buf, uint32_t n) { (void)s; (void)pd; if (buf && n) buf[0] = 0; return 1; }
// The scene focus belongs to us: libvr compares this with its own pid, and 0
// (no scene app) reads as "another app has focus".
static uint32_t co_focus_process(void *s) { (void)s; return (uint32_t)getpid(); }

// ---- IVRInput ------------------------------------------------------------------------------------
//
// Alyx's action set is fixed and its names are in libclient (/actions/default,
// /interact, /move, /weapon, /dev, /scroll). There is no action manifest to read
// here — the binaries depot does not carry hlvr/cfg/actions.json, and SteamVR's
// binding UI does not exist — so each action is bound by NAME to the Touch-style
// inputs kl_ovrp holds, in the spirit of Alyx's default Oculus Touch bindings:
//
//   trigger           Use, Fire, TriggerPull, ArmGrenade, MenuInteract
//   grip              UseGrip, GGLock (gravity gloves), HandCurl, SqueezeXenGrenade
//   A / X (lower)     EjectClip, ShowInventory... (on the hand that asks)
//   B / Y (upper)     OpenChamber / ReleaseSlide, ToggleBurstFire, MenuDismiss
//   left stick        Walk (vector2), MoveBack, Scroll
//   right stick       Teleport (up), TurnLeft/TurnRight (flick), ContinuousTurn
//   menu (Start)      ToggleMenu
//   poses             HandPoseLeft/Right — the controller pose
//
// A handle carries what it was bound to, so the per-frame reads are a switch.
enum { SRC_NONE, SRC_TRIGGER, SRC_GRIP, SRC_LOWER, SRC_UPPER, SRC_STICK, SRC_STICK_Y,
       SRC_STICK_X, SRC_STICK_UP, SRC_STICK_DOWN, SRC_STICK_LEFT, SRC_STICK_RIGHT,
       SRC_STICK_CLICK, SRC_MENU, SRC_POSE, SRC_SKELETON, SRC_HAPTIC };
typedef struct { const char *suffix; int src; int hand; } bind;     // hand: -1 either, 0 L, 1 R
static const bind k_bind[] = {
    { "/in/TriggerPull", SRC_TRIGGER, -1 }, { "/in/Use", SRC_TRIGGER, -1 },
    { "/in/Fire", SRC_TRIGGER, -1 }, { "/in/ArmGrenade", SRC_TRIGGER, -1 },
    { "/in/ArmXenGrenade", SRC_TRIGGER, -1 }, { "/in/MenuInteract", SRC_TRIGGER, -1 },
    { "/in/AltFire", SRC_TRIGGER, -1 }, { "/in/HealthPen", SRC_TRIGGER, -1 },
    { "/in/UseGrip", SRC_GRIP, -1 }, { "/in/GGLock", SRC_GRIP, -1 }, { "/in/HandCurl", SRC_GRIP, -1 },
    { "/in/SqueezeXenGrenade", SRC_GRIP, -1 },
    { "/in/EjectClip", SRC_LOWER, -1 }, { "/in/ShowInventory", SRC_LOWER, -1 },
    { "/in/Reload", SRC_LOWER, -1 },
    { "/in/OpenChamber", SRC_UPPER, -1 }, { "/in/ReleaseSlide", SRC_UPPER, -1 },
    { "/in/ToggleBurstFire", SRC_UPPER, -1 }, { "/in/ToggleLaserSight", SRC_UPPER, -1 },
    { "/in/MenuDismiss", SRC_UPPER, -1 },
    { "/in/Walk", SRC_STICK, 0 }, { "/in/WalkMantle", SRC_STICK, 0 }, { "/in/MoveBack", SRC_STICK_DOWN, 0 },
    { "/in/Scroll", SRC_STICK_Y, -1 }, { "/in/ScrollTouchPad", SRC_STICK_Y, -1 },
    { "/in/Teleport", SRC_STICK_UP, 1 }, { "/in/TeleportTurn", SRC_STICK, 1 },
    { "/in/TurnLeft", SRC_STICK_LEFT, 1 }, { "/in/TurnRight", SRC_STICK_RIGHT, 1 },
    { "/in/ContinuousTurn", SRC_STICK_X, 1 }, { "/in/AdjustHeight", SRC_STICK_Y, 1 },
    { "/in/MoveCrouchToggle", SRC_STICK_DOWN, 1 }, { "/in/MoveStandToggle", SRC_STICK_UP, 0 },
    { "/in/ShiftDashJump", SRC_STICK_CLICK, 0 },
    { "/in/ToggleMenu", SRC_MENU, 0 },
    { "/in/HandPoseLeft", SRC_POSE, 0 }, { "/in/HandPoseRight", SRC_POSE, 1 },
    { "/in/HandSkeletonLeft", SRC_SKELETON, 0 }, { "/in/HandSkeletonRight", SRC_SKELETON, 1 },
    { "/in/HapticVibration", SRC_HAPTIC, -1 },
};

#define MAX_ACTIONS 256
// prev/cur/seen per restriction (any, left, right): bChanged is "since the
// previous UpdateActionState", not "since the previous read", so the state a
// read compares against is the one latched at the last update. Per-read made a
// press read as changed on every call once the guest asked for both hands.
typedef struct { char name[128]; int src, hand; int prev[3], cur[3]; unsigned seen[3]; int polled; } action;
static unsigned g_update_n;
static action g_actions[MAX_ACTIONS];
static int g_nactions;
static char g_sets[32][96];
static int g_nsets;
static pthread_mutex_t g_imu = PTHREAD_MUTEX_INITIALIZER;
static float g_input_time;

#define H_SOURCE_LEFT  1
#define H_SOURCE_RIGHT 2
#define H_SOURCE_HEAD  3

static int in_manifest(void *s, const char *p) {
    (void)s;
    fprintf(stderr, "  [openvr] action manifest %s — bindings are the built-in Touch table\n", p ? p : "(null)");
    return 0;
}
static int in_set_handle(void *s, const char *name, uint64_t *h) {
    (void)s;
    pthread_mutex_lock(&g_imu);
    int i = 0;
    for (; i < g_nsets; i++) if (!strcasecmp(g_sets[i], name)) break;
    if (i == g_nsets && g_nsets < 32) snprintf(g_sets[g_nsets++], sizeof g_sets[0], "%s", name);
    pthread_mutex_unlock(&g_imu);
    *h = 0x1000 + (uint64_t)i;
    return 0;
}
static int in_action_handle(void *s, const char *name, uint64_t *h) {
    (void)s;
    pthread_mutex_lock(&g_imu);
    int i = 0;
    for (; i < g_nactions; i++) if (!strcasecmp(g_actions[i].name, name)) break;
    if (i == g_nactions && g_nactions < MAX_ACTIONS) {
        action *a = &g_actions[g_nactions++];
        snprintf(a->name, sizeof a->name, "%s", name);
        a->src = SRC_NONE; a->hand = -1;
        size_t nl = strlen(name);
        for (size_t k = 0; k < sizeof k_bind / sizeof *k_bind; k++) {
            size_t sl = strlen(k_bind[k].suffix);
            if (nl >= sl && !strcasecmp(name + nl - sl, k_bind[k].suffix)) {
                a->src = k_bind[k].src; a->hand = k_bind[k].hand; break;
            }
        }
        TRACE("action %s -> source %d, hand %d\n", name, a->src, a->hand);
        if (a->src == SRC_NONE) fprintf(stderr, "  [openvr] action %s has no binding (reads inactive)\n", name);
    }
    pthread_mutex_unlock(&g_imu);
    *h = 0x2000 + (uint64_t)i;
    return 0;
}
static int in_source_handle(void *s, const char *path, uint64_t *h) {
    (void)s;
    *h = !strcmp(path, "/user/hand/left") ? H_SOURCE_LEFT : !strcmp(path, "/user/hand/right") ? H_SOURCE_RIGHT
       : !strcmp(path, "/user/head") ? H_SOURCE_HEAD : 0x100 + (uint64_t)(strlen(path) & 0xff);
    return 0;
}
static int in_update(void *s, const VRActiveActionSet_t *sets, uint32_t size, uint32_t n) {
    (void)s; (void)sets; (void)size; (void)n;
    g_update_n++;
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    g_input_time = (float)t.tv_sec + (float)t.tv_nsec * 1e-9f;
    return 0;
}
static action *act_of(uint64_t h) {
    uint64_t i = h - 0x2000;
    return (h >= 0x2000 && i < (uint64_t)g_nactions) ? &g_actions[i] : NULL;
}
// One hand's value for a source: digital (on) and analog (x, y).
static void read_src(int src, int hand, int *on, float *x, float *y) {
    uint32_t b = 0, t = 0; float trig = 0, grip = 0, sx = 0, sy = 0;
    kl_ovrp_controller_input(hand, &b, &t, &trig, &grip, &sx, &sy);
    *on = 0; *x = 0; *y = 0;
    int right = hand == 1;
    switch (src) {
    case SRC_TRIGGER: *x = trig; *on = trig > 0.5f; break;
    case SRC_GRIP:    *x = grip; *on = grip > 0.5f; break;
    case SRC_LOWER:   *on = (b & (right ? KL_OVRP_RAW_A : KL_OVRP_RAW_X)) != 0; *x = (float)*on; break;
    case SRC_UPPER:   *on = (b & (right ? KL_OVRP_RAW_B : KL_OVRP_RAW_Y)) != 0; *x = (float)*on; break;
    case SRC_STICK:   *x = sx; *y = sy; *on = fabsf(sx) > 0.3f || fabsf(sy) > 0.3f; break;
    case SRC_STICK_X: *x = sx; *on = fabsf(sx) > 0.5f; break;
    case SRC_STICK_Y: *x = sy; *y = sy; *on = fabsf(sy) > 0.5f; break;
    case SRC_STICK_UP:    *on = sy > 0.7f; *x = (float)*on; break;
    case SRC_STICK_DOWN:  *on = sy < -0.7f; *x = (float)*on; break;
    case SRC_STICK_LEFT:  *on = sx < -0.7f; *x = (float)*on; break;
    case SRC_STICK_RIGHT: *on = sx > 0.7f; *x = (float)*on; break;
    case SRC_STICK_CLICK: *on = (b & (right ? KL_OVRP_RAW_RTHUMBSTICK : KL_OVRP_RAW_LTHUMBSTICK)) != 0; *x = (float)*on; break;
    case SRC_MENU:    *on = (b & KL_OVRP_RAW_START) != 0; *x = (float)*on; break;
    default: break;
    }
}
// IVRSystem::GetControllerState — the legacy input path, which Alyx still
// polls (outside its action sets, e.g. for "Press Trigger To Start" while a
// level loads). VRControllerState_t: packet number, pressed and touched masks
// by EVRButtonId, and five axes; axis 0 the stick, axis 1 the trigger, axis 2
// the grip, which is where a Touch controller's driver reports them.
typedef struct { float x, y; } VRControllerAxis_t;
typedef struct __attribute__((packed, aligned(4))) {
    uint32_t unPacketNum; uint64_t ulButtonPressed, ulButtonTouched; VRControllerAxis_t rAxis[5];
} VRControllerState_t;
static int sys_controller_state(void *s, uint32_t dev, VRControllerState_t *st, uint32_t size) {
    (void)s;
    if (!st || size < sizeof *st || (dev != DEV_LEFT && dev != DEV_RIGHT)) return 0;
    int hand = dev == DEV_RIGHT;
    uint32_t b = 0, t = 0; float trig = 0, grip = 0, sx = 0, sy = 0;
    if (!kl_ovrp_controller_input(hand, &b, &t, &trig, &grip, &sx, &sy)) return 0;
    static uint32_t packet;
    memset(st, 0, sizeof *st);
    st->unPacketNum = ++packet;
    uint64_t p = 0;
    if (b & KL_OVRP_RAW_START) p |= 1ull << 1;                                  // ApplicationMenu
    if (grip > 0.5f) p |= 1ull << 2 | 1ull << 34;                              // Grip, Axis2
    if (b & (hand ? KL_OVRP_RAW_A : KL_OVRP_RAW_X)) p |= 1ull << 7;             // A
    if (b & (hand ? KL_OVRP_RAW_B : KL_OVRP_RAW_Y)) p |= 1ull << 1;             // B/Y as the menu button
    if (b & (hand ? KL_OVRP_RAW_RTHUMBSTICK : KL_OVRP_RAW_LTHUMBSTICK)) p |= 1ull << 32;
    if (trig > 0.5f) p |= 1ull << 33;                                          // Axis1 = trigger
    st->ulButtonPressed = p;
    st->ulButtonTouched = p;
    st->rAxis[0] = (VRControllerAxis_t){ sx, sy };
    st->rAxis[1].x = trig;
    st->rAxis[2].x = grip;
    return 1;
}

// Which hands an action reads: the restriction the guest asked for, else the
// binding's own hand, else both (OR / larger magnitude).
static int hands_for(const action *a, uint64_t restrict_to, int out[2]) {
    if (restrict_to == H_SOURCE_LEFT) { out[0] = 0; return 1; }
    if (restrict_to == H_SOURCE_RIGHT) { out[0] = 1; return 1; }
    if (a->hand >= 0) { out[0] = a->hand; return 1; }
    out[0] = 0; out[1] = 1; return 2;
}
static int in_digital(void *s, uint64_t h, uint8_t *d, uint32_t size, uint64_t restrict_to) {
    (void)s;
    if (!d || size < 24) return 4;
    memset(d, 0, 24);
    action *a = act_of(h);
    if (!a) return 3;
    if (a->src == SRC_NONE || a->src == SRC_POSE || a->src == SRC_SKELETON || a->src == SRC_HAPTIC) return 0;
    int hs[2], n = hands_for(a, restrict_to, hs), on = 0, src_hand = hs[0];
    for (int i = 0; i < n; i++) {
        int o; float x, y;
        read_src(a->src, hs[i], &o, &x, &y);
        if (o && !on) { on = 1; src_hand = hs[i]; }
    }
    int slot = restrict_to == H_SOURCE_RIGHT ? 2 : restrict_to == H_SOURCE_LEFT ? 1 : 0;
    d[0] = 1;                                                         // bActive
    uint64_t origin = src_hand ? H_SOURCE_RIGHT : H_SOURCE_LEFT;
    memcpy(d + 8, &origin, 8);
    d[16] = (uint8_t)on;
    if (a->seen[slot] != g_update_n) {               // first read since an update
        a->prev[slot] = a->cur[slot];
        a->seen[slot] = g_update_n;
    }
    a->cur[slot] = on;
    d[17] = (uint8_t)(on != a->prev[slot]);
    float ut = 0; memcpy(d + 20, &ut, 4);
    if (!a->polled || (d[17] && kl_env_on("KL_TRACE_OPENVR", 0))) {
        a->polled = 1;
        TRACE("digital %s%s: state %d changed %d\n", a->name,
              restrict_to == H_SOURCE_LEFT ? " (left)" : restrict_to == H_SOURCE_RIGHT ? " (right)" : "",
              d[16], d[17]);
    }
    return 0;
}
static int in_analog(void *s, uint64_t h, uint8_t *d, uint32_t size, uint64_t restrict_to) {
    (void)s;
    if (!d || size < 48) return 4;
    memset(d, 0, 48);
    action *a = act_of(h);
    if (!a) return 3;
    if (a->src == SRC_NONE || a->src == SRC_POSE || a->src == SRC_SKELETON || a->src == SRC_HAPTIC) return 0;
    int hs[2], n = hands_for(a, restrict_to, hs), src_hand = hs[0];
    float bx = 0, by = 0;
    for (int i = 0; i < n; i++) {
        int o; float x, y;
        read_src(a->src, hs[i], &o, &x, &y);
        if (fabsf(x) + fabsf(y) > fabsf(bx) + fabsf(by)) { bx = x; by = y; src_hand = hs[i]; }
    }
    d[0] = 1;
    uint64_t origin = src_hand ? H_SOURCE_RIGHT : H_SOURCE_LEFT;
    memcpy(d + 8, &origin, 8);
    memcpy(d + 16, &bx, 4); memcpy(d + 20, &by, 4);
    return 0;
}
static int pose_data(uint64_t h, int origin, uint8_t *d, uint32_t size, uint64_t restrict_to) {
    if (!d || size < 96) return 4;
    memset(d, 0, 96);
    action *a = act_of(h);
    if (!a) return 3;
    int hand = restrict_to == H_SOURCE_LEFT ? 0 : restrict_to == H_SOURCE_RIGHT ? 1 : a->hand;
    if (a->src != SRC_POSE || hand < 0) return 0;
    int saved = g_origin;
    g_origin = origin;
    TrackedDevicePose_t p;
    device_pose(hand ? DEV_RIGHT : DEV_LEFT, &p);
    g_origin = saved;
    d[0] = 1;
    uint64_t o = hand ? H_SOURCE_RIGHT : H_SOURCE_LEFT;
    memcpy(d + 8, &o, 8);
    memcpy(d + 16, &p, sizeof p);
    return 0;
}
static int in_pose_now(void *s, uint64_t h, int origin, float secs, uint8_t *d, uint32_t size, uint64_t r) {
    (void)s; (void)secs; return pose_data(h, origin, d, size, r);
}
static int in_pose_next(void *s, uint64_t h, int origin, uint8_t *d, uint32_t size, uint64_t r) {
    (void)s; return pose_data(h, origin, d, size, r);
}
// Skeletal input: no bone data — reported as an active action with no skeleton
// (the summary below carries the curls), which games treat as a controller
// without finger tracking.
static int in_skeletal(void *s, uint64_t h, uint8_t *d, uint32_t size) {
    (void)s; (void)h;
    if (!d || size < 16) return 4;
    memset(d, 0, 16);
    return 0;
}
static int in_dominant(void *s, int *role) { (void)s; if (role) *role = 2; return 0; }
static int in_no_data(void *s) { (void)s; return 13; }                 // VRInputError_NoData
static int in_tracking_level(void *s, uint64_t h, int *lvl) { (void)s; (void)h; if (lvl) *lvl = 0; return 0; }
static int in_summary(void *s, uint64_t h, int type, float *out) {
    (void)s; (void)type;
    action *a = act_of(h);
    if (!out) return 4;
    memset(out, 0, 9 * sizeof(float));
    if (!a || a->hand < 0) return 0;
    uint32_t b, t; float trig, grip, sx, sy;
    kl_ovrp_controller_input(a->hand, &b, &t, &trig, &grip, &sx, &sy);
    out[0] = (t & (a->hand ? KL_OVRP_RAW_A | KL_OVRP_RAW_B : KL_OVRP_RAW_X | KL_OVRP_RAW_Y)) ? 0.5f : 0.0f; // thumb
    out[1] = trig;                                                       // index
    out[2] = out[3] = out[4] = grip;                                     // middle, ring, pinky
    return 0;
}
static int in_haptic(void *s, uint64_t h, float start, float dur, float freq, float amp, uint64_t restrict_to) {
    (void)s; (void)h; (void)start; (void)freq;
    int hand = restrict_to == H_SOURCE_LEFT ? 0 : restrict_to == H_SOURCE_RIGHT ? 1 : -1;
    if (hand < 0) { kl_ovrp_haptics_apply(0, amp, dur); hand = 1; }
    kl_ovrp_haptics_apply(hand, amp, dur);
    return 0;
}
static int in_false(void *s) { (void)s; return 0; }

// ---- IVRChaperone -------------------------------------------------------------------------------
static int ch_state(void *s) { (void)s; return 1; }                     // ChaperoneCalibrationState_OK
static int ch_size(void *s, float *x, float *z) { (void)s; if (x) *x = 2; if (z) *z = 2; return 1; }
static int ch_rect(void *s, float *q) {
    (void)s;
    static const float r[12] = { -1, 0, -1,  1, 0, -1,  1, 0, 1,  -1, 0, 1 };
    if (q) memcpy(q, r, sizeof r);
    return 1;
}

// ---- IVRSettings: every key reads as unset ---------------------------------------------------
static int st_get_bool(void *s, const char *sec, const char *k, int *err) { (void)s; (void)sec; (void)k; if (err) *err = 2; return 0; }
static float st_get_float(void *s, const char *sec, const char *k, int *err) { (void)s; (void)sec; (void)k; if (err) *err = 2; return 0; }
static void st_get_string(void *s, const char *sec, const char *k, char *buf, uint32_t n, int *err) {
    (void)s; (void)sec; (void)k; if (buf && n) buf[0] = 0; if (err) *err = 2;
}

// ---- IVRMailbox: SteamVR's message bus, private ------------------------------------------------
// The layout, from libvr.so (CVr owns the object at +0x800):
//   #0 RegisterMailbox(name, &handle) -> error     (0x95b38, the handle at +0x250)
//   #1 UndoRegisterMailbox(handle)                  (0xb610c)
//   #2 SendMessage(handle, address, json) -> error  (0x98be8)
//   #3 ReadMessage(handle, buf, size, &needed) -> error: 0 a message, 2 the
//      buffer is short (resized to needed and read again), anything else none
//      (0xb579c, polled every frame)
// Messages are JSON dispatched on "type". Half-Life: Alyx registers
// "hlvr/mailbox", waits for {"type":"ready"} from the SteamVR web helper that
// hosts its VR menu ("Mongoose"), and 15 s without one is a message box and
// exit ("Timed out waiting for response from Mongoose"). There is no web
// helper here, so every mailbox is told "ready" once, on its first read; what
// the game sends back is logged and dropped.
#define MBOX_MAX 8
static struct { char name[96]; int told; } g_mbox[MBOX_MAX];
static int g_nmbox;
static pthread_mutex_t g_mbmu = PTHREAD_MUTEX_INITIALIZER;
static int mb_register(void *s, const char *name, uint64_t *h) {
    (void)s;
    pthread_mutex_lock(&g_mbmu);
    int i = g_nmbox < MBOX_MAX ? g_nmbox++ : -1;
    if (i >= 0) { snprintf(g_mbox[i].name, sizeof g_mbox[i].name, "%s", name ? name : ""); g_mbox[i].told = 0; }
    pthread_mutex_unlock(&g_mbmu);
    fprintf(stderr, "  [openvr] IVRMailbox::RegisterMailbox(\"%s\") -> %d\n", name ? name : "", i + 1);
    if (i < 0) return 1;
    if (h) *h = (uint64_t)i + 1;
    return 0;
}
static int mb_unregister(void *s, uint64_t h) { (void)s; (void)h; return 0; }
static int mb_send(void *s, uint64_t h, const char *addr, const char *msg) {
    (void)s;
    static int n;
    if (n++ < 16)
        fprintf(stderr, "  [openvr] IVRMailbox::SendMessage(#%llu -> \"%s\"): %.200s\n",
                (unsigned long long)h, addr ? addr : "", msg ? msg : "");
    return 0;
}
static int mb_read(void *s, uint64_t h, char *buf, uint32_t size, uint32_t *needed) {
    (void)s;
    static const char ready[] = "{\"type\":\"ready\"}";
    if (h < 1 || h > MBOX_MAX) return 1;
    pthread_mutex_lock(&g_mbmu);
    int told = g_mbox[h - 1].told;
    pthread_mutex_unlock(&g_mbmu);
    if (told) return 1;
    if (needed) *needed = sizeof ready;
    if (!buf || size < sizeof ready) return 2;
    memcpy(buf, ready, sizeof ready);
    pthread_mutex_lock(&g_mbmu);
    g_mbox[h - 1].told = 1;
    pthread_mutex_unlock(&g_mbmu);
    fprintf(stderr, "  [openvr] IVRMailbox: told \"%s\" %s (no SteamVR web helper here)\n",
            g_mbox[h - 1].name, ready);
    return 0;
}

// ---- implemented slots ----------------------------------------------------------------------------
#define SYS "IVRSystem_026"
#define COMP "IVRCompositor_029"
#define INP "IVRInput_011"
#define CHAP "IVRChaperone_004"
#define SETT "IVRSettings_003"
static const impl g_impl[] = {
    { "IVRMailbox_001", "RegisterMailbox", (void *)mb_register },
    { "IVRMailbox_001", "UndoRegisterMailbox", (void *)mb_unregister },
    { "IVRMailbox_001", "SendMessage", (void *)mb_send },
    { "IVRMailbox_001", "ReadMessage", (void *)mb_read },
    { SYS, "GetRecommendedRenderTargetSize", (void *)sys_rt_size },
    { SYS, "GetProjectionMatrix", (void *)sys_proj }, { SYS, "GetProjectionRaw", (void *)sys_proj_raw },
    { SYS, "ComputeDistortion", (void *)sys_distortion },
    { SYS, "GetEyeToHeadTransform", (void *)sys_eye_to_head },
    { SYS, "GetTimeSinceLastVsync", (void *)sys_vsync },
    { SYS, "GetOutputDevice", (void *)sys_output_device },
    { SYS, "IsDisplayOnDesktop", (void *)sys_false },
    { SYS, "GetDeviceToAbsoluteTrackingPose", (void *)sys_poses },
    { SYS, "GetSeatedZeroPoseToStandingAbsoluteTrackingPose", (void *)sys_seated_to_standing },
    { SYS, "GetSortedTrackedDeviceIndicesOfClass", (void *)sys_sorted },
    { SYS, "GetTrackedDeviceActivityLevel", (void *)sys_activity },
    { SYS, "GetTrackedDeviceIndexForControllerRole", (void *)sys_index_for_role },
    { SYS, "GetControllerRoleForTrackedDeviceIndex", (void *)sys_role_for_index },
    { SYS, "GetTrackedDeviceClass", (void *)sys_class }, { SYS, "IsTrackedDeviceConnected", (void *)sys_connected },
    { SYS, "GetBoolTrackedDeviceProperty", (void *)sys_prop_bool },
    { SYS, "GetFloatTrackedDeviceProperty", (void *)sys_prop_float },
    { SYS, "GetInt32TrackedDeviceProperty", (void *)sys_prop_int },
    { SYS, "GetUint64TrackedDeviceProperty", (void *)sys_prop_u64 },
    { SYS, "GetMatrix34TrackedDeviceProperty", (void *)sys_prop_m34 },
    { SYS, "GetArrayTrackedDeviceProperty", (void *)sys_prop_array },
    { SYS, "GetStringTrackedDeviceProperty", (void *)sys_prop_string },
    { SYS, "GetPropErrorNameFromEnum", (void *)sys_prop_err_name },
    { SYS, "GetControllerState", (void *)sys_controller_state },
    { SYS, "PollNextEvent", (void *)sys_poll }, { SYS, "PollNextEventWithPose", (void *)sys_poll_pose },
    { SYS, "GetHiddenAreaMesh", (void *)sys_hidden },
    { SYS, "IsInputAvailable", (void *)sys_true }, { SYS, "IsSteamVRDrawingControllers", (void *)sys_false },
    { SYS, "ShouldApplicationPause", (void *)sys_false },
    { SYS, "ShouldApplicationReduceRenderingWork", (void *)sys_false },
    { SYS, "GetRuntimeVersion", (void *)sys_runtime_version }, { SYS, "SetSDKVersion", (void *)sys_set_sdk },

    { COMP, "SetTrackingSpace", (void *)co_set_space }, { COMP, "GetTrackingSpace", (void *)co_get_space },
    { COMP, "WaitGetPoses", (void *)co_wait_get_poses }, { COMP, "GetLastPoses", (void *)co_last_poses },
    { COMP, "GetLastPoseForTrackedDeviceIndex", (void *)co_last_pose_idx },
    { COMP, "Submit", (void *)co_submit }, { COMP, "SubmitWithArrayIndex", (void *)co_submit_array },
    { COMP, "FadeToColor", (void *)co_fade_to }, { COMP, "GetCurrentFadeColor", (void *)co_fade_color },
    { COMP, "FadeGrid", (void *)co_fade_grid }, { COMP, "GetCurrentGridAlpha", (void *)co_grid_alpha },
    { COMP, "SuspendRendering", (void *)co_suspend },
    { COMP, "ClearLastSubmittedFrame", (void *)co_void }, { COMP, "PostPresentHandoff", (void *)co_void },
    { COMP, "GetFrameTiming", (void *)co_frame_timing },
    { COMP, "GetFrameTimings", (void *)co_frame_timings }, { COMP, "GetFrameTimeRemaining", (void *)co_time_remaining },
    { COMP, "IsFullscreen", (void *)co_true }, { COMP, "CanRenderScene", (void *)co_true },
    { COMP, "GetCurrentSceneFocusProcess", (void *)co_focus_process },
    { COMP, "GetLastFrameRenderer", (void *)co_focus_process },
    { COMP, "IsMirrorWindowVisible", (void *)co_false },
    { COMP, "ShouldAppRenderWithLowResources", (void *)co_false },
    { COMP, "GetVulkanInstanceExtensionsRequired", (void *)co_vk_inst_ext },
    { COMP, "GetVulkanDeviceExtensionsRequired", (void *)co_vk_dev_ext },
    { COMP, "IsMotionSmoothingEnabled", (void *)co_false }, { COMP, "IsMotionSmoothingSupported", (void *)co_false },
    { COMP, "IsCurrentSceneFocusAppLoading", (void *)co_false },

    { INP, "SetActionManifestPath", (void *)in_manifest }, { INP, "GetActionSetHandle", (void *)in_set_handle },
    { INP, "GetActionHandle", (void *)in_action_handle }, { INP, "GetInputSourceHandle", (void *)in_source_handle },
    { INP, "UpdateActionState", (void *)in_update },
    { INP, "GetDigitalActionData", (void *)in_digital }, { INP, "GetAnalogActionData", (void *)in_analog },
    { INP, "GetPoseActionDataRelativeToNow", (void *)in_pose_now },
    { INP, "GetPoseActionDataForNextFrame", (void *)in_pose_next },
    { INP, "GetSkeletalActionData", (void *)in_skeletal }, { INP, "GetDominantHand", (void *)in_dominant },
    { INP, "GetBoneCount", (void *)in_no_data }, { INP, "GetSkeletalBoneData", (void *)in_no_data },
    { INP, "GetSkeletalReferenceTransforms", (void *)in_no_data },
    { INP, "GetSkeletalBoneDataCompressed", (void *)in_no_data },
    { INP, "GetSkeletalTrackingLevel", (void *)in_tracking_level },
    { INP, "GetSkeletalSummaryData", (void *)in_summary },
    { INP, "TriggerHapticVibrationAction", (void *)in_haptic },
    { INP, "IsUsingLegacyInput", (void *)in_false },

    { CHAP, "GetCalibrationState", (void *)ch_state }, { CHAP, "GetPlayAreaSize", (void *)ch_size },
    { CHAP, "GetPlayAreaRect", (void *)ch_rect }, { CHAP, "AreBoundsVisible", (void *)co_false },

    { SETT, "GetBool", (void *)st_get_bool }, { SETT, "GetInt32", (void *)st_get_bool },
    { SETT, "GetFloat", (void *)st_get_float }, { SETT, "GetString", (void *)st_get_string },
};

static void *impl_for(const char *ver, const char *m) {
    for (size_t i = 0; i < sizeof g_impl / sizeof *g_impl; i++)
        if (!strcmp(g_impl[i].ver, ver) && !strcmp(g_impl[i].method, m)) return g_impl[i].fn;
    return NULL;
}

static vobj *g_objs[48];
static int g_nobjs;
static pthread_mutex_t g_omu = PTHREAD_MUTEX_INITIALIZER;
static vobj *iface(const char *ver) {
    if (!ver) return NULL;
    pthread_mutex_lock(&g_omu);
    for (int i = 0; i < g_nobjs; i++)
        if (!strcmp(g_objs[i]->ver, ver)) { pthread_mutex_unlock(&g_omu); return g_objs[i]; }
    const iface_desc *d = NULL;
    for (size_t i = 0; i < sizeof g_desc / sizeof *g_desc; i++)
        if (!strcmp(g_desc[i].ver, ver)) d = &g_desc[i];
    int n = 0;
    if (d) while (d->slots[n]) n++;
    else n = 128;
    vobj *o = calloc(1, sizeof *o);
    o->ver = strdup(ver);
    o->vt = calloc((size_t)n + 1, sizeof *o->vt);
    for (int s = 0; s < n; s++) {
        void *fn = d ? impl_for(ver, d->slots[s]) : NULL;
        if (!fn) {
            char nm[200];
            snprintf(nm, sizeof nm, "%s::%s (#%d)", ver, d ? d->slots[s] : "?", s);
            fn = kl_named_stub(nm, (void *)ovr_unimpl);
        }
        o->vt[s] = fn;
    }
    if (g_nobjs < 48) g_objs[g_nobjs++] = o;
    pthread_mutex_unlock(&g_omu);
    fprintf(stderr, "  [openvr] interface %s -> %p (%s, %d slots)\n", ver, (void *)o,
            d ? d->cls : "not in the public header: every slot a logging stub", n);
    return o;
}

// ---- the exports -----------------------------------------------------------------------------------
static uint32_t g_token = 1;
static uint32_t vr_init(int *err, int type, const char *info) {
    if (err) *err = 0;
    fprintf(stderr, "  [openvr] VR_InitInternal2(type %d%s%s) -> OK: Klepton's runtime "
                    "(HMD + two Touch-style controllers via kl_ovrp)\n",
            type, info ? ", " : "", info ? info : "");
    return g_token;
}
static void vr_shutdown(void) { fprintf(stderr, "  [openvr] VR_ShutdownInternal\n"); g_token++; }
static void *vr_generic(const char *ver, int *err) {
    if (err) *err = 0;
    if (ver && !strncmp(ver, "FnTable:", 8)) {
        fprintf(stderr, "  [openvr] %s: FnTable interfaces are not served\n", ver);
        if (err) *err = 105;                           // VRInitError_Init_InterfaceNotFound
        return NULL;
    }
    // SteamVR's private shared-texture service. libvr's CVr::CreateSwapchain
    // uses it to allocate the eye swapchains in vrserver and import them back
    // by fd (VK_KHR_external_memory_fd), which MoltenVK cannot import. Without
    // it libvr says "No IPCResourceManagerClient available. Using copies!" and
    // renders into its own images, which IVRCompositor::Submit hands us — the
    // path kl_openvr already serves. A logging stub here instead answered
    // NewSharedVulkanImage with false and the engine crashed on the NULL
    // swapchain (CVr::CreateSwapchain NewSharedVulkanImage failed).
    if (ver && !strncmp(ver, "IVRIPCResourceManagerClient", 27)) {
        fprintf(stderr, "  [openvr] %s: refused — libvr falls back to its own eye images "
                        "and Submit\n", ver);
        if (err) *err = 105;
        return NULL;
    }
    return iface(ver);
}
static int vr_valid(const char *ver) { (void)ver; return 1; }
static uint32_t vr_token(void) { return g_token; }
static const char *vr_err_symbol(int e) { if (e) fprintf(stderr, "  [openvr] asked to name init error %d\n", e); return e ? "VRInitError_Unknown" : "VRInitError_None"; }
static const char *vr_err_english(int e) { return e ? "Unknown error (Klepton OpenVR)" : "No Error (0)"; }
static int vr_true(void) { return 1; }

typedef struct { const char *name; void *fn; } flat;
static const flat g_flat[] = {
    { "VR_InitInternal2", (void *)vr_init }, { "VR_InitInternal", (void *)vr_init },
    { "VR_ShutdownInternal", (void *)vr_shutdown },
    { "VR_GetGenericInterface", (void *)vr_generic },
    { "VR_IsInterfaceVersionValid", (void *)vr_valid },
    { "VR_GetInitToken", (void *)vr_token },
    { "VR_GetVRInitErrorAsSymbol", (void *)vr_err_symbol },
    { "VR_GetVRInitErrorAsEnglishDescription", (void *)vr_err_english },
    { "VR_GetStringForHmdError", (void *)vr_err_english },
    { "VR_IsHmdPresent", (void *)vr_true }, { "VR_IsRuntimeInstalled", (void *)vr_true },
};

void *kl_openvr_lookup(const char *name) {
    if (!name || name[0] != 'V' || strncmp(name, "VR_", 3)) return NULL;
    if (!kl_env_on("KL_OPENVR", 1)) return NULL;
    for (size_t i = 0; i < sizeof g_flat / sizeof *g_flat; i++)
        if (!strcmp(g_flat[i].name, name)) return g_flat[i].fn;
    return NULL;
}
