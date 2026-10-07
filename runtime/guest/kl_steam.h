// A stand-in Steam client (kl_steam.c): the libsteam_api entry points the
// engine imports (SteamAPI_*, SteamInternal_*, SteamGameServer_*), answered
// without any Steam, with C++ interface objects whose vtables follow the
// Steamworks SDK. Logged-on user, the target's app owned, and real loopback
// ISteamNetworkingSockets. NULL for any other name. Installed on the glibc door
// (kl_glibc_shim.c, Alyx) and on the bionic one (kl_shim.c, Metro Awakening's
// Android build, which links libsteam_api.so).
// KL_STEAM_STUB=0 leaves them to the guest's own libsteam_api.
#ifndef KL_STEAM_H
#define KL_STEAM_H
void *kl_steam_lookup(const char *name);
#endif
