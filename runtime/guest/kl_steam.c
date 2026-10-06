// A Steam client that is not there — see kl_steam.h.
//
// WHICH LAYER
//
// The engine never talks to steamclient.so itself. Every library imports the
// same handful of libsteam_api entry points — SteamInternal_SteamAPI_Init,
// SteamInternal_ContextInit, SteamInternal_FindOrCreateUserInterface /
// ...GameServerInterface, SteamInternal_CreateInterface, the callback
// (un)registration pair and the RunCallbacks pumps — and gets C++ interface
// objects back, which it then calls virtually. So this answers at THAT layer:
// those imports bind here (through the glibc ABI tier, ahead of the guest's own
// libsteam_api), and the objects handed back are ours, with vtables laid out
// slot for slot after the Steamworks SDK headers (kl_steam_vtables.h, generated
// by tools/gen_steam_vtables.py; Itanium ABI, `this` in x0).
//
// Every slot of every interface exists. The ones below are implemented; every
// other one is a named stub that logs "<version>::<method> (#slot)" the first
// few times and returns 0 — which the SDK's own conventions mostly read as
// "no", "none" or k_uAPICallInvalid. An interface version the SDK does not
// describe (SteamAudio001, Valve-private) gets 128 such slots.
//
// WHO WE ARE: user 76561197960287930, logged on, owning app 546560 (Half-Life:
// Alyx) and nothing else, language english, universe public, no overlay.
//
// NETWORKING: Source 2 runs even single player as a client connected to a
// listen server in the same process, over ISteamNetworkingSockets. So that
// interface is real, for the one topology that exists here — loopback: listen
// sockets (IP or P2P virtual port), connects to them, CreateSocketPair, poll
// groups, reliable ordered message delivery (loopback cannot lose anything),
// and connection-status callbacks, delivered both through the
// FnSteamNetConnectionStatusChanged config callbacks (global, per listen
// socket, per connection) and through SteamAPI_RegisterCallback(1221). Both
// the user and the game-server accessors return the SAME sockets object, which
// is what lets the client find the server's listen socket at all.
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <CoreFoundation/CoreFoundation.h>

#include "klepton.h"
#include "kl_env.h"
#include "kl_steam.h"

#define APP_ID      546560u
#define USER_ID     76561197960287930ull
#define SERVER_ID   90071992547409921ull

// ---- interface objects ---------------------------------------------------------------
typedef struct { const char *ver, *cls; const char *const *slots; } iface_desc;
#define KL_STEAM_IFACE(ver, cls, ...) { ver, cls, (const char *const[]){ __VA_ARGS__, NULL } },
static const iface_desc g_desc[] = {
#include "kl_steam_vtables.h"
};
#undef KL_STEAM_IFACE

typedef struct { void **vt; const char *ver; } sobj;   // vt first: it is a C++ object
static sobj *g_objs[64];
static int g_nobjs;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

typedef struct { const char *ver, *method; int nth; void *fn; } impl;
static sobj *iface(const char *ver);

// ---- the flat entry points -------------------------------------------------------------
static int  st_init(const char *versions, void *err) {
    (void)versions;
    if (err) memset(err, 0, 1024);
    fprintf(stderr, "  [steam] SteamAPI_Init -> OK (stub client: user %llu, app %u)\n",
            (unsigned long long)USER_ID, APP_ID);
    return 0;                                     // k_ESteamAPIInitResult_OK
}
static int  st_init_bool(void) { st_init(NULL, NULL); return 1; }
static int  st_gs_init(uint32_t ip, uint16_t gport, uint16_t qport, int mode,
                       const char *ver, const char *versions, void *err) {
    (void)ip; (void)qport; (void)versions;
    if (err) memset(err, 0, 1024);
    fprintf(stderr, "  [steam] SteamGameServer_Init(port %u, mode %d, \"%s\") -> OK\n",
            gport, mode, ver ? ver : "");
    return 0;
}
static int  st_hsteamuser(void) { return 1; }
static int  st_hsteampipe(void) { return 1; }
static int  st_gs_hsteamuser(void) { return 2; }
static int  st_true(void) { return 1; }
static int  st_false(void) { return 0; }
static void st_void(void) {}
static void *st_find_user(int hUser, const char *ver) { (void)hUser; return iface(ver); }
static void *st_create_iface(const char *ver) { return iface(ver); }

// CSteamAPIContext and friends: { void (*init)(void *ctx); uintp counter; ctx }.
static uintptr_t g_ctx_counter = 1;
static void *st_context_init(void *data) {
    struct { void (*fn)(void *); uintptr_t counter; char ctx[]; } *d = data;
    if (d->counter != g_ctx_counter) { d->fn(d->ctx); d->counter = g_ctx_counter; }
    return d->ctx;
}

// ---- callbacks ---------------------------------------------------------------------------
// CCallbackBase: vtable; uint8 m_nCallbackFlags @8; int m_iCallback @12.
// vtable: Run(void *), Run(void *, bool, SteamAPICall_t), GetCallbackSizeBytes().
typedef struct { void **vt; uint8_t flags; uint8_t pad[3]; int id; } ccb;
static ccb *g_cbs[512];
static int g_ncbs;
typedef struct { ccb *cb; uint64_t call; } cres;
static cres g_cres[256];
static int g_ncres;
typedef struct { int id; uint64_t call; size_t n; void *data; } posted;
static posted g_posted[256];
static int g_nposted;
static uint64_t g_next_call = 1;
static pthread_mutex_t g_cbmu = PTHREAD_MUTEX_INITIALIZER;

static void st_register_cb(ccb *cb, int id) {
    pthread_mutex_lock(&g_cbmu);
    cb->flags |= 1;
    cb->id = id;
    if (g_ncbs < 512) g_cbs[g_ncbs++] = cb;
    pthread_mutex_unlock(&g_cbmu);
    if (kl_env_on("KL_TRACE_STEAM", 0))
        fprintf(stderr, "  [steam] RegisterCallback(%p, %d)\n", (void *)cb, id);
}
static void st_unregister_cb(ccb *cb) {
    pthread_mutex_lock(&g_cbmu);
    for (int i = 0; i < g_ncbs; i++) if (g_cbs[i] == cb) g_cbs[i--] = g_cbs[--g_ncbs];
    cb->flags &= (uint8_t)~1;
    pthread_mutex_unlock(&g_cbmu);
}
static void st_register_result(ccb *cb, uint64_t call) {
    pthread_mutex_lock(&g_cbmu);
    if (g_ncres < 256) g_cres[g_ncres++] = (cres){ cb, call };
    pthread_mutex_unlock(&g_cbmu);
}
static void st_unregister_result(ccb *cb, uint64_t call) {
    pthread_mutex_lock(&g_cbmu);
    for (int i = 0; i < g_ncres; i++)
        if (g_cres[i].cb == cb && g_cres[i].call == call) g_cres[i--] = g_cres[--g_ncres];
    pthread_mutex_unlock(&g_cbmu);
}
// Queue a callback (call == 0) or a call result, delivered by the next pump.
static uint64_t post(int id, const void *data, size_t n, int as_call_result) {
    pthread_mutex_lock(&g_cbmu);
    uint64_t call = as_call_result ? g_next_call++ : 0;
    if (g_nposted < 256) {
        void *c = malloc(n);
        memcpy(c, data, n);
        g_posted[g_nposted++] = (posted){ id, call, n, c };
    }
    pthread_mutex_unlock(&g_cbmu);
    return call;
}

static void net_dispatch(void);
static void st_run_callbacks(void) {
    net_dispatch();
    for (;;) {
        pthread_mutex_lock(&g_cbmu);
        if (!g_nposted) { pthread_mutex_unlock(&g_cbmu); break; }
        posted p = g_posted[0];
        memmove(g_posted, g_posted + 1, (size_t)--g_nposted * sizeof *g_posted);
        ccb *targets[64];
        int nt = 0;
        if (!p.call) {
            for (int i = 0; i < g_ncbs && nt < 64; i++)
                if (g_cbs[i]->id == p.id) targets[nt++] = g_cbs[i];
        } else {
            for (int i = 0; i < g_ncres && nt < 64; i++)
                if (g_cres[i].call == p.call) { targets[nt++] = g_cres[i].cb; g_cres[i--] = g_cres[--g_ncres]; }
        }
        pthread_mutex_unlock(&g_cbmu);
        for (int i = 0; i < nt; i++) {
            if (!p.call) ((void (*)(ccb *, void *))targets[i]->vt[0])(targets[i], p.data);
            else ((void (*)(ccb *, void *, int, uint64_t))targets[i]->vt[1])(targets[i], p.data, 0, p.call);
        }
        free(p.data);
    }
}

// ---- ISteamClient ------------------------------------------------------------------------
static int   cl_pipe(void *s) { (void)s; return 1; }
static int   cl_true(void *s) { (void)s; return 1; }
static int   cl_user(void *s, int pipe) { (void)s; (void)pipe; return 1; }
static void *cl_get3(void *s, int u, int p, const char *ver) { (void)s; (void)u; (void)p; return iface(ver); }
static void *cl_get2(void *s, int p, const char *ver) { (void)s; (void)p; return iface(ver); }

// ---- ISteamUser / ISteamGameServer ------------------------------------------------------------
static int      us_hsteamuser(void *s) { (void)s; return 1; }
static int      us_true(void *s) { (void)s; return 1; }
static int      us_false(void *s) { (void)s; return 0; }
static uint64_t us_steamid(void *s) { (void)s; return USER_ID; }
static uint64_t gs_steamid(void *s) { (void)s; return SERVER_ID; }
static int us_userdata_folder(void *s, char *buf, int n) {
    (void)s;
    const char *home = getenv("HOME");
    snprintf(buf, (size_t)n, "%s/Library/Application Support/Klepton/userdata/alyx/steam",
             home ? home : "/tmp");
    return 1;
}
static int us_level(void *s) { (void)s; return 1; }
static int us_license(void *s, uint64_t id, uint32_t app) { (void)s; (void)id; return app == APP_ID ? 0 : 2; }
static uint32_t us_ticket(void *s, void *buf, int cb, uint32_t *out, const void *ident) {
    (void)s; (void)ident;
    if (buf && cb >= 8) { memset(buf, 0x4b, 8); if (out) *out = 8; }
    return 1;                                    // HAuthTicket
}
static int us_begin_auth(void *s, const void *t, int cb, uint64_t id) { (void)s; (void)t; (void)cb; (void)id; return 0; }

// ---- ISteamUtils -----------------------------------------------------------------------------------
static uint32_t ut_appid(void *s) { (void)s; return APP_ID; }
static uint32_t ut_time(void *s) { (void)s; return (uint32_t)time(NULL); }
static const char *ut_country(void *s) { (void)s; return "US"; }
// The game language: KL_STEAM_LANG (a Steam language API name: english,
// spanish, latam, french, german, ...) or else the system's first preferred
// language. Alyx ships English voices only; this picks UI text and subtitles.
static const char *steam_lang(void) {
    static char lang[32];
    if (lang[0]) return lang;
    const char *e = getenv("KL_STEAM_LANG");
    if (e && *e) { snprintf(lang, sizeof lang, "%s", e); return lang; }
    char code[64] = "en";
    CFArrayRef pref = CFLocaleCopyPreferredLanguages();
    if (pref) {
        if (CFArrayGetCount(pref) > 0)
            CFStringGetCString(CFArrayGetValueAtIndex(pref, 0), code, sizeof code, kCFStringEncodingUTF8);
        CFRelease(pref);
    }
    static const struct { const char *pfx, *name; } map[] = {
        {"es-ES", "spanish"}, {"es-", "latam"}, {"es", "spanish"},
        {"pt-BR", "brazilian"}, {"pt", "portuguese"}, {"zh-Hant", "tchinese"}, {"zh", "schinese"},
        {"fr", "french"}, {"de", "german"}, {"it", "italian"}, {"ja", "japanese"}, {"ko", "koreana"},
        {"ru", "russian"}, {"pl", "polish"}, {"nl", "dutch"}, {"sv", "swedish"}, {"da", "danish"},
        {"fi", "finnish"}, {"nb", "norwegian"}, {"no", "norwegian"}, {"cs", "czech"}, {"hu", "hungarian"},
        {"tr", "turkish"}, {"uk", "ukrainian"}, {"th", "thai"}, {"el", "greek"}, {"bg", "bulgarian"},
        {"ro", "romanian"}, {"vi", "vietnamese"},
    };
    const char *name = "english";
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
        if (strncmp(code, map[i].pfx, strlen(map[i].pfx)) == 0) { name = map[i].name; break; }
    snprintf(lang, sizeof lang, "%s", name);
    fprintf(stderr, "  [steam] game language: %s (system %s)\n", lang, code);
    return lang;
}
static const char *ut_lang(void *s) { (void)s; return steam_lang(); }
static int ut_universe(void *s) { (void)s; return 1; }
static uint8_t ut_battery(void *s) { (void)s; return 255; }
static int ut_call_done(void *s, uint64_t call, int *failed) {
    (void)s;
    pthread_mutex_lock(&g_cbmu);
    int done = 0;
    for (int i = 0; i < g_nposted; i++) if (g_posted[i].call == call) done = 1;
    pthread_mutex_unlock(&g_cbmu);
    if (failed) *failed = 0;
    return done;
}
static int ut_call_result(void *s, uint64_t call, void *out, int cb, int expected, int *failed) {
    (void)s; (void)expected;
    int got = 0;
    pthread_mutex_lock(&g_cbmu);
    for (int i = 0; i < g_nposted; i++)
        if (g_posted[i].call == call) {
            memcpy(out, g_posted[i].data, (size_t)cb < g_posted[i].n ? (size_t)cb : g_posted[i].n);
            free(g_posted[i].data);
            g_posted[i] = g_posted[--g_nposted];
            got = 1;
            break;
        }
    pthread_mutex_unlock(&g_cbmu);
    if (failed) *failed = !got;
    return got;
}

// ---- ISteamApps ---------------------------------------------------------------------------------------
static int ap_true(void *s) { (void)s; return 1; }
static int ap_false(void *s) { (void)s; return 0; }
static int ap_subscribed_app(void *s, uint32_t app) { (void)s; return app == APP_ID; }
static int ap_installed(void *s, uint32_t app) { (void)s; return app == APP_ID; }
static const char *ap_lang(void *s) { (void)s; return steam_lang(); }
static uint32_t ap_purchase(void *s, uint32_t app) { (void)s; (void)app; return 1600000000u; }
static int ap_beta(void *s, char *buf, int n) { (void)s; if (buf && n) buf[0] = 0; return 0; }
static uint32_t ap_install_dir(void *s, uint32_t app, char *buf, uint32_t n) {
    (void)s; (void)app;
    char cwd[1024];
    if (!getcwd(cwd, sizeof cwd)) cwd[0] = 0;   // the door chdir'd into the game root
    snprintf(buf, n, "%s", cwd);
    return (uint32_t)strlen(buf);
}
static uint64_t ap_owner(void *s) { (void)s; return USER_ID; }
static const char *ap_empty(void *s, const char *k) { (void)s; (void)k; return ""; }
static int ap_build(void *s) { (void)s; return 1; }
static int ap_cmdline(void *s, char *buf, int n) { (void)s; if (buf && n) buf[0] = 0; return 0; }

// ---- ISteamFriends ------------------------------------------------------------------------------------
static const char *fr_name(void *s) { (void)s; return kl_env_str("KL_STEAM_NAME", "Gordon"); }
static int fr_state(void *s) { (void)s; return 1; }                       // online
static const char *fr_friend_name(void *s, uint64_t id) { (void)s; (void)id; return kl_env_str("KL_STEAM_NAME", "Gordon"); }
static int fr_zero(void *s) { (void)s; return 0; }

// ---- ISteamNetworking (the old P2P API): only its packet size is asked ----
static uint32_t nw_max_packet(void *s, uint32_t sock) { (void)s; (void)sock; return 1200; }

// ---- ISteamNetworkingSockets: loopback ---------------------------------------------------------------
#define NS_MAXCONN 256
#define NS_MAXLISTEN 32
#define CS_NONE 0
#define CS_CONNECTING 1
#define CS_CONNECTED 3
#define CS_CLOSEDBYPEER 4
#define CS_PROBLEM 5
#define EK_OK 1
#define EK_NOCONN 3
#define EK_INVALIDPARAM 8

typedef void (*status_fn)(void *cb);
typedef struct msg {
    // SteamNetworkingMessage_t, 216 bytes; offsets per steamnetworkingtypes.h
    // (aarch64 Linux): data 0, size 8, conn 12, identity 16 (136), connuserdata
    // 152, time 160, number 168, freedata 176, release 184, channel 192,
    // flags 196, userdata 200, lane 208.
    uint8_t raw[216];
    struct msg *next;
} msg;
typedef struct {
    int used, state, peer, listen, poll;
    int64_t userdata;
    char name[128];
    status_fn cb;
    uint16_t port;
    int64_t nextnum;
    msg *head, *tail;
} nconn;
typedef struct { int used; uint16_t port; int p2p; status_fn cb; } nlisten;
static nconn g_conn[NS_MAXCONN];
static nlisten g_listen[NS_MAXLISTEN];
static int g_npoll;
static status_fn g_global_status_cb;
static pthread_mutex_t g_nmu = PTHREAD_MUTEX_INITIALIZER;
// Pending status changes: (conn, old state), turned into callbacks by net_dispatch.
static struct { int conn, old; } g_ns_q[512];
static int g_ns_nq;

#define H_CONN(i)   ((uint32_t)(0x10000 + (i)))
#define H_LISTEN(i) ((uint32_t)(0x20000 + (i)))
#define H_POLL(i)   ((uint32_t)(0x30000 + (i)))
static nconn *conn_of(uint32_t h) {
    uint32_t i = h - 0x10000;
    return (h >= 0x10000 && i < NS_MAXCONN && g_conn[i].used) ? &g_conn[i] : NULL;
}

static int64_t now_us(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

static void set_state(int ci, int st) {          // caller holds g_nmu
    int old = g_conn[ci].state;
    if (old == st) return;
    g_conn[ci].state = st;
    if (g_ns_nq < 512) { g_ns_q[g_ns_nq].conn = ci; g_ns_q[g_ns_nq].old = old; g_ns_nq++; }
}

static status_fn opts_status_cb(int n, const uint8_t *opts) {
    // SteamNetworkingConfigValue_t: { int m_eValue; int m_eDataType; union m_val @8 } = 16 bytes
    for (int i = 0; opts && i < n; i++) {
        int v; memcpy(&v, opts + i * 16, 4);
        if (v == 201) { void *p; memcpy(&p, opts + i * 16 + 8, 8); return (status_fn)p; }
    }
    return NULL;
}

static void put_identity(uint8_t *dst, uint64_t sid) {   // SteamNetworkingIdentity, 136 bytes
    memset(dst, 0, 136);
    int type = 16, size = 8;                              // k_ESteamNetworkingIdentityType_SteamID
    memcpy(dst, &type, 4); memcpy(dst + 4, &size, 4); memcpy(dst + 8, &sid, 8);
}
static void fill_info(uint8_t *info, int ci) {           // SteamNetConnectionInfo_t, 696 bytes
    nconn *c = &g_conn[ci];
    memset(info, 0, 696);
    put_identity(info, c->listen >= 0 ? USER_ID : SERVER_ID);
    memcpy(info + 136, &c->userdata, 8);
    uint32_t ls = c->listen >= 0 ? H_LISTEN(c->listen) : 0;
    memcpy(info + 144, &ls, 4);
    uint8_t addr[18] = { 0,0,0,0,0,0,0,0,0,0,0xff,0xff,127,0,0,1 };
    memcpy(addr + 16, &c->port, 2);
    memcpy(info + 148, addr, 18);
    int st = c->state;
    memcpy(info + 176, &st, 4);
    snprintf((char *)info + 312, 128, "loopback #%d%s%s", ci, c->name[0] ? " " : "", c->name);
}

static int new_conn(void) {                              // caller holds g_nmu
    for (int i = 0; i < NS_MAXCONN; i++)
        if (!g_conn[i].used) {
            memset(&g_conn[i], 0, sizeof g_conn[i]);
            g_conn[i].used = 1; g_conn[i].peer = -1; g_conn[i].listen = -1;
            g_conn[i].poll = -1; g_conn[i].userdata = -1; g_conn[i].nextnum = 1;
            return i;
        }
    return -1;
}

static void net_dispatch(void) {
    for (;;) {
        pthread_mutex_lock(&g_nmu);
        if (!g_ns_nq) { pthread_mutex_unlock(&g_nmu); return; }
        int ci = g_ns_q[0].conn, old = g_ns_q[0].old;
        memmove(g_ns_q, g_ns_q + 1, (size_t)--g_ns_nq * sizeof *g_ns_q);
        uint8_t cbdata[704];
        memset(cbdata, 0, sizeof cbdata);
        uint32_t h = H_CONN(ci);
        memcpy(cbdata, &h, 4);
        fill_info(cbdata + 4, ci);
        memcpy(cbdata + 700, &old, 4);
        status_fn fn = g_conn[ci].cb;
        if (!fn && g_conn[ci].listen >= 0) fn = g_listen[g_conn[ci].listen].cb;
        if (!fn) fn = g_global_status_cb;
        int st = g_conn[ci].state;
        pthread_mutex_unlock(&g_nmu);
        if (kl_env_on("KL_TRACE_STEAM", 0))
            fprintf(stderr, "  [steam] connection %#x: state %d -> %d (%s)\n", h, old, st,
                    fn ? "config callback" : "SteamAPI callback");
        if (fn) fn(cbdata);
        else post(1221, cbdata, sizeof cbdata, 0);
    }
}

static uint32_t ns_listen_ip(void *s, const uint8_t *addr, int n, const uint8_t *opts) {
    (void)s;
    pthread_mutex_lock(&g_nmu);
    for (int i = 0; i < NS_MAXLISTEN; i++)
        if (!g_listen[i].used) {
            g_listen[i].used = 1;
            memcpy(&g_listen[i].port, addr + 16, 2);
            g_listen[i].p2p = 0;
            g_listen[i].cb = opts_status_cb(n, opts);
            pthread_mutex_unlock(&g_nmu);
            fprintf(stderr, "  [steam] sockets: listen (IP port %u) -> %#x\n", g_listen[i].port, H_LISTEN(i));
            return H_LISTEN(i);
        }
    pthread_mutex_unlock(&g_nmu);
    return 0;
}
static uint32_t ns_listen_p2p(void *s, int vport, int n, const uint8_t *opts) {
    (void)s;
    pthread_mutex_lock(&g_nmu);
    for (int i = 0; i < NS_MAXLISTEN; i++)
        if (!g_listen[i].used) {
            g_listen[i].used = 1; g_listen[i].port = (uint16_t)vport; g_listen[i].p2p = 1;
            g_listen[i].cb = opts_status_cb(n, opts);
            pthread_mutex_unlock(&g_nmu);
            fprintf(stderr, "  [steam] sockets: listen (P2P vport %d) -> %#x\n", vport, H_LISTEN(i));
            return H_LISTEN(i);
        }
    pthread_mutex_unlock(&g_nmu);
    return 0;
}
static uint32_t connect_to(uint16_t port, int p2p, int n, const uint8_t *opts) {
    pthread_mutex_lock(&g_nmu);
    int li = -1;
    for (int i = 0; i < NS_MAXLISTEN; i++)
        if (g_listen[i].used && g_listen[i].p2p == p2p && (g_listen[i].port == port || !port)) { li = i; break; }
    if (li < 0)                                          // any listener: the loopback has one server
        for (int i = 0; i < NS_MAXLISTEN; i++) if (g_listen[i].used) { li = i; break; }
    int c = new_conn();
    if (c < 0) { pthread_mutex_unlock(&g_nmu); return 0; }
    g_conn[c].cb = opts_status_cb(n, opts);
    g_conn[c].port = port;
    set_state(c, CS_CONNECTING);
    if (li >= 0) {
        int sv = new_conn();
        g_conn[sv].listen = li;
        g_conn[sv].port = port;
        g_conn[sv].peer = c;
        g_conn[c].peer = sv;
        set_state(sv, CS_CONNECTING);                    // the server app must Accept it
    } else {
        set_state(c, CS_PROBLEM);
    }
    pthread_mutex_unlock(&g_nmu);
    fprintf(stderr, "  [steam] sockets: connect (%s %u) -> %#x%s\n", p2p ? "P2P vport" : "port",
            port, H_CONN(c), li < 0 ? " — nothing listening" : "");
    return H_CONN(c);
}
static uint32_t ns_connect_ip(void *s, const uint8_t *addr, int n, const uint8_t *opts) {
    (void)s; uint16_t port; memcpy(&port, addr + 16, 2);
    return connect_to(port, 0, n, opts);
}
static uint32_t ns_connect_p2p(void *s, const void *ident, int vport, int n, const uint8_t *opts) {
    (void)s; (void)ident;
    return connect_to((uint16_t)vport, 1, n, opts);
}
static int ns_accept(void *s, uint32_t h) {
    (void)s;
    pthread_mutex_lock(&g_nmu);
    nconn *c = conn_of(h);
    int r = EK_INVALIDPARAM;
    if (c && c->state == CS_CONNECTING) {
        int ci = (int)(c - g_conn);
        set_state(ci, CS_CONNECTED);
        if (c->peer >= 0 && g_conn[c->peer].used) set_state(c->peer, CS_CONNECTED);
        r = EK_OK;
    }
    pthread_mutex_unlock(&g_nmu);
    return r;
}
static void free_msgs(nconn *c) {
    for (msg *m = c->head; m; ) { msg *n = m->next; void *d; memcpy(&d, m->raw, 8); free(d); free(m); m = n; }
    c->head = c->tail = NULL;
}
static int ns_close(void *s, uint32_t h, int reason, const char *dbg, int linger) {
    (void)s; (void)reason; (void)linger;
    pthread_mutex_lock(&g_nmu);
    nconn *c = conn_of(h);
    if (!c) { pthread_mutex_unlock(&g_nmu); return 0; }
    if (c->peer >= 0 && g_conn[c->peer].used) {
        g_conn[c->peer].peer = -1;
        if (g_conn[c->peer].state == CS_CONNECTED || g_conn[c->peer].state == CS_CONNECTING)
            set_state(c->peer, CS_CLOSEDBYPEER);
    }
    free_msgs(c);
    // A closed handle never reports again; drop any queued change for it.
    int ci = (int)(c - g_conn);
    for (int i = 0; i < g_ns_nq; i++) if (g_ns_q[i].conn == ci) g_ns_q[i--] = g_ns_q[--g_ns_nq];
    c->used = 0;
    pthread_mutex_unlock(&g_nmu);
    if (kl_env_on("KL_TRACE_STEAM", 0))
        fprintf(stderr, "  [steam] sockets: close %#x (%s)\n", h, dbg ? dbg : "");
    return 1;
}
static int ns_close_listen(void *s, uint32_t h) {
    (void)s; uint32_t i = h - 0x20000;
    if (i >= NS_MAXLISTEN) return 0;
    pthread_mutex_lock(&g_nmu); g_listen[i].used = 0; pthread_mutex_unlock(&g_nmu);
    return 1;
}
static int ns_set_ud(void *s, uint32_t h, int64_t ud) {
    (void)s; pthread_mutex_lock(&g_nmu); nconn *c = conn_of(h); if (c) c->userdata = ud;
    pthread_mutex_unlock(&g_nmu); return c != NULL;
}
static int64_t ns_get_ud(void *s, uint32_t h) {
    (void)s; pthread_mutex_lock(&g_nmu); nconn *c = conn_of(h); int64_t v = c ? c->userdata : -1;
    pthread_mutex_unlock(&g_nmu); return v;
}
static void ns_set_name(void *s, uint32_t h, const char *n) {
    (void)s; pthread_mutex_lock(&g_nmu); nconn *c = conn_of(h);
    if (c) snprintf(c->name, sizeof c->name, "%s", n ? n : ""); pthread_mutex_unlock(&g_nmu);
}
static int ns_get_name(void *s, uint32_t h, char *buf, int n) {
    (void)s; pthread_mutex_lock(&g_nmu); nconn *c = conn_of(h);
    if (c && buf && n) snprintf(buf, (size_t)n, "%s", c->name); pthread_mutex_unlock(&g_nmu);
    return c != NULL;
}

static void msg_release(void *m) {
    void *d; memcpy(&d, (uint8_t *)m, 8);
    void (*fd)(void *); memcpy(&fd, (uint8_t *)m + 176, 8);
    if (fd) fd(m); else free(d);
    free(m);
}
static void msg_free_data(void *m) { void *d; memcpy(&d, (uint8_t *)m, 8); free(d); }

// Copy `data` into a message on `to`'s receive queue. Caller holds g_nmu.
static int deliver(int from, const void *data, uint32_t n, int flags, int64_t *num) {
    nconn *src = &g_conn[from];
    if (src->state != CS_CONNECTED || src->peer < 0 || !g_conn[src->peer].used) return EK_NOCONN;
    nconn *dst = &g_conn[src->peer];
    msg *m = calloc(1, sizeof *m);
    void *copy = malloc(n ? n : 1);
    memcpy(copy, data, n);
    uint8_t *r = m->raw;
    int sz = (int)n;
    uint32_t h = H_CONN(src->peer);
    int64_t t = now_us(), nn = src->nextnum++;
    void *rel = (void *)msg_release, *fd = (void *)msg_free_data;
    memcpy(r, &copy, 8); memcpy(r + 8, &sz, 4); memcpy(r + 12, &h, 4);
    put_identity(r + 16, src->listen >= 0 ? USER_ID : SERVER_ID);   // who sent it
    memcpy(r + 152, &dst->userdata, 8); memcpy(r + 160, &t, 8); memcpy(r + 168, &nn, 8);
    memcpy(r + 176, &fd, 8); memcpy(r + 184, &rel, 8); memcpy(r + 196, &flags, 4);
    if (dst->tail) dst->tail->next = m; else dst->head = m;
    dst->tail = m;
    if (num) *num = nn;
    return EK_OK;
}
static int ns_send(void *s, uint32_t h, const void *data, uint32_t n, int flags, int64_t *num) {
    (void)s;
    pthread_mutex_lock(&g_nmu);
    nconn *c = conn_of(h);
    int r = c ? deliver((int)(c - g_conn), data, n, flags, num) : EK_INVALIDPARAM;
    pthread_mutex_unlock(&g_nmu);
    return r;
}
static void ns_send_many(void *s, int n, void *const *msgs, int64_t *out) {
    (void)s;
    for (int i = 0; i < n; i++) {
        uint8_t *m = msgs[i];
        void *data; int sz, flags; uint32_t h;
        memcpy(&data, m, 8); memcpy(&sz, m + 8, 4); memcpy(&h, m + 12, 4); memcpy(&flags, m + 196, 4);
        int64_t num = 0;
        pthread_mutex_lock(&g_nmu);
        nconn *c = conn_of(h);
        int r = c ? deliver((int)(c - g_conn), data, (uint32_t)sz, flags, &num) : EK_INVALIDPARAM;
        pthread_mutex_unlock(&g_nmu);
        if (out) out[i] = r == EK_OK ? num : -r;
        void (*rel)(void *); memcpy(&rel, m + 184, 8);
        if (rel) rel(m);
    }
}
static int ns_flush(void *s, uint32_t h) { (void)s; (void)h; return EK_OK; }
static int take(nconn *c, void **out, int max) {          // caller holds g_nmu
    int k = 0;
    while (c->head && k < max) {
        msg *m = c->head;
        c->head = m->next;
        if (!c->head) c->tail = NULL;
        // Hand the guest the 216-byte struct itself; msg_release frees it, and
        // `raw` is the first member so the two pointers are the same.
        out[k++] = m;
    }
    return k;
}
static int ns_recv(void *s, uint32_t h, void **out, int max) {
    (void)s;
    pthread_mutex_lock(&g_nmu);
    nconn *c = conn_of(h);
    int k = c ? take(c, out, max) : -1;
    pthread_mutex_unlock(&g_nmu);
    return k;
}
static int ns_info(void *s, uint32_t h, uint8_t *info) {
    (void)s;
    pthread_mutex_lock(&g_nmu);
    nconn *c = conn_of(h);
    if (c && info) fill_info(info, (int)(c - g_conn));
    pthread_mutex_unlock(&g_nmu);
    return c != NULL;
}
static int ns_status(void *s, uint32_t h, uint8_t *st, int nl, void *lanes) {
    (void)s; (void)nl; (void)lanes;
    pthread_mutex_lock(&g_nmu);
    nconn *c = conn_of(h);
    if (c && st) {
        memset(st, 0, 120);
        int state = c->state; float q = 1.0f; int rate = 1 << 30;
        memcpy(st, &state, 4); memcpy(st + 8, &q, 4); memcpy(st + 12, &q, 4); memcpy(st + 32, &rate, 4);
    }
    pthread_mutex_unlock(&g_nmu);
    return c ? EK_OK : EK_NOCONN;
}
static int ns_detailed(void *s, uint32_t h, char *buf, int n) {
    (void)s; (void)h; if (buf && n) snprintf(buf, (size_t)n, "loopback"); return 0;
}
static int ns_listen_addr(void *s, uint32_t h, uint8_t *addr) {
    (void)s; uint32_t i = h - 0x20000;
    if (i >= NS_MAXLISTEN || !g_listen[i].used || !addr) return 0;
    uint8_t a[18] = { 0,0,0,0,0,0,0,0,0,0,0xff,0xff,127,0,0,1 };
    memcpy(a + 16, &g_listen[i].port, 2);
    memcpy(addr, a, 18);
    return 1;
}
static int ns_pair(void *s, uint32_t *a, uint32_t *b, int loop, const void *i1, const void *i2) {
    (void)s; (void)loop; (void)i1; (void)i2;
    pthread_mutex_lock(&g_nmu);
    int x = new_conn(), y = new_conn();
    g_conn[x].peer = y; g_conn[y].peer = x;
    g_conn[x].state = g_conn[y].state = CS_CONNECTED;     // born connected: no callbacks
    pthread_mutex_unlock(&g_nmu);
    if (a) *a = H_CONN(x);
    if (b) *b = H_CONN(y);
    return 1;
}
static int ns_lanes(void *s, uint32_t h, int n, const int *p, const uint16_t *w) {
    (void)s; (void)h; (void)n; (void)p; (void)w; return EK_OK;
}
static int ns_identity(void *s, uint8_t *id) { (void)s; if (id) put_identity(id, USER_ID); return 1; }
static int ns_avail(void *s) { (void)s; return 100; }   // k_ESteamNetworkingAvailability_Current
static int ns_auth_status(void *s, uint8_t *st) {
    (void)s; if (st) { memset(st, 0, 260); int a = 100; memcpy(st, &a, 4); } return 100;
}
static uint32_t ns_poll_create(void *s) { (void)s; return H_POLL(++g_npoll); }
static int ns_poll_destroy(void *s, uint32_t h) {
    (void)s;
    pthread_mutex_lock(&g_nmu);
    for (int i = 0; i < NS_MAXCONN; i++) if (g_conn[i].used && g_conn[i].poll == (int)h) g_conn[i].poll = -1;
    pthread_mutex_unlock(&g_nmu);
    return 1;
}
static int ns_set_poll(void *s, uint32_t hc, uint32_t hp) {
    (void)s; pthread_mutex_lock(&g_nmu); nconn *c = conn_of(hc); if (c) c->poll = hp ? (int)hp : -1;
    pthread_mutex_unlock(&g_nmu); return c != NULL;
}
static int ns_recv_poll(void *s, uint32_t hp, void **out, int max) {
    (void)s;
    int k = 0;
    pthread_mutex_lock(&g_nmu);
    for (int i = 0; i < NS_MAXCONN && k < max; i++)
        if (g_conn[i].used && g_conn[i].poll == (int)hp) k += take(&g_conn[i], out + k, max - k);
    pthread_mutex_unlock(&g_nmu);
    return k;
}
static void ns_run_callbacks(void *s) { (void)s; net_dispatch(); }

// ---- ISteamNetworkingUtils -------------------------------------------------------------------------
static void *nu_alloc(void *s, int cb) {
    (void)s;
    msg *m = calloc(1, sizeof *m);
    void *d = cb > 0 ? malloc((size_t)cb) : NULL;
    void *rel = (void *)msg_release, *fd = (void *)msg_free_data;
    memcpy(m->raw, &d, 8); memcpy(m->raw + 8, &cb, 4);
    memcpy(m->raw + 176, &fd, 8); memcpy(m->raw + 184, &rel, 8);
    return m;
}
static int nu_relay(void *s, uint8_t *st) {
    (void)s; if (st) { memset(st, 0, 272); int a = 100; memcpy(st, &a, 4); memcpy(st + 8, &a, 4); memcpy(st + 12, &a, 4); }
    return 100;
}
static int nu_true(void *s) { (void)s; return 1; }
static int64_t nu_time(void *s) { (void)s; return now_us(); }
static void nu_debug(void *s, int lvl, void *fn) { (void)s; (void)lvl; (void)fn; }
static int nu_set_config(void *s, int value, int scope, intptr_t obj, int type, const void *arg) {
    (void)s; (void)type;
    if (value == 201 && arg) {                       // Callback_ConnectionStatusChanged
        status_fn fn; memcpy(&fn, arg, 8);
        pthread_mutex_lock(&g_nmu);
        if (scope == 1 || scope == 2) g_global_status_cb = fn;
        else if (scope == 3 && (uint32_t)obj - 0x20000 < NS_MAXLISTEN) g_listen[(uint32_t)obj - 0x20000].cb = fn;
        else if (scope == 4) { nconn *c = conn_of((uint32_t)obj); if (c) c->cb = fn; }
        pthread_mutex_unlock(&g_nmu);
    }
    return 1;
}
static int nu_get_config(void *s, int value, int scope, intptr_t obj, int *type, void *out, size_t *cb) {
    (void)s; (void)value; (void)scope; (void)obj; (void)type; (void)out; (void)cb;
    return -1;                                        // k_ESteamNetworkingGetConfigValue_BadValue
}
static const char *nu_config_info(void *s, int value, int *type, int *scope) {
    (void)s; (void)value; (void)type; (void)scope; return NULL;
}
static int nu_iterate(void *s, int cur, int dev) { (void)s; (void)cur; (void)dev; return 0; }
static void nu_addr_tostr(void *s, const uint8_t *a, char *buf, size_t n, int port) {
    (void)s;
    uint16_t p; memcpy(&p, a + 16, 2);
    if (port) snprintf(buf, n, "%u.%u.%u.%u:%u", a[12], a[13], a[14], a[15], p);
    else snprintf(buf, n, "%u.%u.%u.%u", a[12], a[13], a[14], a[15]);
}
static int nu_addr_parse(void *s, uint8_t *a, const char *str) {
    (void)s;
    unsigned x[4] = {0}, p = 0;
    if (!str || sscanf(str, "%u.%u.%u.%u:%u", &x[0], &x[1], &x[2], &x[3], &p) < 4) return 0;
    memset(a, 0, 18); a[10] = a[11] = 0xff;
    for (int i = 0; i < 4; i++) a[12 + i] = (uint8_t)x[i];
    uint16_t pp = (uint16_t)p; memcpy(a + 16, &pp, 2);
    return 1;
}
static int nu_zero(void *s) { (void)s; return 0; }
static void nu_id_tostr(void *s, const uint8_t *id, char *buf, size_t n) {
    (void)s; uint64_t sid; memcpy(&sid, id + 8, 8); snprintf(buf, n, "steamid:%llu", (unsigned long long)sid);
}
static int nu_id_parse(void *s, uint8_t *id, const char *str) {
    (void)s; unsigned long long sid = 0;
    if (str && sscanf(str, "steamid:%llu", &sid) == 1) { put_identity(id, sid); return 1; }
    return 0;
}

// ---- the implemented slots -----------------------------------------------------------------------------
#define I(ver, m, f) { ver, m, 0, (void *)(f) }
#define I2(ver, m, n, f) { ver, m, n, (void *)(f) }
#define CLV "SteamClient023"
#define USV "SteamUser023"
#define GSV "SteamGameServer015"
#define UTV "SteamUtils011"
#define APV "STEAMAPPS_INTERFACE_VERSION009"
#define FRV "SteamFriends018"
#define NSV "SteamNetworkingSockets013"
#define NUV "SteamNetworkingUtils004"
static const impl g_impl[] = {
    I(CLV, "CreateSteamPipe", cl_pipe), I(CLV, "BReleaseSteamPipe", cl_true),
    I(CLV, "ConnectToGlobalUser", cl_user), I(CLV, "BShutdownIfAllPipesClosed", cl_true),
    I(CLV, "GetISteamUser", cl_get3), I(CLV, "GetISteamGameServer", cl_get3),
    I(CLV, "GetISteamFriends", cl_get3), I(CLV, "GetISteamUtils", cl_get2),
    I(CLV, "GetISteamMatchmaking", cl_get3), I(CLV, "GetISteamMatchmakingServers", cl_get3),
    I(CLV, "GetISteamGenericInterface", cl_get3), I(CLV, "GetISteamUserStats", cl_get3),
    I(CLV, "GetISteamGameServerStats", cl_get3), I(CLV, "GetISteamApps", cl_get3),
    I(CLV, "GetISteamNetworking", cl_get3), I(CLV, "GetISteamRemoteStorage", cl_get3),
    I(CLV, "GetISteamScreenshots", cl_get3), I(CLV, "GetISteamHTTP", cl_get3),
    I(CLV, "GetISteamController", cl_get3), I(CLV, "GetISteamUGC", cl_get3),
    I(CLV, "GetISteamMusic", cl_get3), I(CLV, "GetISteamHTMLSurface", cl_get3),
    I(CLV, "GetISteamInventory", cl_get3), I(CLV, "GetISteamVideo", cl_get3),
    I(CLV, "GetISteamParentalSettings", cl_get3), I(CLV, "GetISteamInput", cl_get3),
    I(CLV, "GetISteamParties", cl_get3), I(CLV, "GetISteamRemotePlay", cl_get3),

    I(USV, "GetHSteamUser", us_hsteamuser), I(USV, "BLoggedOn", us_true),
    I(USV, "GetSteamID", us_steamid), I(USV, "GetUserDataFolder", us_userdata_folder),
    I(USV, "GetPlayerSteamLevel", us_level), I(USV, "GetGameBadgeLevel", us_false),
    I(USV, "UserHasLicenseForApp", us_license), I(USV, "BIsBehindNAT", us_false),
    I(USV, "GetAuthSessionTicket", us_ticket), I(USV, "BeginAuthSession", us_begin_auth),
    I(USV, "BIsPhoneVerified", us_true), I(USV, "BIsTwoFactorEnabled", us_true),

    I(GSV, "BLoggedOn", us_true), I(GSV, "BSecure", us_false), I(GSV, "GetSteamID", gs_steamid),
    I(GSV, "InitGameServer", us_true), I(GSV, "WasRestartRequested", us_false),
    I(GSV, "GetAuthSessionTicket", us_ticket), I(GSV, "BeginAuthSession", us_begin_auth),
    I(GSV, "UserHasLicenseForApp", us_license), I(GSV, "BUpdateUserData", us_true),

    I(UTV, "GetAppID", ut_appid), I(UTV, "GetServerRealTime", ut_time),
    I(UTV, "GetIPCountry", ut_country), I(UTV, "GetSteamUILanguage", ut_lang),
    I(UTV, "GetConnectedUniverse", ut_universe), I(UTV, "GetCurrentBatteryPower", ut_battery),
    I(UTV, "IsAPICallCompleted", ut_call_done), I(UTV, "GetAPICallResult", ut_call_result),

    I(APV, "BIsSubscribed", ap_true), I(APV, "BIsLowViolence", ap_false),
    I(APV, "BIsCybercafe", ap_false), I(APV, "BIsVACBanned", ap_false),
    I(APV, "GetCurrentGameLanguage", ap_lang), I(APV, "GetAvailableGameLanguages", ap_lang),
    I(APV, "BIsSubscribedApp", ap_subscribed_app), I(APV, "BIsAppInstalled", ap_installed),
    I(APV, "GetEarliestPurchaseUnixTime", ap_purchase), I(APV, "GetCurrentBetaName", ap_beta),
    I(APV, "GetAppInstallDir", ap_install_dir), I(APV, "GetAppOwner", ap_owner),
    I(APV, "GetLaunchQueryParam", ap_empty), I(APV, "GetAppBuildId", ap_build),
    I(APV, "GetLaunchCommandLine", ap_cmdline),

    I("SteamNetworking006", "GetMaxPacketSize", nw_max_packet),
    I(FRV, "GetPersonaName", fr_name), I(FRV, "GetPersonaState", fr_state),
    I(FRV, "GetFriendPersonaName", fr_friend_name), I(FRV, "GetFriendCount", fr_zero),

    I(NSV, "CreateListenSocketIP", ns_listen_ip), I(NSV, "ConnectByIPAddress", ns_connect_ip),
    I(NSV, "CreateListenSocketP2P", ns_listen_p2p), I(NSV, "ConnectP2P", ns_connect_p2p),
    I(NSV, "AcceptConnection", ns_accept), I(NSV, "CloseConnection", ns_close),
    I(NSV, "CloseListenSocket", ns_close_listen),
    I(NSV, "SetConnectionUserData", ns_set_ud), I(NSV, "GetConnectionUserData", ns_get_ud),
    I(NSV, "SetConnectionName", ns_set_name), I(NSV, "GetConnectionName", ns_get_name),
    I(NSV, "SendMessageToConnection", ns_send), I(NSV, "SendMessages", ns_send_many),
    I(NSV, "FlushMessagesOnConnection", ns_flush),
    I(NSV, "ReceiveMessagesOnConnection", ns_recv), I(NSV, "GetConnectionInfo", ns_info),
    I(NSV, "GetConnectionRealTimeStatus", ns_status),
    I(NSV, "GetDetailedConnectionStatus", ns_detailed),
    I(NSV, "GetListenSocketAddress", ns_listen_addr), I(NSV, "CreateSocketPair", ns_pair),
    I(NSV, "ConfigureConnectionLanes", ns_lanes), I(NSV, "GetIdentity", ns_identity),
    I(NSV, "InitAuthentication", ns_avail), I(NSV, "GetAuthenticationStatus", ns_auth_status),
    I(NSV, "CreatePollGroup", ns_poll_create), I(NSV, "DestroyPollGroup", ns_poll_destroy),
    I(NSV, "SetConnectionPollGroup", ns_set_poll),
    I(NSV, "ReceiveMessagesOnPollGroup", ns_recv_poll), I(NSV, "RunCallbacks", ns_run_callbacks),

    I(NUV, "AllocateMessage", nu_alloc), I(NUV, "GetRelayNetworkStatus", nu_relay),
    I(NUV, "CheckPingDataUpToDate", nu_true), I(NUV, "GetLocalTimestamp", nu_time),
    I(NUV, "SetDebugOutputFunction", nu_debug), I(NUV, "SetConfigValue", nu_set_config),
    I(NUV, "GetConfigValue", nu_get_config), I(NUV, "GetConfigValueInfo", nu_config_info),
    I(NUV, "IterateGenericEditableConfigValues", nu_iterate),
    I(NUV, "SteamNetworkingIPAddr_ToString", nu_addr_tostr),
    I(NUV, "SteamNetworkingIPAddr_ParseString", nu_addr_parse),
    I(NUV, "SteamNetworkingIPAddr_GetFakeIPType", nu_zero),
    I(NUV, "SteamNetworkingIdentity_ToString", nu_id_tostr),
    I(NUV, "SteamNetworkingIdentity_ParseString", nu_id_parse),
};
static const int g_nimpl = (int)(sizeof g_impl / sizeof *g_impl);

// ---- interface construction ------------------------------------------------------------
static long steam_unimpl(const char *name) {
    static struct { const char *n; unsigned c; } seen[512];
    static unsigned ns;
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&mu);
    unsigned i = 0;
    for (; i < ns; i++) if (seen[i].n == name) break;
    if (i == ns && ns < 512) { seen[ns].n = name; seen[ns].c = 0; ns++; }
    unsigned c = i < 512 ? seen[i].c++ : 99;
    pthread_mutex_unlock(&mu);
    if (c < 3) fprintf(stderr, "  [steam] %s: not implemented -> 0\n", name);
    return 0;
}

static void *impl_for(const char *ver, const char *method, int nth) {
    for (int i = 0; i < g_nimpl; i++)
        if (!strcmp(g_impl[i].ver, ver) && !strcmp(g_impl[i].method, method) &&
            g_impl[i].nth == nth)
            return g_impl[i].fn;
    return NULL;
}

static sobj *iface(const char *ver) {
    if (!ver) return NULL;
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < g_nobjs; i++)
        if (!strcmp(g_objs[i]->ver, ver)) { pthread_mutex_unlock(&g_mu); return g_objs[i]; }
    const iface_desc *d = NULL;
    for (size_t i = 0; i < sizeof g_desc / sizeof *g_desc; i++)
        if (!strcmp(g_desc[i].ver, ver)) d = &g_desc[i];
    int n = 0;
    if (d) while (d->slots[n]) n++;
    else n = 128;
    sobj *o = calloc(1, sizeof *o);
    o->ver = strdup(ver);
    o->vt = calloc((size_t)n + 1, sizeof *o->vt);
    for (int s = 0; s < n; s++) {
        void *fn = NULL;
        if (d) {
            int nth = 0;                          // which overload of this name
            for (int k = 0; k < s; k++) if (!strcmp(d->slots[k], d->slots[s])) nth++;
            fn = impl_for(ver, d->slots[s], nth);
        }
        if (!fn) {
            char nm[200];
            snprintf(nm, sizeof nm, "%s::%s (#%d)", ver, d ? d->slots[s] : "?", s);
            fn = kl_named_stub(nm, (void *)steam_unimpl);
        }
        o->vt[s] = fn;
    }
    if (g_nobjs < 64) g_objs[g_nobjs++] = o;
    pthread_mutex_unlock(&g_mu);
    fprintf(stderr, "  [steam] interface %s -> %p (%s, %d slots)\n", ver, (void *)o,
            d ? d->cls : "not in the SDK: every slot a logging stub", n);
    return o;
}

// ---- the flat table ------------------------------------------------------------------------------------
typedef struct { const char *name; void *fn; } flat;
static const flat g_flat[] = {
    { "SteamInternal_SteamAPI_Init", (void *)st_init },
    { "SteamAPI_InitSafe", (void *)st_init_bool }, { "SteamAPI_Init", (void *)st_init_bool },
    { "SteamAPI_InitAnonymousUser", (void *)st_init_bool },
    { "SteamInternal_GameServer_Init_V2", (void *)st_gs_init },
    { "SteamAPI_Shutdown", (void *)st_void }, { "SteamGameServer_Shutdown", (void *)st_void },
    { "SteamAPI_RunCallbacks", (void *)st_run_callbacks },
    { "SteamGameServer_RunCallbacks", (void *)st_run_callbacks },
    { "SteamAPI_RegisterCallback", (void *)st_register_cb },
    { "SteamAPI_UnregisterCallback", (void *)st_unregister_cb },
    { "SteamAPI_RegisterCallResult", (void *)st_register_result },
    { "SteamAPI_UnregisterCallResult", (void *)st_unregister_result },
    { "SteamAPI_GetHSteamUser", (void *)st_hsteamuser }, { "SteamAPI_GetHSteamPipe", (void *)st_hsteampipe },
    { "SteamGameServer_GetHSteamUser", (void *)st_gs_hsteamuser },
    { "SteamGameServer_GetHSteamPipe", (void *)st_hsteampipe },
    { "SteamInternal_ContextInit", (void *)st_context_init },
    { "SteamInternal_FindOrCreateUserInterface", (void *)st_find_user },
    { "SteamInternal_FindOrCreateGameServerInterface", (void *)st_find_user },
    { "SteamInternal_CreateInterface", (void *)st_create_iface },
    { "SteamAPI_IsSteamRunning", (void *)st_true }, { "SteamAPI_RestartAppIfNecessary", (void *)st_false },
    { "SteamAPI_SetMiniDumpComment", (void *)st_void }, { "SteamAPI_SetBreakpadAppID", (void *)st_void },
    { "SteamAPI_UseBreakpadCrashHandler", (void *)st_void },
    { "SteamAPI_SetTryCatchCallbacks", (void *)st_void },
    { "SteamAPI_WriteMiniDump", (void *)st_void },
    { "SteamGameServer_BSecure", (void *)st_false },
    { "SteamGameServer_GetSteamID", (void *)gs_steamid },
};

void *kl_steam_lookup(const char *name) {
    if (!name || (strncmp(name, "SteamAPI_", 9) && strncmp(name, "SteamInternal_", 14) &&
                  strncmp(name, "SteamGameServer_", 16)))
        return NULL;
    if (!kl_env_on("KL_STEAM_STUB", 1)) return NULL;
    for (size_t i = 0; i < sizeof g_flat / sizeof *g_flat; i++)
        if (!strcmp(g_flat[i].name, name)) return g_flat[i].fn;
    return NULL;
}
