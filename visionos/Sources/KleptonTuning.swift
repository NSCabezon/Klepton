// Live controller-alignment tuning — the values in DEBUG_ENV_VARS.md's
// KL_SENSE_* and KL_XR_GRIP_* entries, made adjustable while wearing the
// headset instead of only at launch.
//
// **This exists because these numbers cannot be derived, only judged.** They
// are the gap between Apple's grip convention and the pose each guest was built
// against, and every guest applies its own controller-model transform on top —
// so the residual differs per title and the only instrument is a person looking
// at their hand. At a relaunch per candidate that is two minutes an iteration,
// which is why three targets were tuned by eye and none of them converged.
//
// The environment still sets the starting values, so a run that never opens the
// panel behaves exactly as it did before this file existed.
import SwiftUI
import simd

/// One complete set. Value semantics on purpose: the render thread reads a
/// whole snapshot, so it can never see one slider's new value beside another's
/// old one.
struct KLTune: Equatable {
    var sensePitch: Float = -37
    /// Yaw (about the grip's Y) and roll (about its Z), degrees, on top of the
    /// pitch. Mirrored for the left hand together with the offsets' X term
    /// (KL_SENSE_MIRROR): reflecting X negates rotations about Y and Z.
    var senseYaw:  Float = 0
    var senseRoll: Float = 0
    var sensePos   = SIMD3<Float>(-0.004, 0.02, -0.02)
    var sensePivot = SIMD3<Float>(0, 0, 0)
    var gripPitch: Float = 37
    var aimPitch:  Float = 0
    var gripPivot  = SIMD3<Float>(0, 0, 0)
    var gripPos    = SIMD3<Float>(0, 0, 0)
}

final class KleptonTuning: ObservableObject {
    static let shared = KleptonTuning()

    /// What the UI edits. Main thread only — SwiftUI requires it, and the
    /// render thread never touches it.
    @Published var ui = KLTune() {
        didSet {
            publish()
            // Tuning inside the game persists for that game, and is what the
            // hub's settings sheet shows next time.
            guard !loading, !gameTarget.isEmpty else { return }
            let c = Self.calibration(ui, baseline: baseline)
            KLGameSettingsStore.update(gameTarget) { $0.calibration = c }
        }
    }

    /// ...and what the render thread reads, behind a lock. Two copies rather
    /// than one atomic because a tune is seven values and a controller drawn
    /// from half of one set and half of another is exactly the incoherence this
    /// whole area is about.
    private let lock = NSLock()
    private var live = KLTune()

    /// What the environment and the built-in defaults say, before any game's
    /// saved calibration — what Reset returns to, and what a saved field is
    /// compared against (only fields that DIFFER from it are saved, so a later
    /// change of default still reaches a game nobody tuned).
    private(set) var baseline = KLTune()
    /// The game the calibration is saved for (KLGameSettings.calibration).
    private(set) var gameTarget = ""
    private var loading = false

    private init() {
        // Seeded from whatever is actually in force, not from these defaults:
        // a panel showing its own idea of the numbers while the runtime uses
        // the environment's is an instrument that lies on the first frame.
        var t = KLTune()
        t.sensePitch = KLSenseTune.hiltPitch
        t.senseYaw   = klEnvFloat("KL_SENSE_YAW", 0)
        t.senseRoll  = klEnvFloat("KL_SENSE_ROLL", 0)
        t.sensePos   = KLSenseTune.shared[1].pos
        t.sensePivot = KLSenseTune.shared[1].pivot
        var gp: Float = 0, ap: Float = 0
        var pivot = [Float](repeating: 0, count: 3)
        var pos   = [Float](repeating: 0, count: 3)
        kl_openxr_grip_tune(&gp, &ap, &pivot, &pos)
        t.gripPitch = gp
        t.aimPitch  = ap
        t.gripPivot = SIMD3<Float>(pivot[0], pivot[1], pivot[2])
        t.gripPos   = SIMD3<Float>(pos[0], pos[1], pos[2])
        baseline = t
        ui = t
        live = t
        loadGame(klTargetName())
    }

    /// Apply `target`'s saved calibration over the baseline. Environment wins:
    /// a field whose KL_* knob is set in the launch environment keeps the
    /// environment's value (debugging), whatever was saved. Called at init and
    /// by the hub when Play picks a game.
    func loadGame(_ target: String) {
        loading = true
        defer { loading = false }
        gameTarget = target
        var t = baseline
        if let c = KLGameSettingsStore.load(target).calibration {
            let env = ProcessInfo.processInfo.environment
            func free(_ k: String) -> Bool { env[k] == nil && env[k + "_L"] == nil && env[k + "_R"] == nil }
            func v3(_ a: [Float]?) -> SIMD3<Float>? { a.flatMap { $0.count == 3 ? SIMD3<Float>($0[0], $0[1], $0[2]) : nil } }
            if free("KL_SENSE_POS"), let v = v3(c.senseOffset) { t.sensePos = v }
            if free("KL_SENSE_PIVOT"), let v = v3(c.sensePivot) { t.sensePivot = v }
            if free("KL_SENSE_PITCH"), let v = c.sensePitch { t.sensePitch = v }
            if free("KL_SENSE_YAW"), let v = c.senseYaw { t.senseYaw = v }
            if free("KL_SENSE_ROLL"), let v = c.senseRoll { t.senseRoll = v }
            if free("KL_XR_GRIP_PITCH"), let v = c.gripPitch { t.gripPitch = v }
            if free("KL_XR_AIM_PITCH"), let v = c.aimPitch { t.aimPitch = v }
            if free("KL_XR_GRIP_PIVOT"), let v = v3(c.gripPivot) { t.gripPivot = v }
            if free("KL_XR_GRIP_POS"), let v = v3(c.gripPos) { t.gripPos = v }
        }
        ui = t
        NSLog("[tune] \(target): \(asEnvironment())")
    }

    /// Back to the baseline, and forget this game's saved calibration.
    func reset() {
        ui = baseline
        if !gameTarget.isEmpty { KLGameSettingsStore.update(gameTarget) { $0.calibration = nil } }
    }

    /// The calibration as saved: only what differs from the baseline.
    static func calibration(_ t: KLTune, baseline b: KLTune) -> KLCalibration? {
        func d(_ x: Float, _ y: Float) -> Float? { abs(x - y) > 1e-4 ? x : nil }
        func d3(_ x: SIMD3<Float>, _ y: SIMD3<Float>) -> [Float]? {
            simd_length(x - y) > 1e-4 ? [x.x, x.y, x.z] : nil
        }
        let c = KLCalibration(senseOffset: d3(t.sensePos, b.sensePos),
                              sensePitch: d(t.sensePitch, b.sensePitch),
                              senseYaw: d(t.senseYaw, b.senseYaw),
                              senseRoll: d(t.senseRoll, b.senseRoll),
                              sensePivot: d3(t.sensePivot, b.sensePivot),
                              gripPitch: d(t.gripPitch, b.gripPitch),
                              aimPitch: d(t.aimPitch, b.aimPitch),
                              gripPivot: d3(t.gripPivot, b.gripPivot),
                              gripPos: d3(t.gripPos, b.gripPos))
        return c == KLCalibration() ? nil : c
    }

    /// The snapshot the pose path uses. Called once per controller pose.
    func snapshot() -> KLTune {
        lock.lock(); defer { lock.unlock() }
        return live
    }

    private func publish() {
        lock.lock(); live = ui; lock.unlock()
        var pivot = [ui.gripPivot.x, ui.gripPivot.y, ui.gripPivot.z]
        var pos   = [ui.gripPos.x,   ui.gripPos.y,   ui.gripPos.z]
        kl_openxr_set_grip_tune(ui.gripPitch, ui.aimPitch, &pivot, &pos)
    }

    /// The current set as the environment lines that would reproduce it —
    /// which is the whole point of tuning live. A value found by dragging and
    /// not written down is a value found twice.
    func asEnvironment() -> String {
        func v(_ s: SIMD3<Float>) -> String {
            String(format: "%.3f,%.3f,%.3f", s.x, s.y, s.z)
        }
        return """
        KL_SENSE_PITCH=\(String(format: "%.3f", ui.sensePitch)) \
        KL_SENSE_YAW=\(String(format: "%.3f", ui.senseYaw)) \
        KL_SENSE_ROLL=\(String(format: "%.3f", ui.senseRoll)) \
        KL_SENSE_POS="\(v(ui.sensePos))" \
        KL_SENSE_PIVOT="\(v(ui.sensePivot))" \
        KL_XR_GRIP_PITCH=\(String(format: "%.3f", ui.gripPitch)) \
        KL_XR_AIM_PITCH=\(String(format: "%.3f", ui.aimPitch)) \
        KL_XR_GRIP_PIVOT="\(v(ui.gripPivot))" \
        KL_XR_GRIP_POS="\(v(ui.gripPos))"
        """
    }
}

/// The panel. Lives in the boot window, which stays open beside the immersive
/// space — so the guest keeps rendering while these move.
struct TuningView: View {
    @ObservedObject private var tune = KleptonTuning.shared

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text("Controller alignment").font(.headline)
            Text("The X term (and yaw/roll) is mirrored for the left hand. Grip-frame "
                 + "axes: −Z points, +Y up, +X right. Saved for this game.")
                .font(.caption).foregroundStyle(.secondary)

            Group {
                deg("Sense pitch", $tune.ui.sensePitch, -90 ... 90)
                deg("Sense yaw",   $tune.ui.senseYaw,   -180 ... 180)
                deg("Sense roll",  $tune.ui.senseRoll,  -180 ... 180)
                vec("Sense pivot", $tune.ui.sensePivot)
                vec("Sense offset", $tune.ui.sensePos)
            }
            Divider()
            Group {
                deg("OpenXR grip pitch", $tune.ui.gripPitch, -90 ... 90)
                deg("OpenXR aim pitch",  $tune.ui.aimPitch,  -90 ... 90)
                vec("OpenXR grip pivot", $tune.ui.gripPivot)
                vec("OpenXR grip offset", $tune.ui.gripPos)
            }
            Divider()
            HStack {
                Button("Log as environment") { NSLog("[tune] \(tune.asEnvironment())") }
                Button("Reset") { tune.reset() }
            }
            // Shown as well as logged: reading a value off the panel beats
            // going to find the log for it, and this is what gets pasted.
            Text(tune.asEnvironment())
                .font(.system(.caption2, design: .monospaced))
                .textSelection(.enabled)
                .foregroundStyle(.secondary)
        }
        .padding()
    }

    private func deg(_ label: String, _ value: Binding<Float>,
                     _ range: ClosedRange<Float>) -> some View {
        HStack {
            Text(label).frame(width: 170, alignment: .leading)
            Slider(value: value, in: range, step: 0.5)
            Text(String(format: "%6.1f°", value.wrappedValue))
                .font(.system(.body, design: .monospaced)).frame(width: 70)
        }
    }

    private func vec(_ label: String, _ value: Binding<SIMD3<Float>>) -> some View {
        HStack {
            Text(label).frame(width: 170, alignment: .leading)
            // ±30 cm: the largest offset measured so far was 8 cm (BONELAB),
            // but Alyx's hands sat far further off than that.
            axis("x", value.x); axis("y", value.y); axis("z", value.z)
        }
    }

    private func axis(_ name: String, _ value: Binding<Float>) -> some View {
        VStack(spacing: 2) {
            Slider(value: value, in: -0.30 ... 0.30, step: 0.005)
            Text("\(name) \(String(format: "%+.3f", value.wrappedValue))")
                .font(.system(.caption2, design: .monospaced))
        }
    }
}
