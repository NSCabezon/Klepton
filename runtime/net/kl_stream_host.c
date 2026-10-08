// kl_stream_host.c — the Mac side of "Jugar desde el Mac": UDP server,
// Bonjour, pose-in, frame-out pacing, packetisation and the 1 s HUD line.
// The Metal compose + VideoToolbox encode live in kl_stream_host_mtl.m.
//
// Threads:
//   net    — owns the socket's receive side and is the ONLY thread that
//            publishes poses into kl_ovrp (its seqlock is single-writer).
//            Also Bonjour, PING, client timeout and the stats line.
//   frame  — polls kl_vulkan_frame_serial(); on a new guest frame pulls the
//            haptics envelope, composes and hands the frame to the encoder.
//   VT     — VideoToolbox's output thread fragments and sends VIDEO.
//   main   — waits for quit, or runs the KL_STREAM_PREVIEW window.
//
// Knobs (all optional):
//   KL_STREAM_HOST=1             select this frontend (m_boot)
//   KL_STREAM_PORT=9944          UDP port (KL_STREAM_PORT_DEFAULT)
//   KL_STREAM_NAME=<s>           Bonjour instance name (default: computer name)
//   KL_STREAM_BITRATE_KBPS=100000  average bitrate; capped by HELLO.max_bitrate_kbps
//   KL_STREAM_FPS=60             ExpectedFrameRate / HELLO_ACK.host_fps_target
//   KL_STREAM_SCALE=1.0          encoded eye size = HELLO eye size x this
//   KL_STREAM_WAIT=1             wait for the first HELLO before booting the guest
//                                (0 = boot now with the built-in Vision Pro defaults)
//   KL_STREAM_TIMEOUT_MS=2000    no POSE for this long = client gone
//   KL_STREAM_PREVIEW=1          local preview window of the encoded SBS frame
#include "kl_stream_host.h"
#include "kl_stream_proto.h"

#include <arpa/inet.h>
#include <dns_sd.h>
#include <errno.h>
#include <math.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "kl_env.h"
#include "kl_ovrp.h"
#include "kl_vulkan.h"

#define SH "[stream-host] "

double kl_stream_host_now(void) {
    struct timespec t;
    clock_gettime(CLOCK_UPTIME_RAW, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}
static uint64_t mono_ns(void) {     // the frame record's clock (submit_ns)
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

int kl_stream_host_enabled(void) { return kl_env_on("KL_STREAM_HOST", 0); }

// ---- state ------------------------------------------------------------------

static int g_fd = -1;
static int g_port;
static char g_guest[32] = "guest";
static DNSServiceRef g_dns;

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cv = PTHREAD_COND_INITIALIZER;
static struct {
    int      active;               // a client is streaming
    int      configured;           // a HELLO has been applied to kl_ovrp, ever
    struct sockaddr_storage addr;
    socklen_t addr_len;
    double   last_heard;           // host time of the last POSE/HELLO from it
    // what the encoder is asked for
    int      frame_w, frame_h, kbps;
    float    fps;
    // what the guest was configured with (for the late-config warning)
    float    hz;
    uint32_t eye_w, eye_h;
    float    tan[2][4];
} C;
static int g_guest_running;        // set once the guest thread exists
static volatile int g_idr;         // force a keyframe on the next frame
static uint32_t g_seq[16];         // per-type send sequence
static uint32_t g_frame_id;

// pose_id -> host receipt time, for the pose-age statistic
#define POSE_RING 1024
static struct { uint32_t id; uint64_t ns; double t; float q[4]; } g_pose_ring[POSE_RING];

static struct {
    unsigned frames, keyframes, packets, poses, send_err;
    double   enc_ms, compose_ms, age_ms, render_ms, send_ms;
    unsigned retries, skipped, untagged;
    float    tag_err_deg;   // worst angle between the frame record's head and the echoed POSE's
    unsigned age_n;
    uint64_t bytes;
    double   rtt_ms;
    unsigned long long total_frames, total_key;
} S;

static void hdr_init(kl_stream_hdr *h, uint16_t type) {
    h->magic = KL_STREAM_MAGIC;
    h->version = KL_STREAM_VERSION;
    h->type = type;
    h->seq = __atomic_fetch_add(&g_seq[type & 15], 1, __ATOMIC_RELAXED);
}

static const char *addr_str(const struct sockaddr_storage *a, char *buf, size_t n) {
    char ip[INET6_ADDRSTRLEN] = "?";
    int port = 0;
    if (a->ss_family == AF_INET) {
        const struct sockaddr_in *s = (const struct sockaddr_in *)a;
        inet_ntop(AF_INET, &s->sin_addr, ip, sizeof ip);
        port = ntohs(s->sin_port);
    } else if (a->ss_family == AF_INET6) {
        const struct sockaddr_in6 *s = (const struct sockaddr_in6 *)a;
        inet_ntop(AF_INET6, &s->sin6_addr, ip, sizeof ip);
        port = ntohs(s->sin6_port);
    }
    snprintf(buf, n, "%s:%d", ip, port);
    return buf;
}

static int addr_eq(const struct sockaddr_storage *a, socklen_t al,
                   const struct sockaddr_storage *b, socklen_t bl) {
    return al == bl && memcmp(a, b, al) == 0;
}

// Send to the current client. From any thread: sendto on one UDP socket is
// thread-safe; the address is copied under the lock. ENOBUFS (the interface
// queue is full — a 200 KB frame is ~170 packets in one burst) is retried
// briefly rather than dropped, since a lost fragment is a lost frame.
static int send_client(const void *buf, size_t n) {
    struct sockaddr_storage a;
    socklen_t al;
    pthread_mutex_lock(&g_mu);
    int on = C.active;
    a = C.addr; al = C.addr_len;
    pthread_mutex_unlock(&g_mu);
    if (!on || g_fd < 0) return -1;
    for (int tries = 0; tries < 200; tries++) {
        if (sendto(g_fd, buf, n, 0, (const struct sockaddr *)&a, al) == (ssize_t)n) return 0;
        if (errno != ENOBUFS && errno != EAGAIN) break;
        S.retries++;
        usleep(50);
    }
    S.send_err++;
    return -1;
}

static void send_to(const struct sockaddr_storage *a, socklen_t al, const void *buf, size_t n) {
    sendto(g_fd, buf, n, 0, (const struct sockaddr *)a, al);
}

// ---- display description -> kl_ovrp ------------------------------------------

static void apply_display(const char *who, const char *device, float hz,
                          uint32_t eye_w, uint32_t eye_h, const kl_stream_eye eye[2]) {
    int late = __atomic_load_n(&g_guest_running, __ATOMIC_ACQUIRE);
    int changed = hz != C.hz || eye_w != C.eye_w || eye_h != C.eye_h;
    for (int e = 0; e < 2; e++) {
        const kl_stream_eye *d = &eye[e];
        if (d->tan_left != C.tan[e][0] || d->tan_right != C.tan[e][1] ||
            d->tan_up != C.tan[e][2] || d->tan_down != C.tan[e][3]) changed = 1;
        kl_ovrp_set_eye_frustum(e, d->tan_left, d->tan_right, d->tan_up, d->tan_down);
        kl_ovrp_set_eye_offset(e, d->offset.x, d->offset.y, d->offset.z);
        kl_ovrp_set_eye_rotation(e, d->rotation.x, d->rotation.y, d->rotation.z, d->rotation.w);
        C.tan[e][0] = d->tan_left; C.tan[e][1] = d->tan_right;
        C.tan[e][2] = d->tan_up;   C.tan[e][3] = d->tan_down;
    }
    if (hz > 0) kl_ovrp_set_display_frequency(hz);
    if (eye_w && eye_h) kl_ovrp_set_eye_texture_size((int)eye_w, (int)eye_h);
    C.hz = hz; C.eye_w = eye_w; C.eye_h = eye_h;
    fprintf(stderr, SH "display from %s (%s): %.1f Hz, eye %ux%u, IPD %.1f mm, "
                    "tan L(l %.4f r %.4f u %.4f d %.4f) R(l %.4f r %.4f u %.4f d %.4f)\n",
            who, device, (double)hz, eye_w, eye_h,
            (double)((eye[1].offset.x - eye[0].offset.x) * 1000.0f),
            (double)eye[0].tan_left, (double)eye[0].tan_right, (double)eye[0].tan_up,
            (double)eye[0].tan_down, (double)eye[1].tan_left, (double)eye[1].tan_right,
            (double)eye[1].tan_up, (double)eye[1].tan_down);
    if (late && changed)
        fprintf(stderr, SH "LATE display description: the guest is already running. The "
                        "frustum and eye offsets apply from the next frame (kl_ovrp reads them "
                        "per frame); the eye size re-creates the stage images; the display rate "
                        "is a number Alyx read once at boot and keeps. Each VIDEO frame still "
                        "carries the tangents the guest actually rendered with, so the headset "
                        "places it correctly — at worst with less field than it asked for.\n");
}

// The Vision Pro-like description the fake client also sends: tangents from
// the device (eye 0 wider on its temporal/left side, mirrored for eye 1), the
// 66.9 mm IPD of the bench unit, 90 Hz, 1888x1824 per eye.
static void default_eyes(kl_stream_eye eye[2]) {
    memset(eye, 0, 2 * sizeof eye[0]);
    const float ipd = 0.0669f;
    eye[0].tan_left = 1.7321f; eye[0].tan_right = 1.0f;
    eye[0].tan_up = 1.0f;      eye[0].tan_down = 1.1918f;
    eye[1].tan_left = 1.0f;    eye[1].tan_right = 1.7321f;
    eye[1].tan_up = 1.0f;      eye[1].tan_down = 1.1918f;
    eye[0].offset.x = -ipd * 0.5f;
    eye[1].offset.x =  ipd * 0.5f;
    eye[0].rotation.w = eye[1].rotation.w = 1.0f;
}

static void frame_size_for(uint32_t eye_w, uint32_t eye_h, int *fw, int *fh) {
    float sc = kl_env_float("KL_STREAM_SCALE", 1.0f);
    if (!(sc > 0.1f && sc <= 2.0f)) sc = 1.0f;
    int w = (int)((float)eye_w * sc + 0.5f) & ~1;
    int h = (int)((float)eye_h * sc + 0.5f) & ~1;
    if (w < 64) w = 64;
    if (h < 64) h = 64;
    *fw = 2 * w; *fh = h;
}

void kl_stream_host_apply_defaults(void) {
    kl_stream_eye eye[2];
    default_eyes(eye);
    pthread_mutex_lock(&g_mu);
    apply_display("built-in defaults", "Vision Pro (no client yet)", 90.0f, 1888, 1824, eye);
    frame_size_for(1888, 1824, &C.frame_w, &C.frame_h);
    pthread_mutex_unlock(&g_mu);
}

// ---- receive ------------------------------------------------------------------

static void on_hello(const kl_stream_hello *m, size_t n,
                     const struct sockaddr_storage *from, socklen_t fl) {
    kl_stream_hello_ack ack;
    memset(&ack, 0, sizeof ack);
    hdr_init(&ack.h, KL_STREAM_HELLO_ACK);
    snprintf(ack.guest, sizeof ack.guest, "%s", g_guest);
    ack.codec = 0x31637668u;   // 'hvc1'
    char who[80];
    addr_str(from, who, sizeof who);
    if (n < sizeof *m || m->h.version != KL_STREAM_VERSION) {
        ack.status = KL_STREAM_ERR_VERSION;
        fprintf(stderr, SH "HELLO from %s refused: version %u (we speak %u) or size %zu\n",
                who, m->h.version, KL_STREAM_VERSION, n);
        send_to(from, fl, &ack, sizeof ack);
        return;
    }
    double now = kl_stream_host_now();
    double timeout = kl_env_int("KL_STREAM_TIMEOUT_MS", 2000) / 1000.0;
    pthread_mutex_lock(&g_mu);
    int same = C.active && addr_eq(&C.addr, C.addr_len, from, fl);
    if (C.active && !same && now - C.last_heard < timeout) {
        pthread_mutex_unlock(&g_mu);
        ack.status = KL_STREAM_ERR_BUSY;
        fprintf(stderr, SH "HELLO from %s refused: busy streaming to another client\n", who);
        send_to(from, fl, &ack, sizeof ack);
        return;
    }
    char dev[33];
    memcpy(dev, m->device, 32); dev[32] = 0;
    if (!same) {
        apply_display(who, dev, m->display_hz, m->eye_w, m->eye_h, m->eye);
        frame_size_for(m->eye_w, m->eye_h, &C.frame_w, &C.frame_h);
        int kbps = kl_env_int("KL_STREAM_BITRATE_KBPS", 100000);
        if (m->max_bitrate_kbps && (int)m->max_bitrate_kbps < kbps) kbps = (int)m->max_bitrate_kbps;
        C.kbps = kbps;
        C.fps = kl_env_float("KL_STREAM_FPS", 60.0f);
        C.addr = *from; C.addr_len = fl;
        C.active = 1;
        // A new session numbers its poses from scratch, so a pose_id the guest
        // rendered with in a previous session must not be echoed as one of
        // this client's: frames until its first POSE is drawn go out with 0.
        memset(g_pose_ring, 0, sizeof g_pose_ring);
        g_idr = 1;
        fprintf(stderr, SH "client %s connected: streaming %dx%d HEVC at %d kbit/s, %.0f fps target\n",
                who, C.frame_w, C.frame_h, C.kbps, (double)C.fps);
    }
    C.last_heard = now;
    C.configured = 1;
    ack.status = KL_STREAM_OK;
    ack.frame_w = (uint32_t)C.frame_w;
    ack.frame_h = (uint32_t)C.frame_h;
    ack.bitrate_kbps = (uint32_t)C.kbps;
    ack.host_fps_target = C.fps;
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mu);
    send_to(from, fl, &ack, sizeof ack);
}

static void on_pose(const kl_stream_pose *m, size_t n,
                    const struct sockaddr_storage *from, socklen_t fl) {
    if (n < sizeof *m) return;
    pthread_mutex_lock(&g_mu);
    int ok = C.active && addr_eq(&C.addr, C.addr_len, from, fl);
    if (ok) C.last_heard = kl_stream_host_now();
    pthread_mutex_unlock(&g_mu);
    if (!ok) return;
    uint64_t ns = mono_ns();
    g_pose_ring[m->pose_id % POSE_RING].id = m->pose_id;
    g_pose_ring[m->pose_id % POSE_RING].ns = ns;
    g_pose_ring[m->pose_id % POSE_RING].t = m->t_pose;
    g_pose_ring[m->pose_id % POSE_RING].q[0] = m->head_rot.x;
    g_pose_ring[m->pose_id % POSE_RING].q[1] = m->head_rot.y;
    g_pose_ring[m->pose_id % POSE_RING].q[2] = m->head_rot.z;
    g_pose_ring[m->pose_id % POSE_RING].q[3] = m->head_rot.w;
    S.poses++;
    // The head: tag, time, pose — in that order, from this one thread. The
    // tag travels through the guest's frame latch into the frame record, which
    // is how VIDEO names the pose the frame was rendered with.
    kl_ovrp_set_head_pose_tag(m->pose_id);
    kl_ovrp_set_head_pose_time(m->t_pose);
    kl_ovrp_set_head_pose(m->head_pos.x, m->head_pos.y, m->head_pos.z,
                          m->head_rot.x, m->head_rot.y, m->head_rot.z, m->head_rot.w);
    for (int hand = 0; hand < 2; hand++) {
        const kl_stream_hand *h = &m->hand[hand];
        if (h->flags & KL_STREAM_HAND_TRACKED)
            kl_ovrp_set_hand_motion(hand, h->pos.x, h->pos.y, h->pos.z,
                                    h->rot.x, h->rot.y, h->rot.z, h->rot.w,
                                    h->vel.x, h->vel.y, h->vel.z,
                                    h->ang_vel.x, h->ang_vel.y, h->ang_vel.z);
        if (h->flags & KL_STREAM_HAND_CONNECTED)
            kl_ovrp_set_controller_input(hand, h->buttons, h->touches, h->index_trigger,
                                         h->hand_trigger, h->stick_x, h->stick_y);
    }
}

static void on_packet(const uint8_t *buf, size_t n, const struct sockaddr_storage *from,
                      socklen_t fl) {
    if (n < sizeof(kl_stream_hdr)) return;
    kl_stream_hdr h;
    memcpy(&h, buf, sizeof h);
    if (h.magic != KL_STREAM_MAGIC) return;
    if (h.type == KL_STREAM_HELLO) { on_hello((const kl_stream_hello *)buf, n, from, fl); return; }
    if (h.version != KL_STREAM_VERSION) return;
    switch (h.type) {
    case KL_STREAM_POSE:
        on_pose((const kl_stream_pose *)buf, n, from, fl);
        break;
    case KL_STREAM_IDR_REQUEST:
        g_idr = 1;
        fprintf(stderr, SH "IDR requested by the client\n");
        break;
    case KL_STREAM_PING:
        if (n >= sizeof(kl_stream_ping)) {
            kl_stream_ping p;
            memcpy(&p, buf, sizeof p);
            hdr_init(&p.h, KL_STREAM_PONG);
            p.t_reply = kl_stream_host_now();
            send_to(from, fl, &p, sizeof p);
        }
        break;
    case KL_STREAM_PONG:
        if (n >= sizeof(kl_stream_ping)) {
            kl_stream_ping p;
            memcpy(&p, buf, sizeof p);
            S.rtt_ms = (kl_stream_host_now() - p.t_send) * 1000.0;
        }
        break;
    case KL_STREAM_BYE: {
        pthread_mutex_lock(&g_mu);
        int was = C.active && addr_eq(&C.addr, C.addr_len, from, fl);
        if (was) C.active = 0;
        pthread_mutex_unlock(&g_mu);
        if (was) fprintf(stderr, SH "client said BYE — streaming stopped, the guest keeps running\n");
        break;
    }
    default:
        break;
    }
}

// ---- the 1 s line ----------------------------------------------------------------

static void stats_tick(double dt) {
    char who[80] = "none";
    pthread_mutex_lock(&g_mu);
    int on = C.active;
    if (on) addr_str(&C.addr, who, sizeof who);
    pthread_mutex_unlock(&g_mu);
    unsigned f = S.frames;
    if (!on && !f) return;   // quiet while nobody is connected
    fprintf(stderr, SH "%.1f fps encoded, encode %.1f ms (compose %.1f, send %.1f), render %.1f ms, "
                    "%.1f Mbit/s, %.0f pkt/s, pose %.0f Hz, pose age %.1f ms, rtt %.2f ms, "
                    "client %s, total %llu frames (%llu key)%s\n",
            f / dt, f ? S.enc_ms / f : 0.0, f ? S.compose_ms / f : 0.0, f ? S.send_ms / f : 0.0,
            f ? S.render_ms / f : 0.0,
            (double)S.bytes * 8.0 / 1e6 / dt, S.packets / dt, S.poses / dt,
            S.age_n ? S.age_ms / S.age_n : 0.0, S.rtt_ms, who,
            S.total_frames, S.total_key, S.send_err ? " SEND ERRORS" : "");
    if (S.retries) fprintf(stderr, SH "  %u sends retried on a full interface queue (ENOBUFS)\n", S.retries);
    if (f) fprintf(stderr, SH "  pose tag check: frame head vs echoed POSE head differ by %.3f deg "
                           "at worst%s\n", (double)S.tag_err_deg,
                   S.untagged ? " (some frames rendered before this client's first POSE: sent as pose_id 0)" : "");
    if (S.skipped) fprintf(stderr, SH "  %u guest frames skipped: the encoder was already holding "
                                   "KL_STREAM_MAX_INFLIGHT frames\n", S.skipped);
    S.frames = S.keyframes = S.packets = S.poses = S.send_err = 0;
    S.enc_ms = S.compose_ms = S.age_ms = S.render_ms = S.send_ms = 0;
    S.retries = S.skipped = S.untagged = 0;
    S.tag_err_deg = 0;
    S.age_n = 0;
    S.bytes = 0;
}

static void *net_thread(void *arg) {
    (void)arg;
    uint8_t buf[2048];
    double t_stats = kl_stream_host_now(), t_ping = t_stats;
    double timeout = kl_env_int("KL_STREAM_TIMEOUT_MS", 2000) / 1000.0;
    for (;;) {
        struct pollfd pf[2] = { { g_fd, POLLIN, 0 }, { -1, POLLIN, 0 } };
        int np = 1;
        if (g_dns) { pf[1].fd = DNSServiceRefSockFD(g_dns); np = 2; }
        int r = poll(pf, (nfds_t)np, 10);
        if (r > 0 && (pf[0].revents & POLLIN)) {
            for (;;) {
                struct sockaddr_storage from;
                socklen_t fl = sizeof from;
                ssize_t n = recvfrom(g_fd, buf, sizeof buf, MSG_DONTWAIT,
                                     (struct sockaddr *)&from, &fl);
                if (n <= 0) break;
                on_packet(buf, (size_t)n, &from, fl);
            }
        }
        if (r > 0 && np == 2 && (pf[1].revents & POLLIN)) DNSServiceProcessResult(g_dns);
        double now = kl_stream_host_now();
        pthread_mutex_lock(&g_mu);
        int gone = C.active && now - C.last_heard > timeout;
        if (gone) C.active = 0;
        int on = C.active;
        pthread_mutex_unlock(&g_mu);
        if (gone)
            fprintf(stderr, SH "client timed out (no POSE for %.1f s) — streaming stopped, the "
                            "guest keeps running\n", timeout);
        if (on && now - t_ping >= 1.0) {
            kl_stream_ping p;
            memset(&p, 0, sizeof p);
            hdr_init(&p.h, KL_STREAM_PING);
            p.t_send = now;
            send_client(&p, sizeof p);
            t_ping = now;
        }
        if (now - t_stats >= 1.0) {
            stats_tick(now - t_stats);
            t_stats = now;
        }
    }
    return NULL;
}

static void DNSSD_API dns_cb(DNSServiceRef ref, DNSServiceFlags flags, DNSServiceErrorType err,
                             const char *name, const char *type, const char *domain, void *ctx) {
    (void)ref; (void)flags; (void)ctx;
    if (err) fprintf(stderr, SH "Bonjour registration failed: %d\n", (int)err);
    else     fprintf(stderr, SH "Bonjour: '%s' %s%s\n", name, type, domain);
}

int kl_stream_host_start(const char *guest) {
    if (guest) snprintf(g_guest, sizeof g_guest, "%s", guest);
    g_port = kl_env_int("KL_STREAM_PORT", KL_STREAM_PORT_DEFAULT);
    // One dual-stack socket: the headset may reach the Mac over IPv6
    // link-local as easily as over IPv4.
    g_fd = socket(AF_INET6, SOCK_DGRAM, 0);
    if (g_fd < 0) { perror(SH "socket"); return -1; }
    int off = 0, big = 8 << 20, one = 1;
    setsockopt(g_fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof off);
    setsockopt(g_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    while (big > (256 << 10) &&
           setsockopt(g_fd, SOL_SOCKET, SO_SNDBUF, &big, sizeof big) != 0) big /= 2;
    big = 4 << 20;
    while (big > (256 << 10) &&
           setsockopt(g_fd, SOL_SOCKET, SO_RCVBUF, &big, sizeof big) != 0) big /= 2;
    struct sockaddr_in6 a;
    memset(&a, 0, sizeof a);
    a.sin6_family = AF_INET6;
    a.sin6_len = sizeof a;
    a.sin6_port = htons((uint16_t)g_port);
    a.sin6_addr = in6addr_any;
    if (bind(g_fd, (struct sockaddr *)&a, sizeof a) != 0) {
        fprintf(stderr, SH "bind :%d failed: %s\n", g_port, strerror(errno));
        close(g_fd); g_fd = -1;
        return -1;
    }
    TXTRecordRef txt;
    TXTRecordCreate(&txt, 0, NULL);
    TXTRecordSetValue(&txt, "guest", (uint8_t)strlen(g_guest), g_guest);
    char v[8];
    snprintf(v, sizeof v, "%u", KL_STREAM_VERSION);
    TXTRecordSetValue(&txt, "v", (uint8_t)strlen(v), v);
    DNSServiceErrorType e = DNSServiceRegister(&g_dns, 0, 0, getenv("KL_STREAM_NAME"),
                                               KL_STREAM_BONJOUR, NULL, NULL,
                                               htons((uint16_t)g_port),
                                               TXTRecordGetLength(&txt),
                                               TXTRecordGetBytesPtr(&txt), dns_cb, NULL);
    TXTRecordDeallocate(&txt);
    if (e != kDNSServiceErr_NoError) {
        fprintf(stderr, SH "DNSServiceRegister failed (%d) — reachable by address only\n", (int)e);
        g_dns = NULL;
    }
    pthread_t t;
    if (pthread_create(&t, NULL, net_thread, NULL)) { perror(SH "pthread_create"); return -1; }
    pthread_detach(t);
    fprintf(stderr, SH "listening on UDP :%d (%s), guest '%s'\n", g_port, KL_STREAM_BONJOUR, g_guest);
    return 0;
}

int kl_stream_host_wait_hello(double timeout_s, const volatile int *quit) {
    double t_end = timeout_s >= 0 ? kl_stream_host_now() + timeout_s : 0;
    pthread_mutex_lock(&g_mu);
    while (!C.configured && !(quit && *quit) && (timeout_s < 0 || kl_stream_host_now() < t_end)) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 100 * 1000000L;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&g_cv, &g_mu, &ts);
    }
    int got = C.configured;
    pthread_mutex_unlock(&g_mu);
    return got;
}

// ---- frame out ---------------------------------------------------------------------

static void on_encoded(const kl_stream_frame_meta *m, const uint8_t *au, size_t n,
                       int keyframe, float ms_encode) {
    uint8_t pkt[sizeof(kl_stream_video) + KL_STREAM_MTU_PAYLOAD];
    kl_stream_video *v = (kl_stream_video *)pkt;
    memset(v, 0, sizeof *v);
    v->frame_id = m->frame_id;
    v->pose_id = m->pose_id;
    v->t_pose = m->t_pose;
    v->flags = keyframe ? KL_STREAM_VIDEO_KEYFRAME : 0;
    v->frame_bytes = (uint32_t)n;
    uint32_t count = (uint32_t)((n + KL_STREAM_MTU_PAYLOAD - 1) / KL_STREAM_MTU_PAYLOAD);
    v->frag_count = (uint16_t)count;
    memcpy(v->tan, m->tan, sizeof v->tan);
    v->host_ms_render = m->ms_render;
    v->host_ms_encode = ms_encode;
    if (count > 0xffff) {
        fprintf(stderr, SH "frame %u is %zu bytes — more fragments than the protocol can "
                        "number; dropped (lower KL_STREAM_BITRATE_KBPS)\n", m->frame_id, n);
        return;
    }
    unsigned sent = 0;
    double t_send0 = kl_stream_host_now();
    for (uint32_t i = 0; i < count; i++) {
        size_t off = (size_t)i * KL_STREAM_MTU_PAYLOAD;
        size_t len = n - off < KL_STREAM_MTU_PAYLOAD ? n - off : KL_STREAM_MTU_PAYLOAD;
        hdr_init(&v->h, KL_STREAM_VIDEO);
        v->frag_index = (uint16_t)i;
        memcpy(pkt + sizeof *v, au + off, len);
        if (send_client(pkt, sizeof *v + len) == 0) sent++;
    }
    S.send_ms += (kl_stream_host_now() - t_send0) * 1000.0;
    S.frames++;
    S.total_frames++;
    if (keyframe) { S.keyframes++; S.total_key++; }
    S.packets += sent;
    S.bytes += n;
    S.enc_ms += ms_encode;
    S.compose_ms += (m->t_encode_start - m->t_compose_start) * 1000.0;
    S.render_ms += m->ms_render;
    if (m->pose_host_ns) {
        S.age_ms += (double)(mono_ns() - m->pose_host_ns) / 1e6;
        S.age_n++;
    }
}

static volatile int g_sig;
static void on_sig(int s) { (void)s; g_sig = 1; }

typedef struct { const volatile int *quit; } frame_args;

static void *frame_thread(void *arg) {
    const volatile int *quit = ((frame_args *)arg)->quit;
    unsigned long long last = kl_vulkan_frame_serial();
    int said_first = 0;
    while (!(quit && *quit) && !g_sig) {
        unsigned long long s = kl_vulkan_frame_serial();
        if (s == last) { usleep(500); continue; }
        last = s;
        // The haptics envelope: once per hand per guest frame (kl_ovrp.h), and
        // forwarded only while someone can feel it.
        pthread_mutex_lock(&g_mu);
        int on = C.active;
        int fw = C.frame_w, fh = C.frame_h, kbps = C.kbps;
        float fps = C.fps;
        pthread_mutex_unlock(&g_mu);
        for (int hand = 0; hand < 2; hand++) {
            float amp = 0, secs = 0;
            if (kl_ovrp_haptics_pull(hand, &amp, &secs) && on) {
                kl_stream_haptics hp;
                memset(&hp, 0, sizeof hp);
                hdr_init(&hp.h, KL_STREAM_HAPTICS);
                hp.hand = (uint32_t)hand;
                hp.amplitude = amp;
                hp.seconds = secs;
                send_client(&hp, sizeof hp);
            }
        }
        if (!on || fw <= 0) continue;
        int stage = kl_ovrp_last_complete_stage();
        if (stage < 0) continue;
        kl_ovrp_render_pose r;
        if (!kl_ovrp_stage_render_pose(stage, &r)) continue;
        kl_stream_frame_meta m;
        memset(&m, 0, sizeof m);
        m.pose_id = (uint32_t)r.head_tag;
        m.t_pose = r.head_time;
        memcpy(m.tan, r.tangents, sizeof m.tan);
        uint64_t now_ns = mono_ns();
        m.ms_render = r.submit_ns && now_ns > r.submit_ns ? (float)((now_ns - r.submit_ns) / 1e6) : 0;
        if (g_pose_ring[m.pose_id % POSE_RING].id == m.pose_id && r.head_tag &&
            g_pose_ring[m.pose_id % POSE_RING].t == r.head_time) {
            m.pose_host_ns = g_pose_ring[m.pose_id % POSE_RING].ns;
            // The tag's own check: the head the frame record says the guest
            // drew with must BE the head of the POSE we are about to echo.
            const float *q = g_pose_ring[m.pose_id % POSE_RING].q;
            // In double, from the vector part: acos of a float dot product near
            // 1 cannot resolve less than ~0.04 deg.
            double cx = (double)q[3] * r.qx - (double)q[0] * r.qw - ((double)q[1] * r.qz - (double)q[2] * r.qy);
            double cy = (double)q[3] * r.qy - (double)q[1] * r.qw - ((double)q[2] * r.qx - (double)q[0] * r.qz);
            double cz = (double)q[3] * r.qz - (double)q[2] * r.qw - ((double)q[0] * r.qy - (double)q[1] * r.qx);
            double sn = sqrt(cx * cx + cy * cy + cz * cz);
            float deg = (float)(2.0 * asin(sn > 1.0 ? 1.0 : sn) * 57.29577951308232);
            if (deg > S.tag_err_deg) S.tag_err_deg = deg;
        } else {
            // Rendered with no pose of THIS client's (before its first POSE, or
            // with the previous session's): pose_id 0 = "not yours".
            m.pose_id = 0;
            m.t_pose = 0;
            S.untagged++;
        }
        m.frame_id = g_frame_id + 1;
        int idr = __atomic_exchange_n(&g_idr, 0, __ATOMIC_ACQ_REL);
        int rc = kl_stream_enc_frame(stage, fw, fh, kbps, fps, idr, &m, on_encoded);
        if (rc == -2) S.skipped++;
        if (rc == 0) {
            g_frame_id++;
            if (!said_first++)
                fprintf(stderr, SH "first frame to the encoder: stage %d, pose_id %u, tangents "
                                "L(%.3f %.3f %.3f %.3f) R(%.3f %.3f %.3f %.3f)\n", stage, m.pose_id,
                        (double)m.tan[0][0], (double)m.tan[0][1], (double)m.tan[0][2],
                        (double)m.tan[0][3], (double)m.tan[1][0], (double)m.tan[1][1],
                        (double)m.tan[1][2], (double)m.tan[1][3]);
        } else if (idr) {
            g_idr = 1;    // not consumed
        }
    }
    return NULL;
}

#if __has_include(<SDL3/SDL.h>)
#include <SDL3/SDL.h>
#include <SDL3/SDL_metal.h>
static int preview_loop(const volatile int *quit) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, SH "preview: SDL_Init failed: %s\n", SDL_GetError());
        return -1;
    }
    SDL_Window *win = SDL_CreateWindow("Klepton stream host (preview)", 1280, 640,
                                       SDL_WINDOW_METAL | SDL_WINDOW_RESIZABLE);
    SDL_MetalView mv = win ? SDL_Metal_CreateView(win) : NULL;
    void *layer = mv ? SDL_Metal_GetLayer(mv) : NULL;
    if (!layer) {
        fprintf(stderr, SH "preview: no Metal window (%s)\n", SDL_GetError());
        if (win) SDL_DestroyWindow(win);
        SDL_Quit();
        return -1;
    }
    while (!(quit && *quit) && !g_sig) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev))
            if (ev.type == SDL_EVENT_QUIT || ev.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) g_sig = 1;
        int pw = 0, ph = 0;
        SDL_GetWindowSizeInPixels(win, &pw, &ph);
        kl_stream_preview_present(layer, pw, ph);
        SDL_Delay(16);
    }
    SDL_Metal_DestroyView(mv);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
#else
static int preview_loop(const volatile int *quit) {
    (void)quit;
    fprintf(stderr, SH "preview: built without SDL3\n");
    return -1;
}
#endif

int kl_stream_host_run(const volatile int *quit) {
    __atomic_store_n(&g_guest_running, 1, __ATOMIC_RELEASE);
    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
    static frame_args fa;
    fa.quit = quit;
    pthread_t ft;
    if (pthread_create(&ft, NULL, frame_thread, &fa)) { perror(SH "pthread_create"); return 1; }
    fprintf(stderr, SH "frame-out running (audio stays on this Mac's output in this milestone)%s\n",
            kl_env_on("KL_STREAM_PREVIEW", 0) ? "; preview window on" : "");
    if (!kl_env_on("KL_STREAM_PREVIEW", 0) || preview_loop(quit) != 0)
        while (!(quit && *quit) && !g_sig) usleep(20000);
    g_sig = 1;
    pthread_join(ft, NULL);
    // Tell the client we are going, then stop.
    kl_stream_hdr bye;
    hdr_init(&bye, KL_STREAM_BYE);
    send_client(&bye, sizeof bye);
    kl_stream_enc_close();
    fprintf(stderr, SH "stopped: %llu frames encoded (%llu keyframes)\n", S.total_frames, S.total_key);
    return 0;
}
