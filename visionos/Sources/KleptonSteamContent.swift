// Half-Life: Alyx content from Steam, on the headset (alyx target only).
//
// The game's ~72 GB of content (depot 546561) used to be copied Mac -> headset
// with devicectl (stage_alyx.sh). This gate lets the app fetch it itself, from
// the player's own Steam account (ownership is proved by Steam: no depot key
// without a licence), through the SteamDepot package (../../../SteamDepot).
//
// Shape: before the boot window does anything, the alyx build checks — offline,
// stat only — whether the content is COMPLETE: a completion marker written by
// SteamDepot after a successful download/verify, and every file of the last
// manifest present at its exact size. If so, the window is the boot window as
// always. If not, this view takes the window: Steam QR login, then verify /
// adopt whatever is already there (e.g. a partial devicectl copy, which has no
// SteamDepot resume state — every chunk is checked by SHA-1 and kept if it
// matches), then download only what is missing. Play enters the existing boot
// path. Compiled only where the SteamDepot module is linked (the alyx target).
#if canImport(SteamDepot)
import SwiftUI
import UIKit
import CoreImage
import CoreImage.CIFilterBuiltins
import SteamDepot

enum AlyxContent {
    static let app: UInt32 = 546560
    static let depot: UInt32 = 546561

    /// <Documents>/android-files/alyx — the depot's own paths start with "game/",
    /// so the content lands in android-files/alyx/game exactly where kl_app.c
    /// (stage_files "alyx") makes the game root.
    static var root: URL {
        FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("android-files/alyx", isDirectory: true)
    }

    /// The same exclusions as visionos/stage_alyx.sh (default, without --include-extras):
    /// code (bundle frameworks), other-platform dirs, binaries/debug files, host junk,
    /// editor tools, workshop sample maps. `path` is depot-relative ("game/hlvr/…").
    static func include(_ path: String) -> Bool {
        let p = path.lowercased()
        guard p.hasPrefix("game/") else { return false }
        let rel = String(p.dropFirst(5))
        let parts = rel.split(separator: "/", omittingEmptySubsequences: false).map(String.init)
        guard let last = parts.last else { return false }
        if parts.first == "bin" || (parts.count > 2 && parts[1] == "bin") { return false }
        let platformDirs: Set<String> = ["win64", "win32", "linuxsteamrt64", "linux64", "osx64"]
        if parts.dropLast().contains(where: platformDirs.contains) { return false }
        for ext in [".dll", ".exe", ".pdb", ".dbg", ".sym", ".so"] where last.hasSuffix(ext) { return false }
        if last.contains(".so.") || last.hasPrefix("vfxcompile") { return false }
        if last == ".ds_store" || last.hasPrefix("._") || parts.contains(".depotdownloader") { return false }
        if rel.hasPrefix("core/tools/") || rel.hasPrefix("hlvr/maps/workshop_examples/") { return false }
        return true
    }

    static func status() -> LocalContentStatus { SteamDepotDownloader.localStatus(directory: root, depot: depot) }

    /// Strict: marker + every listed file at the right size. Anything else goes through the gate.
    static var isReady: Bool { status().isComplete && !UserDefaults.standard.bool(forKey: verifyNextLaunchKey) }

    /// Content that looks like a (possibly partial) copy — offered "Play anyway".
    static var looksPresent: Bool {
        let g = root.appendingPathComponent("game")
        return FileManager.default.fileExists(atPath: g.appendingPathComponent("hlvr/gameinfo.gi").path)
            && FileManager.default.fileExists(atPath: g.appendingPathComponent("core/pak01_dir.vpk").path)
    }

    static let verifyNextLaunchKey = "kl.steam.verifyNextLaunch"

    /// Only one depot-wide test hook: KL_STEAM_FILTER=prefix[,prefix…] limits the download (sim tests).
    static var debugFilters: [String] {
        (ProcessInfo.processInfo.environment["KL_STEAM_FILTER"] ?? "")
            .split(separator: ",").map { String($0).trimmingCharacters(in: .whitespaces) }.filter { !$0.isEmpty }
    }
}

@MainActor
final class SteamContentModel: ObservableObject {
    enum Stage: Equatable {
        case idle
        case connecting(String)
        case qr(String)
        case working
        case paused
        case complete
        case failed(String)
    }

    @Published var stage: Stage = .idle
    @Published var progress = DownloadProgress()
    @Published var account: String?
    @Published var local = AlyxContent.status()
    @Published var freeBytes: UInt64?
    @Published var speed: Double = 0          // uncompressed bytes/s, smoothed
    @Published var networkSpeed: Double = 0
    @Published var verifySpeed: Double = 0
    private var samples: [(t: Date, b: UInt64, n: UInt64)] = []
    private var task: Task<Void, Never>?
    private var session: SteamSession?
    private let store = KeychainCredentialStore(service: "SteamDepot.refreshToken", account: "steam")
    var interruptedByBackground = false

    init() {
        account = (try? store.load())?.accountName
        refreshLocal()
    }

    var isBusy: Bool {
        switch stage { case .connecting, .qr, .working: return true; default: return false }
    }

    func refreshLocal() {
        local = AlyxContent.status()
        let root = AlyxContent.root
        try? FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        if let v = try? root.resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey])
            .volumeAvailableCapacityForImportantUsage { freeBytes = UInt64(v) }
    }

    func start(forceVerify: Bool = false) {
        guard task == nil else { return }
        UserDefaults.standard.set(false, forKey: AlyxContent.verifyNextLaunchKey)
        UIApplication.shared.isIdleTimerDisabled = true
        samples = []
        task = Task { [weak self] in
            await self?.run(forceVerify: forceVerify)
            await MainActor.run {
                self?.task = nil
                UIApplication.shared.isIdleTimerDisabled = false
                self?.refreshLocal()
            }
        }
    }

    func pause() {
        task?.cancel()
        stage = .paused
    }

    func signOut() {
        task?.cancel()
        try? store.clear()
        account = nil
        stage = .idle
    }

    private func log(_ s: String) { NSLog("[steam] \(s)") }

    private func run(forceVerify: Bool) async {
        let session = SteamSession(log: { NSLog("[steam] %@", $0) })
        self.session = session
        defer { Task { await session.disconnect() } }
        do {
            stage = .connecting("Connecting to Steam…")
            try await session.connect()
            // Token hand-off from the Mac: the QR is drawn inside the headset, where a phone camera
            // cannot see it. `steamdepot` on the Mac writes ~/.steamdepot/token.json; copying that
            // file to <Documents>/steamdepot-token.json (devicectl) imports it into the Keychain.
            let handoff = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
                .appendingPathComponent("steamdepot-token.json")
            if let imported = try? FileCredentialStore(url: handoff).load() {
                try? store.save(imported)
                try? FileManager.default.removeItem(at: handoff)
                log("imported Steam token for '\(imported.accountName)' from Documents/steamdepot-token.json")
            }
            var creds = try? store.load()
            if creds != nil {
                stage = .connecting("Signing in as \(creds!.accountName)…")
                do {
                    try await session.logOn(creds!)
                } catch SteamDepotError.logonFailed(let r) {
                    log("saved token rejected (EResult \(r)); falling back to QR")
                    try? store.clear()
                    account = nil
                    creds = nil
                }
                if creds == nil {
                    await session.disconnect()
                    try await session.connect()
                }
            }
            if creds == nil {
                let device = "Klepton (\(UIDevice.current.name))"
                creds = try await session.authenticateWithQR(deviceName: device) { url in
                    // Second fallback: the Mac can render this from the device log.
                    NSLog("[steam] QR url: %@", url)
                    await MainActor.run { self.stage = .qr(url) }
                }
                try store.save(creds!)
                stage = .connecting("Signing in as \(creds!.accountName)…")
                do {
                    try await session.logOn(creds!)
                } catch SteamDepotError.logonFailed(let r) {
                    try? store.clear()
                    account = nil
                    throw SteamDepotError.authFailed("Steam rejected the sign-in (EResult \(r)). Press Retry to sign in again.")
                }
            }
            account = creds!.accountName
            stage = .working
            let downloader = SteamDepotDownloader(session: session, log: { NSLog("[steam] %@", $0) })
            let opts = DepotDownloadOptions(appID: AlyxContent.app, depotID: AlyxContent.depot, branch: "public",
                                            directory: AlyxContent.root, filters: AlyxContent.debugFilters,
                                            include: { AlyxContent.include($0) },
                                            maxConcurrentDownloads: 16, verifyConcurrency: 6,
                                            forceVerify: forceVerify)
            let summary = try await downloader.download(opts) { p in
                Task { @MainActor in self.update(p) }
            }
            log("done: \(summary.files) files, \(summary.skippedFiles) already there, "
                + "\(summary.bytesDownloaded) bytes fetched, verified \(summary.verifiedBytes) in \(summary.verifyElapsed)s")
            stage = .complete
        } catch is CancellationError {
            stage = .paused
        } catch SteamDepotError.cancelled {
            stage = .paused
        } catch {
            if Task.isCancelled { stage = .paused; return }
            log("failed: \(error)")
            stage = .failed("\(error)")
        }
    }

    private func update(_ p: DownloadProgress) {
        guard task != nil else { return }
        progress = p
        if p.phase == .verifying, p.verifyElapsed > 0 {
            verifySpeed = Double(p.verifyBytesDone) / p.verifyElapsed
        }
        if p.phase == .downloading {
            let now = Date()
            samples.append((now, p.bytesDownloaded, p.networkBytes))
            samples.removeAll { now.timeIntervalSince($0.t) > 15 }
            if let first = samples.first, let last = samples.last, last.t.timeIntervalSince(first.t) > 1 {
                let dt = last.t.timeIntervalSince(first.t)
                speed = Double(last.b - first.b) / dt
                networkSpeed = Double(last.n - first.n) / dt
            }
        }
    }
}

private func gb(_ b: UInt64) -> String { String(format: "%.1f GB", Double(b) / 1e9) }
private func gbD(_ b: Double) -> String { String(format: "%.1f GB", b / 1e9) }
private func mbps(_ b: Double) -> String { String(format: "%.1f MB/s", b / 1e6) }
private func eta(_ s: Double) -> String {
    guard s.isFinite, s > 0 else { return "—" }
    let m = Int(s / 60)
    return m >= 60 ? "\(m / 60) h \(m % 60) min" : (m > 0 ? "\(m) min" : "<1 min")
}

func qrImage(_ string: String) -> UIImage? {
    let f = CIFilter.qrCodeGenerator()
    f.message = Data(string.utf8)
    f.correctionLevel = "M"
    guard let out = f.outputImage?.transformed(by: CGAffineTransform(scaleX: 10, y: 10)) else { return nil }
    let margin: CGFloat = 40
    let canvas = CGRect(x: 0, y: 0, width: out.extent.width + 2 * margin, height: out.extent.height + 2 * margin)
    let padded = out.transformed(by: CGAffineTransform(translationX: margin, y: margin))
        .composited(over: CIImage(color: .white).cropped(to: canvas))
    guard let cg = CIContext().createCGImage(padded, from: canvas) else { return nil }
    return UIImage(cgImage: cg)
}

/// The window shown instead of the boot window while the content is not complete.
struct SteamContentView: View {
    let onPlay: () -> Void
    @StateObject private var model = SteamContentModel()
    @Environment(\.scenePhase) private var scenePhase

    var body: some View {
        VStack(alignment: .leading, spacing: 18) {
            HStack(alignment: .firstTextBaseline) {
                Text("Half-Life: Alyx").font(.largeTitle.bold())
                Spacer()
                if let a = model.account {
                    Text("Steam: \(a)").font(.footnote).foregroundStyle(.secondary)
                    Button("Sign out") { model.signOut() }.font(.footnote).disabled(model.isBusy)
                }
            }
            content
            Spacer(minLength: 0)
            if model.isBusy {
                Label("Keep this window open and the headset on: visionOS pauses apps in the background, "
                      + "and the download stops with it. It resumes where it left off on the next launch.",
                      systemImage: "exclamationmark.triangle")
                    .font(.footnote).foregroundStyle(.secondary)
            }
        }
        .padding(32)
        .onAppear {
            Lifecycle.keepAlive = true
            // "Check for updates / Verify files on next launch" from the boot window.
            if UserDefaults.standard.bool(forKey: AlyxContent.verifyNextLaunchKey) { model.start(forceVerify: true) }
            // Scripted runs (simulator tests): KL_STEAM_AUTOSTART=1 presses the main button.
            else if klEnvOn("KL_STEAM_AUTOSTART", default: false) { model.start() }
        }
        .onDisappear { Lifecycle.keepAlive = false }
        .onChange(of: scenePhase) { _, phase in
            if phase == .background, model.isBusy { model.interruptedByBackground = true }
            if phase == .active, model.interruptedByBackground {
                model.interruptedByBackground = false
                if case .failed = model.stage { model.start() }
            }
        }
    }

    @ViewBuilder private var content: some View {
        switch model.stage {
        case .idle: idle
        case .connecting(let s):
            HStack(spacing: 12) { ProgressView(); Text(s) }
        case .qr(let url):
            HStack(alignment: .top, spacing: 28) {
                if let img = qrImage(url) {
                    Image(uiImage: img).interpolation(.none).resizable().frame(width: 300, height: 300)
                        .clipShape(RoundedRectangle(cornerRadius: 12))
                }
                VStack(alignment: .leading, spacing: 10) {
                    Text("Sign in to Steam").font(.title2.bold())
                    Text("Open the Steam app on your phone, tap the Steam Guard shield, then “Scan a QR code”, and approve the sign-in.")
                    Text("Or, from a Mac: copy a steamdepot token.json into the app's Documents as steamdepot-token.json and press Retry.")
                        .font(.footnote).foregroundStyle(.secondary)
                    Text("Only a sign-in token is kept, in this device's Keychain. Your password never touches this app.")
                        .font(.footnote).foregroundStyle(.secondary)
                    Button("Cancel") { model.pause(); model.stage = .idle }
                }
            }
        case .working: working
        case .paused:
            VStack(alignment: .leading, spacing: 12) {
                Text("Paused.").font(.title3)
                progressSummary
                HStack {
                    Button("Resume") { model.start() }.buttonStyle(.borderedProminent)
                    if AlyxContent.looksPresent { Button("Play anyway") { onPlay() } }
                }
            }
        case .complete:
            VStack(alignment: .leading, spacing: 12) {
                Label("Half-Life: Alyx is installed and verified.", systemImage: "checkmark.circle.fill")
                    .foregroundStyle(.green).font(.title3)
                Text("\(model.local.totalFiles) files, \(gb(model.local.totalBytes)).").foregroundStyle(.secondary)
                HStack(spacing: 16) {
                    Button("Play") { onPlay() }.buttonStyle(.borderedProminent).controlSize(.large)
                    Button("Check for updates / Verify files") { model.start(forceVerify: true) }
                }
            }
        case .failed(let msg):
            VStack(alignment: .leading, spacing: 12) {
                Label(msg, systemImage: "xmark.octagon.fill").foregroundStyle(.red)
                HStack {
                    Button("Retry") { model.start() }.buttonStyle(.borderedProminent)
                    if AlyxContent.looksPresent { Button("Play anyway") { onPlay() } }
                }
            }
        }
    }

    @ViewBuilder private var idle: some View {
        let present = AlyxContent.looksPresent
        VStack(alignment: .leading, spacing: 12) {
            if present {
                Text("Game files were found on this device, but they have not been verified as complete.")
                Text("SteamDepot can check them against Steam (keeping every file that matches) and download only what is missing.")
                    .foregroundStyle(.secondary)
            } else {
                Text("The game content (about 70 GB) is not on this device yet.")
                Text("Download it from Steam with the account that owns Half-Life: Alyx.").foregroundStyle(.secondary)
            }
            if let free = model.freeBytes {
                Text("Free space: \(gb(free))").font(.footnote).foregroundStyle(free < 75_000_000_000 && !present ? .orange : .secondary)
            }
            HStack(spacing: 16) {
                Button(present ? "Complete download from Steam" : "Download Half-Life: Alyx from Steam") { model.start() }
                    .buttonStyle(.borderedProminent).controlSize(.large)
                if present { Button("Play anyway") { onPlay() } }
            }
            if !AlyxContent.debugFilters.isEmpty {
                Text("KL_STEAM_FILTER: \(AlyxContent.debugFilters.joined(separator: ", "))").font(.caption).foregroundStyle(.orange)
            }
        }
    }

    @ViewBuilder private var working: some View {
        let p = model.progress
        VStack(alignment: .leading, spacing: 12) {
            switch p.phase {
            case .preparing:
                HStack(spacing: 12) { ProgressView(); Text(p.message.isEmpty ? "Preparing…" : p.message) }
            case .verifying:
                Text("Verifying existing files…").font(.title3)
                ProgressView(value: Double(p.verifyBytesDone), total: Double(max(1, p.verifyBytesTotal)))
                Text("\(gb(p.verifyBytesDone)) of \(gb(p.verifyBytesTotal)) checked · \(p.verifyFilesDone)/\(p.verifyFilesTotal) files · \(mbps(model.verifySpeed))")
                    .monospacedDigit().foregroundStyle(.secondary)
            case .downloading, .finished:
                Text(p.chunksTotal == 0 ? "Finishing…" : "Downloading from Steam…").font(.title3)
                let have = Double(p.depotBytes - p.bytesToDownload + p.bytesDownloaded)
                ProgressView(value: have, total: Double(max(1, p.depotBytes)))
                Text("\(gbD(have)) of \(gb(p.depotBytes)) · \(mbps(model.speed)) (network \(mbps(model.networkSpeed))) · ETA \(eta(Double(p.bytesToDownload - p.bytesDownloaded) / max(1, model.speed)))")
                    .monospacedDigit()
                Text("Files \(p.completedFiles)/\(p.totalFiles) · this session \(gb(p.bytesDownloaded)) of \(gb(p.bytesToDownload)) fetched")
                    .monospacedDigit().font(.footnote).foregroundStyle(.secondary)
            }
            Button("Pause") { model.pause() }
        }
    }

    @ViewBuilder private var progressSummary: some View {
        let p = model.progress
        if p.depotBytes > 0 {
            Text("\(gbD(Double(p.depotBytes - p.bytesToDownload + p.bytesDownloaded))) of \(gb(p.depotBytes))")
                .monospacedDigit().foregroundStyle(.secondary)
        }
    }
}

/// Small panel in the boot window (alyx only): schedule a full verify / update check.
struct SteamContentMaintenance: View {
    @State private var scheduled = UserDefaults.standard.bool(forKey: AlyxContent.verifyNextLaunchKey)
    var body: some View {
        HStack {
            Button(scheduled ? "Verify scheduled for next launch" : "Check for updates / Verify files on next launch") {
                scheduled.toggle()
                UserDefaults.standard.set(scheduled, forKey: AlyxContent.verifyNextLaunchKey)
            }
            Spacer()
        }
        .font(.callout)
    }
}
#endif
