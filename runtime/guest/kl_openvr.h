// Klepton's own OpenVR runtime for the glibc door (kl_openvr.c): answers the
// libopenvr_api.so entry points Alyx's libvr.so imports (VR_InitInternal2,
// VR_GetGenericInterface, ...) with C++ interface objects backed by kl_ovrp —
// the same XR state kl_openxr serves OpenXR guests from. NULL for any other
// name. KL_OPENVR=0 leaves them to the guest's libopenvr_api (which then looks
// for a SteamVR install and fails).
#ifndef KL_OPENVR_H
#define KL_OPENVR_H
void *kl_openvr_lookup(const char *name);
// WaitGetPoses blocks in `wait` before latching the frame's poses (the frame
// pacer, KL_GUEST_PACE_HZ / KL_GUEST_PACE_DIV). NULL, the default, never waits.
void kl_openvr_set_frame_pacer(void (*wait)(void));
#endif
