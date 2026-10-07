// The Steam bridge: how the Steamworks stand-in (kl_steam.c) reaches the
// user's REAL Steam account.
//
// The account itself lives outside the runtime, in SteamDepot's
// SteamBridgeService (a logged-on Steam session plus an offline cache). The
// stand-in talks to it in newline-delimited JSON, one request and one response
// line at a time, every response carrying "ok":
//
//   {"op":"hello","app":N}    -> {"ok":true,"steamid":"<u64>","name":"..","owned":bool,"offline":bool}
//   {"op":"stats","app":N}    -> {"ok":true,"offline":bool,"stats":{..},"achievements":{..}}
//   {"op":"store","app":N,"stats":{..},"achievements":{..},"dry_run":bool}
//                             -> {"ok":true,"queued":bool,"stored":bool}
//   {"op":"playing","app":N,"on":bool} -> {"ok":true}
//
// TWO TRANSPORTS, one call:
//   - a backend registered with kl_steam_set_backend() (the visionOS app does
//     this before the runtime boots; its Swift glue calls SteamBridgeService);
//   - otherwise, on the Mac, the Unix socket $KL_STEAM_BRIDGE (default
//     ~/.steamdepot/bridge.sock), which `steamdepot serve` listens on.
//
// This header is part of the app's C surface (Klepton-Bridging-Header.h).
#ifndef KL_STEAM_BRIDGE_H
#define KL_STEAM_BRIDGE_H
#include <stddef.h>

// A backend answers ONE request line (no trailing newline) with ONE response
// line, written NUL-terminated into `response` (at most `cap` bytes, NUL
// included). Returns the response length, or -1 if the service could not be
// reached or did not answer within `timeout_ms`. Called on a game thread (never
// on the app's main thread, except for the teardown call), possibly from
// several threads at once; it must block until it has an answer.
typedef int (*kl_steam_backend_fn)(const char *request, char *response, size_t cap, int timeout_ms);

// Register (or, with NULL, remove) the backend. Call before the guest boots.
void kl_steam_set_backend(kl_steam_backend_fn fn);
// Whether one is registered.
int  kl_steam_has_backend(void);

// One round trip through whichever transport applies. Same contract as a
// backend: the response length, or -1.
int  kl_steam_backend_call(const char *request, char *response, size_t cap, int timeout_ms);

// Tell Steam the game is no longer running ("playing" off), once. Called by
// SteamAPI_Shutdown and at exit; the app calls it on its own teardown path too,
// because a visionOS app that is backgrounded exits without unwinding the guest.
// Harmless when nothing was announced.
void kl_steam_shutdown(void);
#endif
