//
//  KleptonControllers.swift — the controller seam on visionOS.
//
//  The guest wants two Oculus Touch controllers: poses for nodes 3 and 4, and
//  `ovrpButton` raw bits (pressed, and touched — both words in the BUTTON
//  layout; kl_ovrp makes the guest's ovrpTouch / ovrpNearTouch words from the
//  touched one), an index trigger, a hand trigger and a
//  thumbstick per hand (`kl_ovrp_set_hand_pose`, `kl_ovrp_set_controller_input`).
//  This platform can answer that two ways, and which one is available is a
//  runtime fact, not a build-time one:
//
//    * **PSVR2 Sense controllers**, when paired. Poses come from ARKit's
//      `AccessoryTrackingProvider`, buttons from GameController. This is the
//      real answer — it is the only source that has buttons at all, the only
//      one whose pose the platform publishes as a proper `.grip`, and the only
//      one that reports velocity.
//
//  **Every pose here is predicted to the frame's presentation time**, the same
//  instant `queryDeviceAnchor(atTimestamp:)` gives the head. Reading the last
//  anchor an async stream delivered instead — which is what this did — caps the
//  controllers at the tracker's delivery rate rather than the display's, and a
//  pose repeated across three frames of a 90 Hz picture does not read as
//  latency, it reads as the hands running at 30 Hz.
//    * **Hands**, always. `HandTrackingProvider`'s skeleton supplies the grip
//      pose (the middle-finger metacarpal, so a hilt is gripped rather than
//      swinging off the wrist), and the index trigger comes from the system's
//      own pinch recogniser through `LayerRenderer.onSpatialEvent` — not from
//      our arithmetic on two fingertip joints, which was tried twice and was
//      unstable both times. The fingertip distance survives as the trigger's
//      analog value and as the fallback if no spatial event ever arrives.
//      No other button exists on a hand, but a trigger is the one a menu
//      clicks with.
//
//  So this file is a merge, not a switch: Sense wins per hand where it is
//  present, hands fill in the rest, and a Sense controller that is paired for
//  the right hand only still leaves the left hand tracking. Per *hand*, not
//  per session, because that is how they actually arrive — one at a time,
//  mid-run, and either one can drop out.
//
//  Nothing below the seam changes. kl_ovrp still reports node 9 for the head
//  and 3/4 for the hands, and the SDL viewer on the host still drives the same
//  two functions (kl_view.c) — which is what makes the host a usable A/B for
//  anything that looks like an input bug here.
//
//  **The Sense half lives in SenseKit** (the `SenseKit` submodule beside this
//  tree): discovery, the accessory anchors and their prediction, the grip
//  correction, the GameController input poll and the haptics engines. What is
//  left here is everything that is about THIS guest seam — the `ovrpButton`
//  layout and every `kl_ovrp_*` call, the KL_* knobs that configure SenseKit,
//  the hand-tracking fallback and the gamepad merge.
//
import Foundation
import ARKit
import GameController
import SenseKit
import SwiftUI
import simd

/// One hand's worth of state, in the guest's own units.
struct KleptonHandState {
    var position: SIMD3<Float>?
    var orientation: simd_quatf?
    var buttons: UInt32 = 0
    var touches: UInt32 = 0
    var indexTrigger: Float = 0
    var handTrigger: Float = 0
    var stick: SIMD2<Float> = .zero
    /// Tracking-space linear (m/s) and angular (rad/s) velocity, for the
    /// ovrpPoseStatef fields libunity forwards to `XRNodeState.velocity`.
    /// Only the Sense controllers report these — a `HandAnchor` carries no
    /// motion at all, and differentiating a pose stream to invent one produces
    /// a number that looks physical and is mostly tracker noise.
    var linearVelocity: SIMD3<Float> = .zero
    var angularVelocity: SIMD3<Float> = .zero
    /// Whether a Sense controller supplied this, as opposed to a hand anchor.
    /// Only a Sense controller can press anything, so this is also "are the
    /// buttons meaningful".
    var fromController = false
}

/// `ovrpButton` **raw** bits.
///
/// Raw, not virtual: `ControllerState4.Buttons` carries `OVRInput.RawButton`,
/// and OVRInput maps raw→virtual itself through its own buttonMap. Emitting
/// virtual values is a specific, silent bug the host already hit — 0x2000 is
/// `PrimaryIndexTrigger` in virtual space and `RThumbstickDown` in raw, so
/// every "trigger press" became a stick flick and no press ever reached a UI.
///
/// The eight marked (v) are the ones `runtime/gfx/kl_view.c` drives on the host and
/// which have been seen to work end to end in gameplay. The rest are the
/// standard raw layout, consistent with those eight, and are **not** yet
/// confirmed against the guest's own metadata — if a menu or stick click does
/// nothing on device, suspect these before suspecting the plumbing.
enum OVRPRawButton {
    static let a: UInt32              = 0x0000_0001   // (v)
    static let b: UInt32              = 0x0000_0002   // (v)
    static let rThumbstick: UInt32    = 0x0000_0004
    static let lThumbstickUp: UInt32   = 0x0000_0010
    static let lThumbstickDown: UInt32 = 0x0000_0020
    static let lThumbstickLeft: UInt32 = 0x0000_0040
    static let lThumbstickRight: UInt32 = 0x0000_0080
    static let x: UInt32              = 0x0000_0100   // (v)
    static let y: UInt32              = 0x0000_0200   // (v)
    static let lThumbstick: UInt32    = 0x0000_0400
    static let rThumbstickUp: UInt32  = 0x0000_1000   // (v)
    static let rThumbstickDown: UInt32 = 0x0000_2000  // (v)
    static let rThumbstickLeft: UInt32 = 0x0000_4000  // (v)
    static let rThumbstickRight: UInt32 = 0x0000_8000 // (v)
    static let start: UInt32          = 0x0010_0000
    static let back: UInt32           = 0x0020_0000
    static let rIndexTrigger: UInt32  = 0x0400_0000   // (v)
    static let rHandTrigger: UInt32   = 0x0800_0000   // (v)
    static let lIndexTrigger: UInt32  = 0x1000_0000   // (v)
    static let lHandTrigger: UInt32   = 0x2000_0000   // (v)
}

/// The wrist frame ARKit reports, expressed in the grip frame the guest wants.
///
/// **These are not the same basis, and nothing above this line converts them.**
/// `HandAnchor.originFromAnchorTransform` is the wrist joint, whose axes are
/// anatomical and mirrored between hands; the guest wants an Oculus Touch grip
/// pose — right-handed, +Y up out of the top of the hilt, **-Z along the
/// direction the hilt points**. Handing the wrist frame over unconverted is
/// what put the laser out of the side of the wrist rather than out of the fist.
///
/// The mapping is ALVR's, from `WorldTracker.swift`'s
/// `rightHandOrientationCorrection` — the same anchor, the same provider, the
/// same OS — and it is observed correct on device for the RIGHT hand:
///
///   grip X = wrist +Y, grip Y = wrist +Z, grip Z = wrist +X
///
/// **The LEFT hand is `R · Rx(180)`**, and four playtests force that — no free
/// parameters are left:
///
/// | left constant | the hilt |
/// |---|---|
/// | A = `R · Ry(180)` (ALVR's mirrored pair) | base at the pinky |
/// | B = `R` (the right hand's own) | "about how I would want it, **if it were just rotated 180**" |
/// | C = `R · Rz(180)` | base at the pinky again — indistinguishable from A |
///
/// A and C look the same, and that is the measurement that closes it. They
/// differ by exactly `Rx(180)` (`Rx·Ry = Rz` for half-turns about orthogonal
/// axes), so a feature invariant between them is invariant under `Rx(180)` —
/// which means **the visible hilt axis is the grip's X**, not its Z. (It is not
/// our Z because the game does not point a saber down the raw grip axis:
/// `IVRPlatformHelper.AdjustControllerTransform` rotates a Touch controller by
/// a large device-specific offset before rendering anything.)
///
/// B reverses the hilt relative to A and C — it flips X — and B is the one whose
/// direction was right. So the fix must keep B's X and turn the hilt over about
/// it: `Rx(180)`, the only half-turn that preserves X.
///
/// Which is also why the two earlier attempts failed the same way. Both reasoned
/// about *which axis of Apple's wrist frame means what*, and that has no ground
/// truth to check against — the relationship between the hands' frames is a
/// convention that is not written down, and cannot be derived (a joint transform
/// is a rotation, determinant +1; a mirror is not, so something must differ
/// between the hands and which axis differs was a free choice). The playtests do
/// have ground truth. Reading them eliminated Y, then eliminated Z, and X is what
/// remains.
///
/// A/Bs, since this is still inference from four samples:
///   * `KL_HAND_ANATOMICAL=1` derives the left frame from joint **positions**
///     instead (`anatomicalFrame`) — no convention needed at all, but it has
///     never been confirmed on device.
///   * `KL_HAND_MIRROR=1` restores row A.
///   * `KL_HAND_ROT_L="180,0,0"` / `"0,180,0"` / `"0,0,180"` sweeps the axes
///     from here with no rebuild.
private let klGripFromWrist: [simd_quatf] = {
    let r = simd_quatf(ix: 0.5, iy: 0.5, iz: 0.5, r: 0.5)
    if ProcessInfo.processInfo.environment["KL_HAND_MIRROR"] == "1" {
        NSLog("[cp] KL_HAND_MIRROR: ALVR's mirrored pair")
        return [simd_quatf(ix: 0.5, iy: -0.5, iz: -0.5, r: 0.5), r]
    }
    // r * Rx(180), multiplied out, so the constant in the source and the
    // constant in a log line are the same object.
    return [simd_quatf(ix: -0.5, iy: -0.5, iz: 0.5, r: 0.5), r]
}()

/// `KL_HAND_ROT="x,y,z"` (degrees) and `KL_HAND_POS="x,y,z"` (metres): a further
/// rotation and offset in the *grip's own* frame, applied to every **hand** pose
/// after the correction above. `KL_HAND_ROT_L` / `KL_HAND_ROT_R` (and the `POS`
/// pair) override it for one hand.
///
/// **Hands only.** This used to be applied to the Sense controllers' poses as
/// well, and that was wrong in a way that is easy to miss: the default offset
/// below is a *hand* correction — the metacarpal midpoint is not where a
/// controller's tracked origin sits — while `AccessoryAnchor`'s `.grip` already
/// **is** "where the hand holds this", published by the platform. Sharing the
/// knob therefore pushed every Sense pose 4 cm down its own hilt for a reason
/// that does not apply to it, and made the two paths impossible to tune
/// independently. The Sense path has `KL_SENSE_ROT` / `KL_SENSE_POS`.
///
/// Per-hand, because the errors that are left are not guaranteed to be
/// symmetric — the shared correction above is a claim about a convention, and
/// if it turns out to be half right the fix is 180 degrees on one hand only,
/// which a shared knob cannot express.
///
/// This exists because the remaining error is a comfort angle, not a basis
/// error, and a comfort angle can only be found by wearing the thing: Beat
/// Saber's own `IVRPlatformHelper.AdjustControllerTransform` tilts a Touch
/// controller by a device-specific offset we cannot read, so even a perfect
/// grip pose does not put the saber where a Quest player's muscle memory
/// expects. One device run per candidate angle, with no rebuild between them.
private struct KLHandTune {
    var rot = simd_quatf(ix: 0, iy: 0, iz: 0, r: 1)
    /// 4 cm out along the hilt axis by default (`-Z`, the direction the blade
    /// leaves the fist). The metacarpal midpoint is where the hand *holds* the
    /// hilt, which is not where a Touch controller's tracked origin sits — the
    /// controller extends past the fist — so a hilt pinned to the palm reads as
    /// sliding back down its own pointing ray. Directionally right, not gripped
    /// right. `KL_HAND_POS` replaces this outright.
    var pos = SIMD3<Float>(0, 0, -0.04)

    /// Indexed by hand: 0 = left, 1 = right.
    static let shared: [KLHandTune] = [make(0), make(1)]

    private static func make(_ hand: Int) -> KLHandTune {
        var t = KLHandTune()
        if let (d, k) = klEnvTriple("KL_HAND_ROT", hand) {
            t.rot = klEulerXYZ(d)
            NSLog("[cp] \(k): hand \(hand) extra grip rotation \(d) degrees")
        }
        if let (p, k) = klEnvTriple("KL_HAND_POS", hand) {
            t.pos = p
            NSLog("[cp] \(k): hand \(hand) extra grip offset \(p) m")
        }
        return t
    }
}

/// `"x,y,z"` from `<key><_L|_R>` or `<key>`, whichever is set — the per-hand key
/// wins. Shared by both tuning structs so a knob means the same thing on either
/// path.
private func klEnvTriple(_ key: String, _ hand: Int) -> (SIMD3<Float>, String)? {
    let env = ProcessInfo.processInfo.environment
    for k in [key + (hand == 0 ? "_L" : "_R"), key] {
        guard let s = env[k] else { continue }
        let f = s.split(separator: ",")
            .compactMap { Float($0.trimmingCharacters(in: .whitespaces)) }
        guard f.count == 3 else {
            NSLog("[cp] \(k)=\(s) is not three comma-separated numbers — ignored")
            continue
        }
        return (SIMD3<Float>(f[0], f[1], f[2]), k)
    }
    return nil
}

/// Degrees to a quaternion, X then Y then Z in the frame being corrected.
private func klEulerXYZ(_ degrees: SIMD3<Float>) -> simd_quatf {
    let r = degrees * .pi / 180
    return simd_quatf(angle: r.x, axis: [1, 0, 0])
         * simd_quatf(angle: r.y, axis: [0, 1, 0])
         * simd_quatf(angle: r.z, axis: [0, 0, 1])
}

/// The Sense controllers' own correction — `KL_SENSE_ROT` / `KL_SENSE_POS`, in
/// the grip's frame, per hand with `_L`/`_R`.
///
/// Separate from `KLHandTune` because the two paths start from different
/// places. `AccessoryAnchor`'s `.grip` is the platform's own answer to "where
/// does a hand hold this and which way does it point", so it needs no
/// hand-shaped compensation at all — what it needs is the gap between Apple's
/// grip convention and the pose an *Oculus Touch* controller would have
/// reported, which is the thing the guest is actually built against and which
/// nothing on this platform can tell us.
///
/// Two of the three defaults are ALVR's, from `WorldTracker.swift`
/// (`controllerToAlvrDeviceMotion`) — the same controller, the same anchor, the
/// same OS:
///
///   * **5.037 degrees about X.** Not a guess: it is the tilt in SteamVR's own
///     PSVR2 controller model JSON, i.e. the offset between the tracked frame
///     and the handle a user perceives.
///   * **(0.002, 0, -0.01) m**, the origin nudge ALVR applies with it.
///
/// The third is this guest's, and it is the larger one:
///
///   * **`hiltPitch`, -37 degrees about X.** Measured the only way it can be —
///     by playing, and reading the number back off Beat Saber's own in-game
///     controller adjustment, where 37 degrees on X is what put the hilts
///     straight. It is a *convention* gap rather than a comfort angle: the
///     platform's `.grip` is where the hand holds the controller, and what the
///     guest is built against is the pose an Oculus Touch would have reported,
///     which is not the same frame and cannot be derived from here.
///
///     **Negative**, because the same magnitude POSITIVE pitches the hilts
///     backward on device — the game's adjustment is applied in Unity's left-handed
///     frame and ours is not, so the magnitude carries over and the sign does
///     not.
///
/// Which leaves the two constants pulling opposite ways, for -31.96 total. If
/// the residual after a playtest looks like about five degrees *forward*, the
/// likely reading is that ALVR's PSVR2 tilt wants this frame's sign too, for
/// -42.04. Beat Saber's own `OculusVRHelper.AdjustControllerTransform` pitches
/// a Touch controller by 40 degrees, and `KL_SENSE_PITCH=-45.037` puts the
/// total there exactly (-45.037 + 5.037) in one run.
///
/// **The maths is SenseKit's** (`SenseGripCorrection.apply`, the velocity gate,
/// the prediction cap); this struct only reads the KL_* knobs and turns them
/// into a `SenseConfiguration` (`senseConfiguration`, plus the live grip
/// correction `KleptonControllers` hands SenseKit per pose).
struct KLSenseTune {
    /// ALVR's PSVR2 comfort tilt and this guest's hilt pitch, kept apart so the
    /// log line and the source agree on where each degree came from.
    static let psvr2Tilt: Float = SenseGripCorrection.psvr2TiltDegrees
    static let hiltPitch: Float =
        ProcessInfo.processInfo.environment["KL_SENSE_PITCH"].flatMap { Float($0) } ?? -37

    var rot = klEulerXYZ(SIMD3<Float>(psvr2Tilt + hiltPitch, 0, 0))
    var pos = SIMD3<Float>(-0.005, 0.02, -0.02)
    /// `KL_SENSE_PIVOT="x,y,z"` — the point, in the grip's own frame, that
    /// `rot` should turn ABOUT. Zero (the default) turns about the reported
    /// origin, which is what this path has always done.
    ///
    /// **A translation cannot remove an arc, and that is why `pos` is not
    /// enough.** `rot` is applied about the origin the platform reports —
    /// somewhere back at the wrist — so everything the guest draws from this
    /// pose swings by the rotation angle about that point. `pos` slides the
    /// result; it cannot change what the result rotated around. Measured on
    /// device: with `KL_SENSE_POS="0,0,0"` the swing on BONELAB is unchanged.
    ///
    /// Stated as a POINT because a point is estimable by eye — "the hilt centre
    /// is about six centimetres forward and four down from where the controller
    /// is tracked" — where the equivalent translation has no physical meaning
    /// and a different right answer for every rotation angle. The same lever,
    /// with the same name and units, as `KL_XR_GRIP_PIVOT` on the OpenXR path.
    var pivot = SIMD3<Float>(0, 0, 0)

    static let shared: [KLSenseTune] = [make(0), make(1)]

    /// `KL_SENSE_MIRROR=0` — stop mirroring the shared offsets' X term for the
    /// LEFT hand.
    ///
    /// A grip offset is a point on a hand, and the two hands are mirror images:
    /// the same physical displacement is `+x` on one and `-x` on the other.
    /// Confirmed on device — "we need to flip the x offset for the left hand" —
    /// and without it a shared value is right on one hand and wrong by twice
    /// its own size on the other, which is the CHIRAL error reported on BONELAB.
    ///
    /// Applies only to the shared `KL_SENSE_POS` / `KL_SENSE_PIVOT`. An explicit
    /// `_L` / `_R` is taken exactly as given: a per-hand value is already an
    /// answer about that hand, and mirroring it would be second-guessing it.
    static let mirrorX =
        ProcessInfo.processInfo.environment["KL_SENSE_MIRROR"] != "0"

    private static func mirrored(_ v: SIMD3<Float>, _ hand: Int) -> SIMD3<Float> {
        (mirrorX && hand == 0) ? SIMD3<Float>(-v.x, v.y, v.z) : v
    }

    private static func make(_ hand: Int) -> KLSenseTune {
        var t = KLSenseTune()
        t.pos   = mirrored(t.pos, hand)
        t.pivot = mirrored(t.pivot, hand)
        if hand == 0 {
            NSLog(String(format: "[cp] Sense grip pitch %.3f deg about X "
                         + "(%.3f PSVR2 tilt + %.3f hilt; KL_SENSE_PITCH, "
                         + "KL_SENSE_ROT override)",
                         psvr2Tilt + hiltPitch, psvr2Tilt, hiltPitch))
        }
        if let (d, k) = klEnvTriple("KL_SENSE_ROT", hand) {
            t.rot = klEulerXYZ(d)
            NSLog("[cp] \(k): hand \(hand) Sense grip rotation \(d) degrees")
        }
        // A shared value is mirrored for the left hand; an explicit _L/_R is
        // not — klEnvTriple reports which it found, so the two are told apart
        // by the NAME rather than by guessing.
        if let (p, k) = klEnvTriple("KL_SENSE_POS", hand) {
            t.pos = k == "KL_SENSE_POS" ? mirrored(p, hand) : p
            NSLog("[cp] \(k): hand \(hand) Sense grip offset \(t.pos) m")
        }
        if let (c, k) = klEnvTriple("KL_SENSE_PIVOT", hand) {
            t.pivot = k == "KL_SENSE_PIVOT" ? mirrored(c, hand) : c
            NSLog("[cp] \(k): hand \(hand) Sense grip rotates about \(t.pivot) m")
        }
        return t
    }

    /// `KL_SENSE_VEL_FRAME=world` — treat `AccessoryAnchor.velocity` as already
    /// being in tracking space instead of the accessory's own frame.
    ///
    /// The default follows ALVR, which rotates both velocities by the grip
    /// orientation before using them, i.e. reads them as controller-local.
    /// Apple documents neither frame, and the two differ only in *direction*
    /// (the magnitude is the same either way), so this is the knob that settles
    /// it on device: shake the controller along one axis and see which reading
    /// stays pointed the way your hand went.
    /// `KL_SENSE_PREDICT_MS=<ms>` — how far into the future a Sense controller
    /// pose may be predicted. 0 is the measured pose; the default 50 is the
    /// same horizon `kl_openxr` clamps the HEAD to, so both poses a guest reads
    /// are bounded by one number.
    ///
    /// **A time cap, not a fraction of the interval**, and that distinction is
    /// the whole knob: a tracker's extrapolation error is a function of how far
    /// AHEAD it reaches, not of what proportion of some pipeline that
    /// represents. A fraction moves with the frame rate, so the same setting
    /// means a different horizon at 90 Hz and at 60, and tuning it once tunes
    /// it for one frame rate.
    ///
    /// It exists because both extremes were measured wrong on Steam Link: at
    /// full prediction the controllers overshoot badly (we predict to
    /// presentation time and publish a velocity, and SteamVR predicts AGAIN to
    /// the PC's photon time — twice applied), and with no prediction the
    /// overshoot goes but they visibly lag, because the PC's own prediction
    /// does not cover our pipeline.
    static let sensePredictMaxMs: Double = {
        if let v = ProcessInfo.processInfo.environment["KL_SENSE_PREDICT_MS"],
           let f = Double(v) { return max(0.0, f) }
        // Steam Link re-predicts downstream (SteamVR, to the PC's photon time),
        // so predicting the pose forward HERE as well applies it twice and the
        // controllers overshoot/shake. Default to the raw measured pose for it
        // and let SteamVR do the single prediction it would do for a native
        // controller — the velocity is still real, so gravity-glove flicks and
        // throws (which read velocity) keep working. Native guests do not
        // re-predict and keep the 50 ms horizon.
        return kl_app_target_is_steamlink() != 0 ? 0.0 : 50.0
    }()

    static let velocityIsLocal =
        ProcessInfo.processInfo.environment["KL_SENSE_VEL_FRAME"] != "world"

    /// (Adapted from avp-play, MIT, github.com/myMartek/avp-play.)
    /// `KL_SENSE_VEL_GATE=<m/s>` / `KL_SENSE_VEL_GATE_ANG=<rad/s>` — a NOISE GATE
    /// on the velocity we publish, not a low-pass. visionOS's accessory velocity
    /// is a few cm/s of noise when the hand is still, invisible in the pose but
    /// turned into jitter once SteamVR extrapolates the render pose from it. A
    /// low-pass killed the jitter but also lagged the pose (it starves SteamVR's
    /// prediction) and blunted the flick (a short flick IS a short velocity
    /// spike). A gate instead leaves fast motion ALONE — above the threshold the
    /// real velocity passes (minus the threshold, a few-percent trim on a flick),
    /// below it goes to zero — so a still hand is steady while a flick and
    /// SteamVR's own prediction keep the full signal. 0 disables it; the default
    /// is on for Steam Link only. Values are deliberately small.
    static let velGate: Float = {
        if let v = ProcessInfo.processInfo.environment["KL_SENSE_VEL_GATE"],
           let f = Float(v) { return max(0.0, f) }
        return kl_app_target_is_steamlink() != 0 ? 0.06 : 0.0
    }()
    static let velGateAng: Float = {
        if let v = ProcessInfo.processInfo.environment["KL_SENSE_VEL_GATE_ANG"],
           let f = Float(v) { return max(0.0, f) }
        return kl_app_target_is_steamlink() != 0 ? 0.35 : 0.0
    }()
    // The gate itself (soft magnitude gate: zero below the threshold, minus
    // the threshold above it) is `SenseVelocityGate.gate`.

    /// The live grip correction for one hand, re-read for every pose so a
    /// slider in the boot window moves the controller while it is being
    /// dragged (KleptonTuning). Its X term (and yaw and roll) is mirrored for
    /// the left hand by SenseKit, for the reason KL_SENSE_MIRROR gives.
    static func liveGrip(_ hand: Int) -> SenseGripCorrection {
        // First access builds `shared`, which is what logs the KL_SENSE_*
        // lines — at the first pose, as before the move.
        _ = shared[hand]
        let live = KleptonTuning.shared.snapshot()
        return SenseGripCorrection(tiltDegrees: psvr2Tilt, pitchDegrees: live.sensePitch,
                                   yawDegrees: live.senseYaw, rollDegrees: live.senseRoll,
                                   position: live.sensePos, pivot: live.sensePivot,
                                   mirrorLeft: mirrorX)
    }

    /// Everything above that SenseKit takes as configuration — the same
    /// defaults and the same KL_* knobs as before the move, haptics included
    /// (`KleptonControllers.hapticsConfiguration`).
    static func senseConfiguration(haptics: SenseHapticsConfiguration) -> SenseConfiguration {
        var c = SenseConfiguration()
        c.gripCorrection.mirrorLeft = mirrorX
        c.predictionCapMs = sensePredictMaxMs
        c.velocityGate = SenseVelocityGate(linear: velGate, angular: velGateAng)
        c.velocityInAccessoryFrame = velocityIsLocal
        c.haptics = haptics
        return c
    }
}

final class KleptonControllers {
    // 0 = left, 1 = right, matching kl_ovrp's hand indices (nodes 3 and 4).
    private var state: [KleptonHandState] = [KleptonHandState(), KleptonHandState()]

    // Poses arrive asynchronously from AccessoryTrackingProvider's anchor
    // stream (inside SenseKit), while buttons are polled synchronously on the
    // render thread and the system pinch arrives on its own queue, so the
    // halves genuinely do cross threads and the lock is not defensive.
    private let lock = NSLock()

    /// The Sense controllers: discovery, the accessory anchors and their
    /// prediction, the grip correction, the input poll and the haptics
    /// engines. Configured from the KL_* knobs (`KLSenseTune`,
    /// `hapticsConfiguration`), logging on `[cp]`, and wired to kl_ovrp
    /// through the hooks set in `init`.
    private let senseKit: SenseControllers

    init() {
        senseKit = SenseControllers(configuration:
            KLSenseTune.senseConfiguration(haptics: Self.hapticsConfiguration))
        senseKit.logger = { NSLog("%@", "[cp] " + $0) }
        senseKit.liveGripCorrection = { KLSenseTune.liveGrip($0.rawValue) }
        senseKit.predictionObserver = { Self.notePredictionHorizon(ahead: $0, cap: $1) }
        // A controller in the hand is a Touch controller picked up: let the
        // guest see its fingers near the controls once, so an edge-driven
        // hand pose (IEYTD) gets the release that opens it (kl_ovrp.c).
        senseKit.onControllerBound = { kl_ovrp_arm_near_touch(Int32($0.rawValue)) }
        // The haptics pull: see `pumpHaptics`.
        senseKit.hapticsSource = { hand in
            var amplitude: Float = 0, seconds: Float = 0
            let live = kl_ovrp_haptics_pull(Int32(hand.rawValue), &amplitude, &seconds) != 0
            return live ? (amplitude, seconds) : nil
        }
    }

    /// Whether the last accessory provider found any Sense controller.
    var senseConnected: Bool { senseKit.accessoriesConnected }
    /// Which hands have a Sense controller connected right now (left, right),
    /// refreshed every frame by `spatialControllersChanged` from the same
    /// GCController poll — the HUD's "controller disconnected" card reads it.
    /// Render thread only, like that poll.
    var presentHands: [Bool] { senseKit.presentHands }

    // MARK: - Ghost controllers (KleptonHUD)

    /// When each controller last showed input (CACurrentMediaTime), under `lock`.
    private var lastActive: [TimeInterval] = [-100, -100]
    func lastInputActivity() -> [TimeInterval] {
        lock.lock(); defer { lock.unlock() }
        return lastActive
    }

    /// The physical controller for the ghost model: the `.grip` frame with the
    /// platform's RENDERED correction (the one meant for drawing content
    /// aligned with the device) and none of the game's calibration, predicted
    /// to the frame like the pose. nil when there is no anchor for that hand.
    func ghostSample(hand: Int, at time: TimeInterval) -> KLGhostSample? {
        guard let h = SenseHand(rawValue: hand),
              let g = senseKit.ghostSample(h, at: time) else { return nil }
        return KLGhostSample(originFromGrip: g.originFromGrip, positionTracked: g.positionTracked,
                             heldByPlatform: g.heldByPlatform,
                             speed: g.speed, angularSpeed: g.angularSpeed)
    }

    // MARK: - Discovery

    /// Has the paired set changed since the last time this was asked?
    /// (SenseKit's `controllersChanged`; also refreshes `presentHands`.)
    func spatialControllersChanged() -> Bool {
        senseKit.controllersChanged()
    }

    /// Build the accessory-tracking provider for whatever is paired now —
    /// unconditionally when the capability exists, including with an empty
    /// accessory list, so a later re-`run()` can pick a controller up (see
    /// `SenseControllers.makeTrackingProvider`).
    func makeAccessoryProvider() async -> (any DataProvider)? {
        await senseKit.makeTrackingProvider()
    }

    /// Drop the pose for any hand whose controller has gone away, so a
    /// disconnect cannot leave the guest holding the last pose forever. The
    /// hand-tracking fallback takes over on the next frame. SenseKit drops the
    /// anchors, the provider, the input bindings and the haptics engines.
    func forgetAccessoryPoses() {
        senseKit.forgetPoses()
        for i in 0...1 {
            lock.lock(); state[i] = KleptonHandState(); lock.unlock()
        }
    }

    /// Consume the accessory anchor stream. Runs for the life of the session.
    func consumeAccessoryAnchors(_ provider: any DataProvider) async {
        guard let p = provider as? AccessoryTrackingProvider else { return }
        await senseKit.consumeAnchors(from: p)
    }

    /// Say, once, how far ahead the frame's presentation time actually is and
    /// whether the cap reaches it (SenseKit's `predictionObserver`).
    ///
    /// Without this the cap is untunable from a log: a horizon of 50 ms over a
    /// pipeline that is 30 ms deep is not a mild setting, it is NO setting —
    /// the cap never binds and the prediction is the full one that overshoots.
    /// The number that decides it is measured here and nowhere else.
    private static var loggedHorizon = false
    private static func notePredictionHorizon(ahead: Double, cap: Double) {
        if loggedHorizon || !ahead.isFinite { return }
        loggedHorizon = true
        NSLog(String(format: "[ctrl] sense prediction: presentation is %.1f ms "
                     + "ahead, cap %.1f ms (KL_SENSE_PREDICT_MS) — the cap %@",
                     ahead * 1000, cap * 1000,
                     cap <= 0 ? "is off, poses are the measured ones"
                     : (cap < ahead ? "BINDS" : "does NOT bind, this is the full "
                        + "prediction; lower it to shorten the horizon")))
    }

    /// Apply the KL_HAND_ROT / KL_HAND_POS tuning, in the grip's own frame.
    private static func tuned(_ p: SIMD3<Float>, _ q: simd_quatf,
                              hand: Int) -> (SIMD3<Float>, simd_quatf) {
        let t = KLHandTune.shared[hand]
        let q2 = q * t.rot
        return (p + q2.act(t.pos), q2)
    }


    /// The hand's **anatomical** frame, built out of joint positions rather than
    /// out of anyone's frame convention — OpenXR's grip basis, by its own
    /// definition:
    ///
    ///   * `-Z` is "the ray through the tube formed by the non-thumb fingers,
    ///     in the direction of little finger to thumb" — so `+Z` runs index
    ///     knuckle to little knuckle.
    ///   * `+X` is the palm normal: *into* the palm for the right hand, *away
    ///     from* it for the left.
    ///   * `+Y` completes it.
    ///
    /// **Why this is worth computing rather than converting.** Two attempts at
    /// converting the wrist frame both got the two hands' relationship wrong,
    /// in opposite directions, because that relationship is a convention nobody
    /// documents and it cannot be derived from anatomy: a joint transform is a
    /// rotation (determinant +1) and a mirror is not, so *something* must differ
    /// between the hands and which axis differs is a free choice Apple made.
    /// Positions have no such freedom. Built from three joint positions, the
    /// frame comes out consistent between hands by construction.
    ///
    /// The chirality that looks like it should need a special case cancels:
    /// `n = (I-W) x (L-W)` points *out of* the palm on the right hand and *into*
    /// it on the left, and OpenXR wants `+X` into the palm on the right and out
    /// of it on the left — so `+X = -n` on both. That cancellation is the whole
    /// reason OpenXR defines `+X` with an explicit per-hand flip: it makes a
    /// symmetric controller held the same way produce the same grip rotation in
    /// either hand, which is exactly the property that has been missing here.
    private static func anatomicalFrame(_ anchor: HandAnchor) -> simd_quatf? {
        guard let sk = anchor.handSkeleton else { return nil }
        let w = sk.joint(.wrist), i = sk.joint(.indexFingerKnuckle),
            l = sk.joint(.littleFingerKnuckle)
        guard w.isTracked, i.isTracked, l.isTracked else { return nil }
        // Anchor-local is enough: the frame is then rotated into the world by
        // the anchor, exactly as the joints are.
        func p(_ j: HandSkeleton.Joint) -> SIMD3<Float> {
            let c = j.anchorFromJointTransform.columns.3
            return SIMD3<Float>(c.x, c.y, c.z)
        }
        let W = p(w), I = p(i), L = p(l)
        let n = simd_cross(I - W, L - W)
        // A flat or degenerate triangle means the joints are stacked and the
        // normal is noise. Refuse rather than emit a frame that spins.
        guard simd_length(n) > 1e-6, simd_length(L - I) > 1e-4 else { return nil }
        let z = simd_normalize(L - I)
        let x = simd_normalize(-n)
        let y = simd_cross(z, x)            // unit already: x and z are orthogonal
        let local = simd_float3x3(columns: (x, y, z))
        return simd_quatf(anchor.originFromAnchorTransform) * simd_quatf(local)
    }

    /// The grip frame expressed in the anatomical frame — the one constant that
    /// relates the two, and the same for both hands by the argument above.
    ///
    /// **Learned from the right hand, applied to the left.** The right hand's
    /// wrist-based conversion is the one that has been observed correct on
    /// device, so it stays untouched and is the reference: whenever the right
    /// hand is tracked, `K = A_right⁻¹ · W_right · R` is measured and latched,
    /// and the left hand is then `A_left · K` — which is the left hand's
    /// anatomically equivalent grip, whatever Apple's wrist convention happens
    /// to be. Zero risk to the hand that already works, and no third guess at
    /// the hand that does not.
    ///
    /// Until the right hand has been seen once, the left falls back to the
    /// wrist conversion, i.e. today's behaviour. `KL_HAND_MIRROR=1` disables
    /// the whole mechanism and restores ALVR's mirrored pair.
    private var gripFromAnatomical: simd_quatf?

    /// The grip pose for a tracked hand: where a held hilt would be, and which
    /// way it would point.
    ///
    /// Position is the middle of the **middle-finger metacarpal** — the bone
    /// running through the palm — taken as the midpoint of its two ends. That
    /// is the bone a fist closes around, so a hilt placed there is a hilt being
    /// gripped rather than swinging about the wrist as the anchor origin does.
    ///
    /// **The offset is cached in the wrist's own frame, and that is not an
    /// optimisation.** Finger joints drop in and out of `isTracked` as the hand
    /// turns, and the old code fell back to the raw anchor origin whenever they
    /// did — a ~7 cm snap between two different definitions of "here", several
    /// times a second while rotating the hand. That is the hilts jumping
    /// around. Holding the last known wrist-local offset instead means losing
    /// the skeleton costs precision, not position.
    private func gripPose(_ anchor: HandAnchor, hand: Int) -> (SIMD3<Float>, simd_quatf) {
        let origin = anchor.originFromAnchorTransform
        let wristRot = simd_quatf(origin)

        // --- orientation.
        var q = wristRot * klGripFromWrist[hand]
        if Self.anatomical, let a = Self.anatomicalFrame(anchor) {
            if hand == 1 {
                // The reference hand. Measure the constant — and LOW-PASS it.
                // K is a constant of the two frames' relationship, so every
                // frame is another sample of the same number; taking the latest
                // one raw would pipe the right hand's joint noise straight into
                // the left hand's pose, which is a jitter with no visible
                // cause. Converges in about two seconds and is then steady.
                let sample = a.inverse * q
                if let k = gripFromAnatomical {
                    gripFromAnatomical = simd_slerp(k, sample, 0.02)
                } else {
                    gripFromAnatomical = sample
                    NSLog(String(format: "[cp] grip-from-anatomical latched from the right hand: "
                                 + "(%.4f, %.4f, %.4f, %.4f) — the left hand now follows it",
                                 sample.imag.x, sample.imag.y, sample.imag.z, sample.real))
                }
            } else if let k = gripFromAnatomical {
                q = a * k
            }
        }

        // --- position, in the wrist's frame so a dropout cannot move it.
        //
        // **Heavily smoothed, and frozen while the trigger is down.** In the
        // wrist's frame this offset is a bone: it is the same number every
        // frame for a given pair of hands, so anything that moves it is either
        // tracker noise or the hand changing shape — and the hand changing
        // shape is exactly what a pinch is. Letting it through means the
        // pointer's origin walks at the instant of the click, which is a ray
        // that leaves the button as you press it. That is indistinguishable
        // from an unreliable trigger and is not a trigger bug at all.
        if !pinchHeld[hand], let sk = anchor.handSkeleton {
            let base = sk.joint(.middleFingerMetacarpal)
            let tip  = sk.joint(.middleFingerKnuckle)
            if base.isTracked && tip.isTracked {
                let a = base.anchorFromJointTransform.columns.3
                let b = tip.anchorFromJointTransform.columns.3
                let m = SIMD3<Float>((a.x + b.x) * 0.5, (a.y + b.y) * 0.5, (a.z + b.z) * 0.5)
                // First sample lands, the rest converge — no visible settle,
                // and steady afterwards.
                gripOffset[hand] = gripOffset[hand].map { $0 + (m - $0) * 0.05 } ?? m
            }
        }
        let local = gripOffset[hand] ?? .zero
        let world = origin * SIMD4<Float>(local.x, local.y, local.z, 1)
        return Self.tuned(SIMD3<Float>(world.x, world.y, world.z), q, hand: hand)
    }

    /// Last known grip point in the wrist's own frame, per hand.
    private var gripOffset: [SIMD3<Float>?] = [nil, nil]

    /// KL_HAND_ANATOMICAL=1 — derive the left hand's frame from joint positions
    /// rather than from the constant above. Off by default: it is the more
    /// principled construction and it has never been confirmed on device, and a
    /// default nobody has looked at is not better than a constant three
    /// playtests point at.
    private static let anatomical =
        ProcessInfo.processInfo.environment["KL_HAND_ANATOMICAL"] == "1"

    // Hysteresis latch for the synthesised thumbstick-direction button bits, per
    // hand — so a stick held near the on/off threshold does not chatter the
    // GetDown/GetUp edges that snap turn keys on. Touched only from pollButtons.
    private static var lStickDir = (up: false, down: false, left: false, right: false)
    private static var rStickDir = (up: false, down: false, left: false, right: false)

    // MARK: - A plain gamepad, alongside the hands
    // Adapted from avp-play (MIT, github.com/myMartek/avp-play) visionos/Sources/KleptonControllers.swift.
    // Adapted from avp-play (MIT, github.com/myMartek/avp-play) visionos/Sources/KleptonControllers.swift.

    /// What an ordinary game controller contributes to one Touch controller:
    /// everything except a pose. See `pollGamepad`.
    struct PadHalf {
        var buttons: UInt32 = 0
        var touches: UInt32 = 0
        var indexTrigger: Float = 0
        var handTrigger: Float = 0
        var stick: SIMD2<Float> = .zero
    }
    private var pad = [PadHalf(), PadHalf()]
    private var padName: String?
    private var padDir = [(up: false, down: false, left: false, right: false),
                          (up: false, down: false, left: false, right: false)]

    /// KL_GAMEPAD=0 ignores a connected gamepad altogether.
    private static let gamepadEnabled = klEnvOn("KL_GAMEPAD", default: true)
    /// KL_GAMEPAD_PINCH=0 — with a gamepad connected, a pinch no longer pulls
    /// the trigger. For a game where fingers curled round the controller keep
    /// reading as pinches; the gamepad's own triggers still work.
    private static let gamepadKeepsPinch = klEnvOn("KL_GAMEPAD_PINCH", default: true)

    /// Which Touch face button each of the gamepad's four stands for.
    ///
    /// The default follows position: the lower right-hand button is A, the
    /// upper one B, and so on. A game can ask for a different layout where
    /// that puts an action under the wrong thumb on a pad — targets.py
    /// "gamepad_map", baked into Info.plist as KLGamepadMap, e.g.
    /// "square=b,circle=x". KL_GAMEPAD_MAP on a launch line overrides it. It
    /// applies to a plain gamepad ONLY: Touch and Sense controllers have the
    /// buttons the game was written for and keep them.
    struct FaceMap: Equatable {
        var cross = "a", circle = "b", square = "x", triangle = "y"

        /// "square=b,circle=x" laid over the default. Unknown names and values
        /// are ignored rather than guessed at.
        static func parse(_ text: String) -> FaceMap {
            var m = FaceMap()
            for pair in text.lowercased().split(separator: ",") {
                let kv = pair.split(separator: "=").map { $0.trimmingCharacters(in: .whitespaces) }
                guard kv.count == 2, ["a", "b", "x", "y"].contains(kv[1]) else { continue }
                switch kv[0] {
                case "cross", "south":    m.cross = kv[1]
                case "circle", "east":    m.circle = kv[1]
                case "square", "west":    m.square = kv[1]
                case "triangle", "north": m.triangle = kv[1]
                default: break
                }
            }
            return m
        }
    }
    static let faceMap: FaceMap = FaceMap.parse(
        ProcessInfo.processInfo.environment["KL_GAMEPAD_MAP"]
            ?? (Bundle.main.object(forInfoDictionaryKey: "KLGamepadMap") as? String) ?? "")

    /// The gamepad in use, for the settings page: its name, or nil.
    static func connectedGamepadName() -> String? { connectedGamepad()?.vendorName ?? connectedGamepad().map { _ in "Game controller" } }

    private static func connectedGamepad() -> GCController? {
        GCController.controllers().first { c in
            guard c.extendedGamepad != nil else { return false }
            if #available(visionOS 26.0, *) { return c.productCategory != GCProductCategorySpatialController }
            return true
        }
    }

    /// Read an ordinary gamepad (DualSense, DualShock 4, Xbox, any MFi
    /// extended gamepad) as the buttons, triggers and sticks of the two Touch
    /// controllers the guest expects.
    ///
    /// **It supplies no pose, and that is the point.** The hands keep doing
    /// what they do — where each controller is, and a pinch for its trigger —
    /// and the gamepad adds what a hand has no way to express: two analog
    /// sticks and the face buttons. That is the shape a game like Moss was
    /// first made for (one controller in the lap for the character, and a
    /// hand reaching into the world), and it needs no tracked controllers.
    ///
    /// The layout is the Touch one, read off the gamepad where each control
    /// sits under the same finger:
    ///
    ///     left stick  / L3         -> left thumbstick / click
    ///     right stick / R3         -> right thumbstick / click
    ///     Cross  (A, south)        -> A   (right controller, lower)
    ///     Circle (B, east)         -> B   (right controller, upper)
    ///     Square (X, west)         -> X   (left controller, lower)
    ///     Triangle (Y, north)      -> Y   (left controller, upper)
    ///     L2 / R2                  -> index triggers
    ///     L1 / R1                  -> hand triggers (grip)
    ///     Options / Create (Menu / View) -> Start (the menu button)
    ///     D-pad                    -> the left stick, while that is centred
    ///
    /// A Sense controller still wins for its hand (see `update`): this is
    /// merged only into a hand no tracked controller is driving.
    private func pollGamepad() {
        guard Self.gamepadEnabled, let c = Self.connectedGamepad(), let g = c.extendedGamepad else {
            if padName != nil {
                NSLog("[ctrl] gamepad gone — its buttons are released")
                padName = nil
            }
            pad = [PadHalf(), PadHalf()]
            return
        }
        if padName == nil {
            padName = c.vendorName ?? "gamepad"
            let m = Self.faceMap
            NSLog("[ctrl] gamepad: \(padName ?? "?") — sticks, face buttons, triggers and shoulders "
                  + "are merged into the Touch controllers alongside the hands (KL_GAMEPAD=0 turns this off); "
                  + "cross=\(m.cross) circle=\(m.circle) square=\(m.square) triangle=\(m.triangle)"
                  + (m == FaceMap() ? "" : " (this game's own layout)"))
        }

        var l = PadHalf(), r = PadHalf()
        l.stick = SIMD2(g.leftThumbstick.xAxis.value, g.leftThumbstick.yAxis.value)
        r.stick = SIMD2(g.rightThumbstick.xAxis.value, g.rightThumbstick.yAxis.value)
        // The D-pad has no counterpart on a Touch controller. Menus are walked
        // with the left stick, so that is what it stands in for.
        if simd_length_squared(l.stick) < 0.15 * 0.15 {
            let d = SIMD2(g.dpad.xAxis.value, g.dpad.yAxis.value)
            if simd_length_squared(d) > 0.25 { l.stick = d }
        }
        l.indexTrigger = g.leftTrigger.value;  r.indexTrigger = g.rightTrigger.value
        l.handTrigger = g.leftShoulder.isPressed ? 1 : 0
        r.handTrigger = g.rightShoulder.isPressed ? 1 : 0

        // The four face buttons, through this game's layout (see FaceMap). A
        // and B live in the right controller's word, X and Y in the left one.
        func press(_ touch: String) {
            switch touch {
            case "a": r.buttons |= OVRPRawButton.a; r.touches |= OVRPRawButton.a
            case "b": r.buttons |= OVRPRawButton.b; r.touches |= OVRPRawButton.b
            case "x": l.buttons |= OVRPRawButton.x; l.touches |= OVRPRawButton.x
            default:  l.buttons |= OVRPRawButton.y; l.touches |= OVRPRawButton.y
            }
        }
        let map = Self.faceMap
        if g.buttonA.isPressed { press(map.cross) }
        if g.buttonB.isPressed { press(map.circle) }
        if g.buttonX.isPressed { press(map.square) }
        if g.buttonY.isPressed { press(map.triangle) }
        if g.leftThumbstickButton?.isPressed == true  { l.buttons |= OVRPRawButton.lThumbstick }
        if g.rightThumbstickButton?.isPressed == true { r.buttons |= OVRPRawButton.rThumbstick }
        // A trigger that is merely rested on reads a few percent; the bit is
        // the click, so it waits for a real pull. The analog value is passed on
        // as it is.
        if l.indexTrigger > 0.1 { l.buttons |= OVRPRawButton.lIndexTrigger; l.touches |= OVRPRawButton.lIndexTrigger }
        if r.indexTrigger > 0.1 { r.buttons |= OVRPRawButton.rIndexTrigger; r.touches |= OVRPRawButton.rIndexTrigger }
        if l.handTrigger > 0 { l.buttons |= OVRPRawButton.lHandTrigger }
        if r.handTrigger > 0 { r.buttons |= OVRPRawButton.rHandTrigger }
        // Start on the LEFT word, where an OVRPlugin guest looks for it and
        // where the Touch profile has `menu/click`.
        if g.buttonMenu.isPressed || g.buttonOptions?.isPressed == true { l.buttons |= OVRPRawButton.start }

        // A thumb on the stick, and the direction bits snap turn and menus read
        // — both synthesised from the analog position exactly as for the Sense
        // pair, with their own latches so the two sources cannot fight.
        let onT: Float = 0.5, offT: Float = 0.4
        func dir(_ v: Float, _ prev: Bool) -> Bool { v >= (prev ? offT : onT) }
        for (hand, half) in [(0, l), (1, r)] {
            var h = half
            if simd_length_squared(h.stick) > 0.1 * 0.1 {
                h.touches |= hand == 0 ? OVRPRawButton.lThumbstick : OVRPRawButton.rThumbstick
            }
            let was = padDir[hand]
            let now = (up: dir(h.stick.y, was.up), down: dir(-h.stick.y, was.down),
                       left: dir(-h.stick.x, was.left), right: dir(h.stick.x, was.right))
            padDir[hand] = now
            if hand == 0 {
                if now.up    { h.buttons |= OVRPRawButton.lThumbstickUp }
                if now.down  { h.buttons |= OVRPRawButton.lThumbstickDown }
                if now.left  { h.buttons |= OVRPRawButton.lThumbstickLeft }
                if now.right { h.buttons |= OVRPRawButton.lThumbstickRight }
            } else {
                if now.up    { h.buttons |= OVRPRawButton.rThumbstickUp }
                if now.down  { h.buttons |= OVRPRawButton.rThumbstickDown }
                if now.left  { h.buttons |= OVRPRawButton.rThumbstickLeft }
                if now.right { h.buttons |= OVRPRawButton.rThumbstickRight }
            }
            pad[hand] = h
        }
    }

    /// Lay the gamepad's half over a hand's state: bits are OR'd, the triggers
    /// take the stronger of pinch and pull, and the stick is the gamepad's
    /// (a hand has none).
    private func mergePad(into st: inout KleptonHandState, hand: Int) {
        guard padName != nil else { return }
        let h = pad[hand]
        st.buttons |= h.buttons
        st.touches |= h.touches
        st.indexTrigger = max(st.indexTrigger, h.indexTrigger)
        st.handTrigger = max(st.handTrigger, h.handTrigger)
        st.stick = h.stick
    }

    // MARK: - Polling

    /// Poll the Sense controllers' buttons and lay them out as the guest's
    /// Touch controllers. Called once per frame from the render thread, before
    /// the guest's frame.
    ///
    /// SenseKit's `pollInput` does the GameController half — it (re)binds every
    /// spatial controller present NOW (connect / disconnect at any time, the
    /// bind arming kl_ovrp's near-touch edge through `onControllerBound`),
    /// reads each one through its input queue or `capture()`, and logs the
    /// census, the first press of each element and the late-connect case.
    /// What is left here is the `ovrpButton` layout.
    private func pollButtons() {
        // Clear the hands that have NO CONTROLLER ATTACHED, then fill in the
        // ones that do.
        //
        // The two halves of this seam have independent lifecycles and they do
        // not agree on timing: GameController connect/disconnect is one event
        // stream, ARKit's accessory `anchorUpdates` is another, and either can
        // lead. Filling in place without clearing means a hand whose controller
        // has gone away is simply never written — so its last frame's bits stay
        // pinned, `fromController` stays true, and the fallback in update()
        // never fires. A controller unpaired mid-press would leave the guest
        // holding that trigger down for the rest of the run.
        //
        // **Which is why the clear is keyed on ATTACHMENT and not, as it was,
        // on a new input state having arrived.** `nextInputState()` returns nil
        // when nothing has been queued since the last call — a controller
        // nobody is moving publishes nothing, because there is nothing to
        // publish — and a loop that `continue`d on that. With both hands
        // cleared up front, every such frame told the guest the exact opposite
        // of what was happening: buttons released, triggers at 0, stick
        // CENTRED, at whatever rate the poll outran the device.
        //
        // Both of the bugs that were reported as separate are this: SUPERHOT
        // retriggering its grab sound over and over while an object was held
        // (the grip read press/release/press, so the guest heard a new grab
        // each time), and Steam Link's stick snapping back to centre while
        // forward was held. One is a button and one is an axis, which is what
        // made them look like two problems; nil means "unchanged", and the only
        // faithful reading of it is to leave the last sample alone.
        //
        // Poses are deliberately NOT cleared here: they belong to the ARKit
        // stream, which reports its own `.removed`. Clearing them on the
        // GameController schedule would drop a pose that is still being
        // tracked.
        // SenseKit clears its own detached hands the same way; this clears the
        // guest-side state that goes with them.
        senseKit.pollInput()
        let input = [senseKit.inputState(.left), senseKit.inputState(.right)]

        lock.lock()
        for i in 0...1 where !input[i].connected {
            state[i].buttons = 0; state[i].touches = 0
            state[i].indexTrigger = 0; state[i].handTrigger = 0
            state[i].stick = .zero; state[i].fromController = false
            // Only the Sense path writes these, and it rewrites them below;
            // a hand that falls back must report no motion rather than the
            // motion its controller had before it was put down.
            state[i].linearVelocity = .zero; state[i].angularVelocity = .zero
        }
        lock.unlock()

        for hand in 0...1 where input[hand].connected {
            let sk = input[hand]
            let side: SenseHand = hand == 0 ? .left : .right

            // **The Oculus layout, laid over the Sense one.**
            //
            //   Menu  (GCInputButtonMenu)  -> Menu `menu/click`   (RAW_START)
            //                              -> AND Home `system/click` (RAW_BACK)
            //   Share (GCInputButtonShare) -> Menu, if it exists
            //   Home  (GCInputButtonHome)  -> Home, if it exists
            //
            // On an Oculus Touch pair the LEFT controller's Menu button is
            // `Start` and the RIGHT controller's Oculus button is `Home`, and
            // the Oculus button is the one that summons a dashboard. Steam Link
            // suggests `system/click` for it, one of the 39 of 41 bindings it
            // takes. Without RAW_BACK set here that action is live, active and
            // permanently false, which has no symptom: it decodes correctly
            // and never fires.
            //
            // **`Button Menu` drives both, and that is ALVR's shape** (218ee37
            // moved to exactly this after having each hand's button on one bit).
            // The Sense pair reports only ONE non-game button per controller and
            // reports it under that one name, so mapping it to `system` alone
            // would take the menu button away to gain a dashboard button. Both
            // costs nothing: the Touch profile splits the pair across the hands
            // — LEFT has `menu/click` and no system, RIGHT has `system/click`
            // and no menu — so a hand with no binding for a control has nothing
            // to take the bit, and on the OVRPlugin path OVRInput ORs the two
            // controllers anyway.
            //
            // **The real PlayStation button may not reach us at all.** It is
            // `GCInputButtonHome`, and visionOS 26 hands it to the system the
            // way it hands over the Digital Crown; taking it needs
            // `GCControllerHomeButtonSetting.inAppAction`, which does not exist
            // in the XROS 26.0 SDK this tree builds against. Read anyway, so a
            // visionOS 27 build gets it the moment the system stops eating it —
            // and SenseKit's first-press log is what will say when that happens,
            // which is worth more than a guessed selector.
            // The Sense Create (L) / Options (R) buttons both arrive as
            // "Button Menu". Split by HAND, per the user's spec:
            //  - LEFT (Create) keeps the original behaviour: menu AND system
            //    together, exactly as it always was.
            //  - RIGHT (Options) is the Oculus MENU button (start) alone - the
            //    old OR also fired system on every press, which read as "not
            //    sure what this button does".
            //  - A real Home/PS element, if this visionOS delivers one (check
            //    the "[cp] ... elements:" line), is the system/home bit alone -
            //    the SteamVR dashboard, the Oculus home.
            //
            // In SenseKit's physical names: "Button Menu" is `.create` on the
            // left and `.options` on the right, "Button Share" / "Button
            // Options" are `.options` on either hand, and Home / PS is `.ps`.
            let menuish = !sk.buttons.isDisjoint(with: [.create, .options])
            let psish = sk.buttons.contains(.ps)
            let homeish = (hand == 0 && sk.buttons.contains(.create)) || psish

            var st = KleptonHandState()
            st.fromController = true
            st.indexTrigger = sk.trigger
            st.handTrigger  = sk.grip
            st.stick = sk.stick

            var bits: UInt32 = 0, touch: UInt32 = 0
            // **A thumb on the stick**, which nothing reported until Half-Life:
            // Alyx needed it. SteamVR's Touch bindings put teleport and snap
            // turn on the stick as a dpad with `sub_mode: touch` — a direction
            // counts only while the stick is being TOUCHED — and with this bit
            // never set every direction was permanently off: the hands tracked,
            // the buttons worked, and the player could neither turn nor move
            // (xrizer's dpad activation, bound to thumbstick/touch: 0 fired in
            // a whole session). The element's own capacitive reading where the
            // controller gives one, and otherwise the plain fact that a stick
            // cannot be off-centre without a thumb on it.
            // Adapted from avp-play (MIT, github.com/myMartek/avp-play)
            // visionos/Sources/KleptonControllers.swift. KL_SENSE_STICK_TOUCH:
            // default on except for our Alyx (OpenVR shim), whose input is
            // tuned without it.
            if Self.stickTouch, sk.touches.contains(.stickClick)
                || st.stick.x * st.stick.x + st.stick.y * st.stick.y > 0.1 * 0.1 {
                touch |= hand == 0 ? OVRPRawButton.lThumbstick : OVRPRawButton.rThumbstick
            }
            // The Sense controllers name their face buttons A and B on both
            // hands (SenseKit: lower / upper face — Square / Triangle on the
            // left, Cross / Circle on the right); the guest's left hand calls
            // them X and Y.
            let lower = SenseButtons.lowerFace(side), upper = SenseButtons.upperFace(side)
            let trigTouched = Self.triggerTouch
                && (sk.touches.contains(.trigger) || st.indexTrigger > Self.triggerTouchMin)
            if hand == 0 {
                if sk.buttons.contains(lower) { bits |= OVRPRawButton.x }
                if sk.buttons.contains(upper) { bits |= OVRPRawButton.y }
                if sk.touches.contains(lower) { touch |= OVRPRawButton.x }
                if sk.touches.contains(upper) { touch |= OVRPRawButton.y }
                if trigTouched { touch |= OVRPRawButton.lIndexTrigger }
                if sk.buttons.contains(.stickClick) { bits |= OVRPRawButton.lThumbstick }
                if st.indexTrigger > Self.triggerPressMin { bits |= OVRPRawButton.lIndexTrigger }
                if st.handTrigger  > 0 { bits |= OVRPRawButton.lHandTrigger }
                if menuish { bits |= OVRPRawButton.start }
                if homeish { bits |= OVRPRawButton.back }
            } else {
                if sk.buttons.contains(lower) { bits |= OVRPRawButton.a }
                if sk.buttons.contains(upper) { bits |= OVRPRawButton.b }
                if sk.touches.contains(lower) { touch |= OVRPRawButton.a }
                if sk.touches.contains(upper) { touch |= OVRPRawButton.b }
                if trigTouched { touch |= OVRPRawButton.rIndexTrigger }
                if sk.buttons.contains(.stickClick) { bits |= OVRPRawButton.rThumbstick }
                if st.indexTrigger > Self.triggerPressMin { bits |= OVRPRawButton.rIndexTrigger }
                if st.handTrigger  > 0 { bits |= OVRPRawButton.rHandTrigger }
                // Both bits on both hands. The Oculus Touch interaction profile
                // splits the pair — the LEFT carries `menu/click` and no
                // system, the RIGHT carries `system/click` and no menu — so a
                // hand whose profile does not have the control simply has no
                // binding to take the bit and cannot cross-fire; and on the
                // OVRPlugin path OVRInput ORs the two controllers anyway, so
                // which hand carried it was never observable there either.
                // Publishing both is what makes the mapping independent of
                // which Sense controller a given physical button sits on.
                // RIGHT hand Options = the RIGHT system click, plainly - the
                // Oculus button's slot on the Touch profile
                // (oculustouch_right_system_click where Steam Input offers it).
                // Delivered as this hand's own system bit; what it does is
                // whatever the user binds to it on the PC.
                //
                // ...and START as well, so the Options pill is a menu button in
                // its own right, the way Create is on the left. An OVRPlugin
                // guest (Walkabout, Beat Saber) reads Start for its pause/menu
                // and never looks at the system bit, so with `back` alone the
                // right pill did nothing there. The Touch profile has no
                // menu/click on the right hand, so on the OpenXR path this bit
                // still has no binding to land on and Steam Link is unchanged;
                // fill_controller_state (kl_ovrp.c) folds it into the left
                // hand's word for a guest that polls the hands separately.
                // (avp-play) KL_SENSE_RIGHT_START: default on except for our
                // Alyx (OpenVR shim), where the right pill stays `back` only.
                if menuish { bits |= (Self.rightStart ? OVRPRawButton.start : 0) | OVRPRawButton.back }
                if homeish { bits |= OVRPRawButton.back }
            }
            // The guest reads stick *direction* bits as well as the axes — and
            // SNAP TURN reads only these (OVRInput.GetDown(Secondary/Primary
            // ThumbstickRight)), never the axis. They MUST come from the analog
            // stick position, not from discrete "Thumbstick Left/Right" GC
            // elements: a PSVR2 Sense / Touch stick is one analog DPAD element
            // that never fires those discrete presses, so deriving the bits from
            // `pressed(...)` left them permanently 0 and snap turn did nothing —
            // exactly Red Matter 2's symptom, while smooth-turn titles (Into The
            // Radius) that read the axis were unaffected. Real Oculus firmware
            // synthesises these bits from the stick past a threshold; do the
            // same, with hysteresis so a stick resting near the edge does not
            // chatter GetDown/GetUp. Both hands, so left-stick snap turn works too.
            let onT: Float = 0.5, offT: Float = 0.4
            func dirBit(_ v: Float, _ prev: Bool) -> Bool {
                return v >= (prev ? offT : onT)
            }
            let sx = st.stick.x, sy = st.stick.y
            let latch = hand == 0 ? Self.lStickDir : Self.rStickDir
            var nd = (up: false, down: false, left: false, right: false)
            nd.right = dirBit( sx, latch.right)
            nd.left  = dirBit(-sx, latch.left)
            nd.up    = dirBit( sy, latch.up)
            nd.down  = dirBit(-sy, latch.down)
            if hand == 0 {
                Self.lStickDir = nd
                if nd.up    { bits |= OVRPRawButton.lThumbstickUp }
                if nd.down  { bits |= OVRPRawButton.lThumbstickDown }
                if nd.left  { bits |= OVRPRawButton.lThumbstickLeft }
                if nd.right { bits |= OVRPRawButton.lThumbstickRight }
            } else {
                Self.rStickDir = nd
                if nd.up    { bits |= OVRPRawButton.rThumbstickUp }
                if nd.down  { bits |= OVRPRawButton.rThumbstickDown }
                if nd.left  { bits |= OVRPRawButton.rThumbstickLeft }
                if nd.right { bits |= OVRPRawButton.rThumbstickRight }
            }
            // A pressed button is a touched one. The capacitive touch inputs
            // above are additive to that, not a replacement: OVRInput treats
            // touch as a superset of press and a press that is not also a touch
            // reads as a hardware fault.
            //
            // `touches` stays in the ovrpButton layout on purpose: it is
            // "which of these controls is touched", which kl_openxr reads as is.
            // The guest's ovrpTouch / ovrpNearTouch words are a different
            // layout and are built from it in ONE place, kl_ovrp's
            // fill_controller_state (kl_ovrp_touch_word). Copying this word into
            // them verbatim is what put IEYTD's left thumb on the right B.
            st.buttons = bits
            st.touches = touch | bits

            // Buttons only. The pose is resolved in update(), where the frame's
            // presentation time is known and the anchor can be predicted to it.
            lock.lock()
            state[hand] = st
            lock.unlock()
        }
    }

    // MARK: - Haptics

    //  The only seam here that runs OUT of the guest. Beat Saber queues an
    //  amplitude envelope through OVRPlugin's buffered haptics API and
    //  `kl_ovrp_haptics_pull` hands it over a pulse at a time (kl_ovrp.c holds
    //  the whole contract); SenseKit plays those pulses on the Sense controllers.
    //
    //  **A hand cannot be vibrated.** There is no actuator on the hand-tracking
    //  path and nothing on the headset stands in for one, so a hand-tracked run
    //  drains the queue and drops it. That is not a gap to fill later — it is
    //  what the hardware is.
    //
    //  The engine comes from ALVR (`GCController.haptics` →
    //  `createEngine(withLocality:)`), the *driving* does not, and the
    //  difference was forced by a headset. ALVR's source is discrete events
    //  from a server — one amplitude, one duration — so it plays each as its
    //  own short `.hapticContinuous`. Ours is a continuous 320 Hz envelope, and
    //  playing that as a run of short events is a restart per frame: a held
    //  buzz survives it, a note cut comes out as a blip with no decay.
    //
    //  So: **one long-lived looping player per hand, and the envelope is sent
    //  to it as a dynamic intensity parameter each frame.** No restarts, no
    //  gaps, and the shape of the clip is what reaches the hand. The player is
    //  paused after a second of silence and resumed on demand, so an idle run
    //  is not holding an actuator open.
    //
    //  `KL_HAPTICS_PULSE=1` restores the discrete shape (ALVR's, plus its 32 ms
    //  floor) — the A/B if the continuous player ever misbehaves on hardware,
    //  and the automatic fallback if the advanced player cannot be made.

    //  The engine and the driving now live in SenseKit (SenseHaptics.swift);
    //  what stays here is the pull and the KL_HAPTICS_* knobs below.

    /// `KL_HAPTICS_OFF=1` turns the path off outright — the pull still runs, so
    /// the guest's queue cannot back up, but nothing is played.
    private static let hapticsEnabled = !klEnvOn("KL_HAPTICS_OFF", default: false)
    /// `KL_HAPTICS_GAIN` scales every pulse. Default 0.25, which is ALVR's own
    /// PSVR2 scale (it uses 0.7 for everything else): the Sense actuators are
    /// strong enough that a guest amplitude of 1.0 played at full intensity is
    /// not what the guest meant by it.
    private static let gain = klEnvFloat("KL_HAPTICS_GAIN", 0.25)
    /// `KL_HAPTICS_SHARPNESS` — CoreHaptics's second axis, which OVRPlugin has
    /// no equivalent of (see kl_ovrp.h on why frequency is not in the seam).
    /// 1.0 is ALVR's constant: crisp, which is what a note cut is.
    private static let sharpness = klEnvFloat("KL_HAPTICS_SHARPNESS", 1.0)
    /// `KL_HAPTICS_PULSE=1` — drive with discrete per-frame events (ALVR's
    /// shape) instead of one continuous player. The A/B if the buzz is ever
    /// wrong in a way the envelope does not explain, and what a hand falls back
    /// to on its own if the continuous player cannot be made or started.
    private static let pulseMode = klEnvOn("KL_HAPTICS_PULSE", default: false)
    /// The floor a discrete event is never shorter than — ALVR's "controllers
    /// can't do 10ms vibrations". The continuous path gets the same floor from
    /// kl_ovrp's side, where it is a hold on the level instead.
    /// `KL_HAPTICS_MIN_MS` moves both.
    private static let minOnMs = klEnvFloat("KL_HAPTICS_MIN_MS", 32)

    /// The knobs above as SenseKit's haptics configuration.
    private static var hapticsConfiguration: SenseHapticsConfiguration {
        SenseHapticsConfiguration(enabled: hapticsEnabled, gain: gain, sharpness: sharpness,
                                  mode: pulseMode ? .pulse : .continuous,
                                  minPulseMs: Double(minOnMs))
    }

    /// Follow both hands' envelopes. Called once a frame from the compositor,
    /// beside `update`.
    ///
    /// The pull is a LEVEL — what the hand should be feeling right now — not an
    /// event to schedule. SenseKit drains it (through `hapticsSource`, i.e.
    /// `kl_ovrp_haptics_pull`) unconditionally, even with playback off, which
    /// keeps the two sides in step, and plays it on a queue of its own so the
    /// render loop never waits for the haptics service (see
    /// `SenseControllers.pumpHaptics`).
    func pumpHaptics() {
        senseKit.pumpHaptics()
    }

    /// Every engine, for a disconnect or teardown.
    func releaseHaptics() {
        senseKit.releaseHaptics()
    }

    // MARK: - The seam

    /// The index trigger, from a pinch.
    ///
    /// A hand has no buttons, which used to mean a hand-tracked run could reach
    /// the menu and then not click anything in it. The thumb-to-index distance
    /// is the one continuous quantity a hand does supply, and it maps onto a
    /// trigger the way a trigger already behaves.
    ///
    /// **It is a TWO-point calibration, and the closed end is the one that
    /// matters.** The first version was `1 - d / 5cm`, which reads as "touching
    /// is fully pressed" and is wrong about what touching means: `thumbTip` and
    /// `indexFingerTip` are skeletal joints inside fingers that have thickness,
    /// so a hard pinch bottoms out around 1.5 cm apart, not 0. That version
    /// therefore topped out near 0.7 — enough that the guest saw the axis move
    /// (the laser changed with it) and never enough for a full press. It read
    /// as a missing button bit and was a missing 30% of range.
    ///
    /// So: `KL_PINCH_CLOSED` metres (default 1.5 cm) or nearer is 1.0,
    /// `KL_PINCH_OPEN` (default 5 cm) or further is 0.0, linear between. Both
    /// are per-user — fingers differ — which is why they are knobs and why
    /// `KL_PINCH_TRACE=1` reports the distance actually measured.
    ///
    /// The *bit* gets hysteresis; the axis does not. `RawButton.RIndexTrigger`
    /// is what a UI click is edged off, and a bare threshold on a noisy
    /// millimetre of fingertip jitter emits a burst of press/release pairs at
    /// exactly the moment the user meant one click.
    private func pinchTrigger(_ anchor: HandAnchor, hand: Int) -> Float {
        guard let sk = anchor.handSkeleton else { pinchNoSkeleton[hand] += 1; return 0 }
        let thumb = sk.joint(.thumbTip), index = sk.joint(.indexFingerTip)
        guard thumb.isTracked && index.isTracked else {
            pinchNoSkeleton[hand] += 1; return 0
        }
        let o = anchor.originFromAnchorTransform
        let a = (o * thumb.anchorFromJointTransform).columns.3
        let b = (o * index.anchorFromJointTransform).columns.3
        let d = simd_length(SIMD3<Float>(a.x - b.x, a.y - b.y, a.z - b.z))
        let t = max(0, min(1, (Self.pinchOpen - d) / (Self.pinchOpen - Self.pinchClosed)))
        tracePinch(hand: hand, distance: d, value: t)
        return t
    }

    /// The distance that reads as a fully released trigger. KL_PINCH_OPEN.
    private static let pinchOpen: Float = envMetres("KL_PINCH_OPEN", 0.05)
    /// ...and the one that reads as fully pressed. KL_PINCH_CLOSED. Kept below
    /// `pinchOpen` whatever the environment says, because the two are divided.
    ///
    /// **3 cm, raised from the original 1.5.** A device playtest got through
    /// the menus only after raising this by hand, which is a measurement: these
    /// joints bottom out much further apart than the first guess assumed. It is
    /// a *floor*, not a threshold — anything closer also reads 1.0 — so erring
    /// wide costs a little sensitivity and erring narrow costs the click.
    private static let pinchClosed: Float = min(envMetres("KL_PINCH_CLOSED", 0.03),
                                                pinchOpen - 0.005)

    /// KL_PINCH_SRC — which source is allowed to press the button. Default
    /// `both`; `system` or `distance` isolates one when the question is which
    /// of the two is misbehaving.
    private enum PinchSource { case both, system, distance }
    private static let pinchSrc: PinchSource = {
        switch ProcessInfo.processInfo.environment["KL_PINCH_SRC"] {
        case "system":   NSLog("[cp] KL_PINCH_SRC=system — system pinch events only"); return .system
        case "distance": NSLog("[cp] KL_PINCH_SRC=distance — fingertip distance only"); return .distance
        default: return .both
        }
    }()

    /// Which source pressed, counted per report interval — so "the system is
    /// dropping presses" and "the distance never gets there" stop being one
    /// sentence of playtest feedback.
    private var pinchFromSystem = [0, 0]
    private var pinchFromDistance = [0, 0]
    /// Spatial events seen, and those the system could not attribute to a hand.
    private var spatialEvents = 0
    private var spatialNoChirality = 0

    private static func envMetres(_ key: String, _ fallback: Float) -> Float {
        let v = ProcessInfo.processInfo.environment[key].flatMap { Float($0) } ?? 0
        return (v > 0 && v < 0.5) ? v : fallback
    }

    /// The closest pinch seen per hand, the axis value it produced, and whether
    /// the bit was ever set — every two seconds.
    ///
    /// **On by default** (`KL_PINCH_TRACE=0` silences it), because two device
    /// playtests have now reported "the trigger does not click" and neither
    /// could say *which* of the four things in the chain was missing: the
    /// skeleton, the joints, the range, or the threshold. One line every two
    /// seconds distinguishes all four, and this is a bring-up seam where that
    /// is worth more than the log space. `pinches` counts frames where the bit
    /// was actually asserted, so "nothing reached the guest" and "the guest
    /// ignored it" stop looking the same.
    private var pinchMin: [Float] = [9, 9]
    private var pinchPeak: [Float] = [0, 0]
    private var pinchFires: [Int] = [0, 0]
    private var pinchNoSkeleton: [Int] = [0, 0]
    private var pinchNextLog: TimeInterval = 0
    private func tracePinch(hand: Int, distance: Float, value: Float) {
        pinchMin[hand] = min(pinchMin[hand], distance)
        pinchPeak[hand] = max(pinchPeak[hand], value)
    }
    private func reportPinch() {
        guard Self.pinchTrace else { return }
        let now = ProcessInfo.processInfo.systemUptime
        guard now >= pinchNextLog else { return }
        pinchNextLog = now + 2
        // A "closest" of 9 means the distance was never computed at all — no
        // skeleton, or the two fingertip joints were not tracked. That is a
        // different bug from a pinch that is measured and too wide, and it is
        // the one the value alone cannot show.
        func fmt(_ h: Int) -> String {
            pinchMin[h] > 8
                ? String(format: "no skeleton (%d frames)", pinchNoSkeleton[h])
                : String(format: "%.3f m -> %.2f, %d down (sys %d, dist %d)",
                         pinchMin[h], pinchPeak[h], pinchFires[h],
                         pinchFromSystem[h], pinchFromDistance[h])
        }
        NSLog("[cp] pinch: L \(fmt(0)) | R \(fmt(1)) "
              + String(format: "(open %.3f closed %.3f, %d spatial events, %d unattributed)",
                       Self.pinchOpen, Self.pinchClosed, spatialEvents, spatialNoChirality))
        pinchMin = [9, 9]; pinchPeak = [0, 0]
        pinchFires = [0, 0]; pinchNoSkeleton = [0, 0]
        pinchFromSystem = [0, 0]; pinchFromDistance = [0, 0]
        spatialEvents = 0; spatialNoChirality = 0
    }
    private static let pinchTrace =
        ProcessInfo.processInfo.environment["KL_PINCH_TRACE"] != "0"

    /// Debounced press state for the *fallback* pinch trigger, per hand.
    private var pinchHeld = [false, false]

    /// Whether a hand is currently NOT hand-tracked because the other one holds
    /// a controller (see `update`). Only for the one-line transition log.
    private var handSuppressed = [false, false]
    /// KL_HANDS_WITH_CONTROLLER=1 — keep hand tracking on a hand with no
    /// controller even while the other hand has one (the old behaviour).
    ///
    /// Adapted from avp-play (MIT, github.com/myMartek/avp-play)
    /// visionos/Sources/KleptonControllers.swift. Default: the old per-hand
    /// fallback for every pre-existing target; suppression only for avp-play's
    /// targets (klAvpPlayTarget). KL_HANDS_WITH_CONTROLLER=0 suppresses anywhere.
    private static let handsWithController =
        klEnvOn("KL_HANDS_WITH_CONTROLLER", default: !klAvpPlayTarget())
    /// See the thumbstick-touch and right-menu notes in pollButtons.
    static let stickTouch = klEnvOn("KL_SENSE_STICK_TOUCH", default: klTargetName() != "alyx")
    static let rightStart = klEnvOn("KL_SENSE_RIGHT_START", default: klTargetName() != "alyx")
    /// **A finger resting on the trigger.** Games that pose the hand from the
    /// trigger's capacitive touch (I Expect You To Die points the index finger
    /// while it is OFF the trigger) only ever saw "touched" while the trigger
    /// was being pulled, and a trigger whose resting value is not exactly 0
    /// (one Sense controller reads a little above it) read as permanently
    /// pulled, so that hand would not point. Use the Sense's own capacitive
    /// reading on "Trigger", plus a small pull for controllers without one.
    /// KL_SENSE_TRIGGER_TOUCH: default on except for our Alyx (OpenVR shim).
    static let triggerTouch = klEnvOn("KL_SENSE_TRIGGER_TOUCH", default: klTargetName() != "alyx")
    /// Pull below which the trigger is neither "touched" (without a capacitive
    /// reading) nor "pressed": swallows the resting offset of a worn trigger.
    static let triggerTouchMin: Float = 0.05
    static let triggerPressMin: Float = klTargetName() == "alyx" ? 0 : 0.05

    // MARK: - The system pinch

    /// Whether the system says each hand is currently pinching, and whether it
    /// has ever said anything at all.
    ///
    /// The second flag matters: until an event arrives we cannot tell "not
    /// pinching" from "this callback is never going to fire", and those need
    /// different behaviour. Before the first event the fingertip-distance
    /// fallback still drives the button; after it, the system is the only thing
    /// that presses, because two sources of truth for one bit is how a click
    /// ends up half-registered.
    private var systemPinch = [false, false]
    private var systemPinchSeen = false

    /// `LayerRenderer.onSpatialEvent`. Called on the system's own queue, hence
    /// the lock.
    ///
    /// Only `.indirectPinch` is taken. `.directPinch` is a finger touching a
    /// real surface and `.pointer`/`.touch` are other devices entirely; the
    /// guest's index trigger means "the user is selecting", which is what an
    /// indirect pinch is.
    func handleSpatialEvents(_ events: SpatialEventCollection) {
        for e in events {
            guard e.kind == .indirectPinch else { continue }
            // No chirality means the system could not attribute the pinch to a
            // hand. Attributing it to the right is the same choice the accessory
            // path already makes, and for the same reason: one-handed use is
            // overwhelmingly the right hand.
            let hand: Int
            var unattributed = false
            switch e.chirality {
            case .left:  hand = 0
            case .right: hand = 1
            default:
                hand = 1
                unattributed = true
                if !loggedNoChirality {
                    loggedNoChirality = true
                    NSLog("[cp] spatial pinch with no chirality — attributed to the right hand")
                }
            }
            let active = e.phase == .active
            lock.lock()
            spatialEvents += 1
            if unattributed { spatialNoChirality += 1 }
            if !systemPinchSeen {
                systemPinchSeen = true
                NSLog("[cp] system pinch events are live — the fingertip-distance "
                      + "fallback no longer presses anything")
            }
            systemPinch[hand] = active
            lock.unlock()
        }
    }
    private var loggedNoChirality = false

    /// Merge every source and push the result across to kl_ovrp.
    ///
    /// The hand anchors are the fallback: used for a hand's *pose* and its
    /// pinch trigger, and only where a Sense controller has not supplied one.
    ///
    /// `presentationTime` is when the frame being built will be shown. Every
    /// pose here is predicted to it, so the controllers move at display rate
    /// and describe the same instant the head pose does.
    func update(leftHand: HandAnchor?, rightHand: HandAnchor?,
                at presentationTime: TimeInterval) {
        pollButtons()
        pollGamepad()

        // **Hands are the fallback for NO controllers, not for a missing one.**
        // With a single Sense controller in use (Walkabout is played one-handed)
        // the empty hand used to fall through to hand tracking, and its pinch
        // is a trigger press on the other Touch controller — which the guest
        // reads as "the player switched hands", over and over. So once either
        // hand has a controller, a hand without one is not tracked at all.
        // KL_HANDS_WITH_CONTROLLER=1 restores the per-hand fallback.
        let sense = [senseKit.pose(.left, at: presentationTime),
                     senseKit.pose(.right, at: presentationTime)]
        lock.lock()
        let wired = [state[0].fromController, state[1].fromController]
        lock.unlock()
        let anyController = !Self.handsWithController
            && (wired[0] || wired[1] || sense[0] != nil || sense[1] != nil)

        for hand in 0...1 {
            lock.lock()
            var st = state[hand]
            // Any sign of a hand ON this controller — a button, a capacitive
            // touch, the trigger or grip, the stick off-centre — for the ghost
            // controllers (KleptonHUD): input means held. From the latched
            // sample, so a button held down keeps counting while no new input
            // state arrives.
            if st.fromController && (st.buttons != 0 || st.touches != 0 || st.indexTrigger > 0.05
                                     || st.handTrigger > 0.05 || simd_length(st.stick) > 0.2) {
                lastActive[hand] = CACurrentMediaTime()
            }
            lock.unlock()

            let accPose = sense[hand]
            if let accPose {
                handSuppressed[hand] = false
                st.position = accPose.position; st.orientation = accPose.orientation
                st.linearVelocity = accPose.linearVelocity; st.angularVelocity = accPose.angularVelocity
            } else if anyController && !st.fromController {
                // Released and parked, once, on the way in: whatever the pinch
                // last held must not stay held, and a hand frozen where
                // tracking left it (or kl_ovrp's synthesised one in front of
                // the face) would be a second hand hanging in the picture. Ten
                // metres under the floor is out of every view.
                if !handSuppressed[hand] {
                    handSuppressed[hand] = true
                    NSLog("[ctrl] hand \(hand == 0 ? "L" : "R"): a controller is "
                          + "connected on the other hand — hand tracking for this "
                          + "one is off (KL_HANDS_WITH_CONTROLLER=1 restores it)")
                }
                pinchHeld[hand] = false
                kl_ovrp_set_hand_motion(Int32(hand), 0, -10, 0, 0, 0, 0, 1,
                                        0, 0, 0, 0, 0, 0)
                kl_ovrp_set_controller_input(Int32(hand), 0, 0, 0, 0, 0, 0)
                continue
            } else if let anchor = (hand == 0 ? leftHand : rightHand) {
                handSuppressed[hand] = false
                let (p, q) = gripPose(anchor, hand: hand)
                st.position = p; st.orientation = q
                // A Sense controller wrote this hand's input, but its pose
                // dropped out for this frame. The hand may stand in for the
                // POSE, never for the input: fingers wrapped round a controller
                // measure as a pinch, and the pinch below replaced the real
                // trigger with that distance — random clicks, and real ones
                // lost (Tea For God's menu: "12 down" on a hand holding a Sense).
                if st.fromController {
                    pinchHeld[hand] = false
                } else {
                // pollButtons() has already cleared this hand's bits (no Sense
                // controller wrote it), so everything the pinch sets is set
                // here and nothing has to be undone.
                let t = pinchTrigger(anchor, hand: hand)
                lock.lock()
                let sysSeen = systemPinchSeen, sysDown = systemPinch[hand]
                lock.unlock()

                // **Both sources, OR'd**, and that is deliberate rather than
                // indecisive. Each has now been observed unreliable on its own:
                // thresholding the fingertip distance chattered, and the
                // system's own recogniser drops presses in here too — plausibly
                // because `.mixed` immersion leaves the system competing for
                // the same gesture (try KL_FULL=1), or because a pinch it
                // cannot attribute to a hand is not one we can use. Their
                // failures are not correlated: the system misses presses the
                // distance sees, and the distance chatters where the system's
                // hysteresis is clean. A union is more reliable than either,
                // and for a *button* a union is also the right shape — the cost
                // of a spurious release is a dropped click, the cost of a
                // spurious press is nothing the user will notice.
                //
                // KL_PINCH_SRC=system|distance isolates one when the question
                // is which of them is misbehaving.
                let byDistance = Self.pinchSrc != .system
                    && (pinchHeld[hand] ? (t > 0.30) : (t > 0.55))
                let bySystem = Self.pinchSrc != .distance && sysSeen && sysDown
                // With a gamepad in use the pinch can be switched off (see
                // gamepadKeepsPinch): the hand then only says where it is.
                let pinchAllowed = padName == nil || Self.gamepadKeepsPinch
                let pressed = pinchAllowed && (byDistance || bySystem)
                if bySystem { pinchFromSystem[hand] += 1 }
                if byDistance { pinchFromDistance[hand] += 1 }

                // The analog value stays the measured distance — the guest's
                // laser reacts to it and a trigger that snaps 0->1 loses that —
                // but a press pins it to 1.0 so the axis can never disagree
                // with the bit.
                st.indexTrigger = pressed ? 1 : (pinchAllowed ? t : 0)
                pinchHeld[hand] = pressed
                if pressed {
                    pinchFires[hand] += 1
                    let bit = hand == 0 ? OVRPRawButton.lIndexTrigger
                                        : OVRPRawButton.rIndexTrigger
                    st.buttons |= bit; st.touches |= bit
                }
                // The gamepad's sticks and buttons, on top of what the hand said.
                mergePad(into: &st, hand: hand)
                }
            } else {
                // Nothing tracked this hand. Leave kl_ovrp's own synthesised
                // head-relative hand alone rather than pushing a stale pose —
                // it at least keeps the controllers inside the frustum.
                //
                // ...but a Sense controller's BUTTONS are pose-independent, and
                // this branch is exactly where they were being dropped: holding
                // the controllers mutes visionOS hand tracking (no anchors), and
                // when the accessory pose is also absent this `continue` skipped
                // the input push - pollButtons() had the trigger, the guest
                // never saw it, and a menu gated on "press trigger" waited
                // forever. Push the buttons; only the pose stays synthesised.
                //
                // The same holds for a gamepad, and more often: two hands
                // wrapped round a controller in the lap are exactly the hands
                // the tracker loses. Its buttons must not wait for a hand to be
                // seen again.
                if !st.fromController, padName != nil {
                    pinchHeld[hand] = false
                    mergePad(into: &st, hand: hand)
                }
                if st.fromController || padName != nil {
                    pushInput(hand, st)
                    lock.lock(); state[hand] = st; lock.unlock()
                }
                continue
            }

            if let p = st.position, let q = st.orientation {
                kl_ovrp_set_hand_motion(Int32(hand), p.x, p.y, p.z,
                                        q.imag.x, q.imag.y, q.imag.z, q.real,
                                        st.linearVelocity.x, st.linearVelocity.y,
                                        st.linearVelocity.z,
                                        st.angularVelocity.x, st.angularVelocity.y,
                                        st.angularVelocity.z)
            }
            pushInput(hand, st)

            lock.lock(); state[hand] = st; lock.unlock()
        }
        reportPinch()
        KLQuickSettings.shared.processInput()
    }

    /// The buttons, triggers and stick to the guest — unless the in-game quick
    /// settings own them (KleptonQuickSettings.swift): then the guest sees a
    /// released controller (the pose still goes through) and the menu reads it.
    private func pushInput(_ hand: Int, _ st: KleptonHandState) {
        if KLQuickSettings.shared.capture(hand: hand, st) {
            kl_ovrp_set_controller_input(Int32(hand), 0, 0, 0, 0, 0, 0)
            return
        }
        kl_ovrp_set_controller_input(Int32(hand), st.buttons, st.touches,
                                     st.indexTrigger, st.handTrigger,
                                     st.stick.x, st.stick.y)
    }
}
