// A stand-in Steam client for the glibc door (kl_steam.c): the libsteam_api
// entry points the engine imports (SteamAPI_*, SteamInternal_*,
// SteamGameServer_*), answered without any Steam, with C++ interface objects
// whose vtables follow the Steamworks SDK. Logged-on user, app 546560 owned,
// and real loopback ISteamNetworkingSockets. NULL for any other name.
// KL_STEAM_STUB=0 leaves them to the guest's own libsteam_api.
#ifndef KL_STEAM_H
#define KL_STEAM_H
void *kl_steam_lookup(const char *name);
#endif
