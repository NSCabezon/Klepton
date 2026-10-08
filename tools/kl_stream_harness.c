// kl_stream_harness — the headset client's C half, run on the Mac.
//
// Links the SAME runtime/net/kl_stream_client.c the visionOS app links (and
// the rest of the runtime, for kl_ovrp), plays the headset against a host —
// normally build/kl_stream_testhost — and checks what comes back:
//
//   * HELLO from kl_ovrp's display description (a Vision-Pro-shaped frustum
//     and IPD pushed through the same kl_ovrp_set_eye_* seams the compositor
//     uses), retried until HELLO_ACK;
//   * POSE at the display rate through kl_stream_client_send_pose_from_ovrp,
//     with a head yawing at --yaw deg/s, so the host's world-locked grid moves;
//   * VIDEO reassembled + decoded; every pulled frame must carry a pose that
//     is still in the 64-entry ring, and must be what that pose rendered;
//   * PNGs of a few decoded frames (both eyes side by side) for eyeballing.
//
// Usage: kl_stream_harness <host> [port] [--seconds N] [--out DIR] [--yaw DPS]
//                          [--expect-loss] [--png-every N]
// Exit 0 when it decoded frames and every frame matched its ring pose.

#include "kl_stream_client.h"
#include "kl_ovrp.h"

#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int write_png(CVPixelBufferRef pb, const char *path) {
    CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
    size_t w = CVPixelBufferGetWidth(pb), h = CVPixelBufferGetHeight(pb);
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef cg = CGBitmapContextCreate(CVPixelBufferGetBaseAddress(pb), w, h, 8,
                                            CVPixelBufferGetBytesPerRow(pb), cs,
                                            kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little);
    int ok = 0;
    if (cg) {
        CGImageRef img = CGBitmapContextCreateImage(cg);
        CFStringRef p = CFStringCreateWithCString(NULL, path, kCFStringEncodingUTF8);
        CFURLRef url = CFURLCreateWithFileSystemPath(NULL, p, kCFURLPOSIXPathStyle, false);
        CGImageDestinationRef d = CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, NULL);
        if (d) {
            CGImageDestinationAddImage(d, img, NULL);
            ok = CGImageDestinationFinalize(d);
            CFRelease(d);
        }
        CFRelease(url); CFRelease(p); CGImageRelease(img); CGContextRelease(cg);
    }
    CGColorSpaceRelease(cs);
    CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
    return ok;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    const char *host = "127.0.0.1";
    int port = 0, png_every = 90, expect_loss = 0;
    double seconds = 6, yaw_dps = 20;
    const char *out = "build/stream";
    int positional = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "--yaw") && i + 1 < argc) yaw_dps = atof(argv[++i]);
        else if (!strcmp(argv[i], "--png-every") && i + 1 < argc) png_every = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--expect-loss")) expect_loss = 1;
        else if (positional == 0) { host = argv[i]; positional++; }
        else if (positional == 1) { port = atoi(argv[i]); positional++; }
    }
    mkdir(out, 0755);

    // The display, through the compositor's own seams (values measured on the
    // device: per-eye asymmetric tangents, ~63 mm IPD, 90 Hz).
    kl_ovrp_set_eye_frustum(0, 1.0f, 0.83f, 0.93f, 1.09f);
    kl_ovrp_set_eye_frustum(1, 0.83f, 1.0f, 0.93f, 1.09f);
    kl_ovrp_set_eye_offset(0, -0.0315f, 0, 0);
    kl_ovrp_set_eye_offset(1, 0.0315f, 0, 0);
    kl_ovrp_set_display_frequency(90);
    kl_ovrp_set_eye_texture_size(1888, 1792);

    kl_stream_client *c = kl_stream_client_create(host, port);
    if (!c) { fprintf(stderr, "harness: no client\n"); return 2; }
    const float tan[2][4] = { { 1.0f, 0.83f, 0.93f, 1.09f }, { 0.83f, 1.0f, 0.93f, 1.09f } };
    kl_stream_hello hello;
    kl_stream_hello_from_ovrp(&hello, tan, "harness");
    kl_stream_client_set_display(c, &hello);

    double t0 = kl_stream_now(), next = t0, period = 1.0 / 90;
    unsigned frames = 0, mismatched = 0, missing = 0, pngs = 0;
    uint32_t last_frame = 0;
    float yaw = 0;
    while (kl_stream_now() - t0 < seconds) {
        double now = kl_stream_now();
        if (next > now) { usleep((useconds_t)((next - now) * 1e6)); continue; }
        next += period;
        double t_present = now + 0.030;     // a typical prediction horizon
        yaw = (float)(yaw_dps * (now - t0) * M_PI / 180);
        float s = sinf(yaw / 2), w = cosf(yaw / 2);
        kl_ovrp_set_head_pose_time(t_present);
        kl_ovrp_set_head_pose(0, 1.6f, 0, 0, s, 0, w);
        kl_stream_client_send_pose_from_ovrp(c, t_present, 0);

        kl_stream_frame_info info;
        CVPixelBufferRef pb = kl_stream_client_pull_frame(c, &info);
        if (!pb) continue;
        frames++;
        if (!info.have_pose) { if (info.pose_id != 0) missing++; }
        else {
            // The pose the frame says it rendered with must be the one we sent
            // with that id: same t_pose, and a yaw consistent with that time.
            double ty = yaw_dps * ((info.t_pose - 0.030) - t0) * M_PI / 180;
            float qy = sinf((float)ty / 2);
            if (fabsf(qy - info.pose7[4]) > 2e-3f) mismatched++;
        }
        if (info.frame_id <= last_frame) mismatched++;
        last_frame = info.frame_id;
        kl_stream_client_note_displayed(c, t_present, info.t_pose);
        if (frames == 1 || (png_every > 0 && frames % (unsigned)png_every == 0)) {
            char p[512];
            snprintf(p, sizeof p, "%s/frame_%05u.png", out, info.frame_id);
            if (write_png(pb, p)) {
                pngs++;
                printf("harness: wrote %s (%dx%d, pose %u, key %d, host render %.1f ms, "
                       "encode %.1f ms, decode->now %.1f ms)\n", p, info.width, info.height,
                       info.pose_id, info.keyframe, info.host_ms_render, info.host_ms_encode,
                       (kl_stream_now() - info.t_decoded) * 1000);
            }
        }
        CVPixelBufferRelease(pb);
    }
    kl_stream_client_status st;
    kl_stream_client_get_status(c, &st);
    printf("harness: %u frames pulled, %u decoded, %u lost, %u IDR requests, %u decode errors, "
           "%u pose misses, %u ring mismatches, %u PNGs, state %s, last line: %s\n",
           frames, st.frames_decoded, st.frames_lost, st.idr_requests, st.decode_errors,
           st.pose_misses, mismatched, pngs, kl_stream_client_state_name(st.state), st.line);
    kl_stream_client_destroy(c);
    int ok = frames > 0 && mismatched == 0 && missing == 0 && st.decode_errors == 0;
    if (expect_loss) ok = ok && st.frames_lost > 0 && st.idr_requests > 0;
    printf("harness: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
