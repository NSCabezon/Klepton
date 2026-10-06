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
        /// Username/password form (primary sign-in). Optional message (e.g. wrong password).
        case signIn(String?)
        /// Steam Guard: a code is needed (and/or the phone can approve).
        case guardCode(type: String, canApprove: Bool, error: String?)
        /// Waiting for approval in the Steam app / by e-mail.
        case approve(String)
        case qr(String)
        case working
        case paused
        case complete
        case failed(String)
    }

    enum Method { case saved, password(String, String), qr }

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
    private var pendingAuth: CredentialsAuthSession?
    private var pendingCodeType: SteamGuardType?
    private var forceVerify = false
    private let store = KeychainCredentialStore(service: "SteamDepot.refreshToken", account: "steam")
    var interruptedByBackground = false

    init() {
        account = (try? store.load())?.accountName
        refreshLocal()
    }

    var isBusy: Bool {
        switch stage { case .connecting, .qr, .working, .guardCode, .approve: return true; default: return false }
    }

    func refreshLocal() {
        local = AlyxContent.status()
        let root = AlyxContent.root
        try? FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        if let v = try? root.resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey])
            .volumeAvailableCapacityForImportantUsage { freeBytes = UInt64(v) }
    }

    /// Main button: use the saved sign-in if there is one, otherwise show the sign-in form.
    func start(forceVerify: Bool = false) {
        self.forceVerify = forceVerify
        UserDefaults.standard.set(false, forKey: AlyxContent.verifyNextLaunchKey)
        launch(.saved)
    }

    /// Primary sign-in: account name + password (used once, RSA-encrypted for Steam, never stored).
    func signIn(user: String, password: String) { launch(.password(user, password)) }

    func signInWithQR() { launch(.qr) }

    func submitCode(_ code: String) {
        guard let session, let auth = pendingAuth, let type = pendingCodeType else { return }
        stage = .connecting("Checking code…")
        Task {
            do {
                try await session.submitGuardCode(auth, code: code, type: type)
                // The poller in run() finishes the sign-in.
                if case .connecting = stage { stage = .connecting("Code accepted, signing in…") }
            } catch {
                stage = .guardCode(type: type.description, canApprove: auth.canApproveOnDevice,
                                   error: "That code was not accepted — try again.")
            }
        }
    }

    private func launch(_ method: Method) {
        guard task == nil else { return }
        UIApplication.shared.isIdleTimerDisabled = true
        samples = []
        task = Task { [weak self] in
            await self?.run(method)
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

    func cancelSignIn() {
        task?.cancel()
        stage = .signIn(nil)
    }

    func signOut() {
        task?.cancel()
        try? store.clear()
        account = nil
        stage = .idle
    }

    private func log(_ s: String) { NSLog("[steam] \(s)") }

    /// Token hand-off from the Mac: `steamdepot` on the Mac writes ~/.steamdepot/token.json;
    /// copying it to <Documents>/steamdepot-token.json (devicectl) imports it into the Keychain.
    private func importHandoffToken() {
        let handoff = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("steamdepot-token.json")
        if let imported = try? FileCredentialStore(url: handoff).load() {
            try? store.save(imported)
            try? FileManager.default.removeItem(at: handoff)
            account = imported.accountName
            log("imported Steam token for '\(imported.accountName)' from Documents/steamdepot-token.json")
        }
    }

    private func run(_ method: Method) async {
        let session = SteamSession(log: { NSLog("[steam] %@", $0) })
        self.session = session
        defer { Task { await session.disconnect() } }
        do {
            importHandoffToken()
            var creds: SteamCredentials
            switch method {
            case .saved:
                guard let saved = try? store.load() else { stage = .signIn(nil); return }
                creds = saved
            case .password(let user, let password):
                stage = .connecting("Contacting Steam…")
                try await session.connect()
                let device = "Klepton (\(UIDevice.current.name))"
                let auth: CredentialsAuthSession
                do {
                    auth = try await session.beginCredentialsAuth(accountName: user, password: password, deviceName: device,
                                                                  guardData: (try? store.load())?.guardData)
                } catch SteamDepotError.authFailed(let why) {
                    stage = .signIn(why.prefix(1).uppercased() + why.dropFirst() + ".")
                    return
                }
                pendingAuth = auth
                pendingCodeType = auth.needsCode
                if let code = auth.needsCode {
                    stage = .guardCode(type: code.description, canApprove: auth.canApproveOnDevice, error: nil)
                } else if auth.canApproveOnDevice {
                    stage = .approve("Approve this sign-in in the Steam app on your phone.")
                } else if auth.canApproveByEmail {
                    stage = .approve("Approve this sign-in from the e-mail Steam just sent you.")
                } else {
                    stage = .connecting("Signing in…")
                }
                creds = try await session.waitForCredentialsAuth(auth)
                pendingAuth = nil
                try store.save(creds)
                log("signed in with account name/password as '\(creds.accountName)'")
            case .qr:
                stage = .connecting("Contacting Steam…")
                try await session.connect()
                let device = "Klepton (\(UIDevice.current.name))"
                creds = try await session.authenticateWithQR(deviceName: device) { url in
                    // The Mac can render this from the device log.
                    NSLog("[steam] QR url: %@", url)
                    await MainActor.run { self.stage = .qr(url) }
                }
                try store.save(creds)
            }

            stage = .connecting("Signing in as \(creds.accountName)…")
            if case .saved = method { try await session.connect() }
            do {
                try await session.logOn(creds)
            } catch SteamDepotError.logonFailed(let r) {
                log("saved sign-in rejected (EResult \(r))")
                try? store.clear()
                account = nil
                stage = .signIn("Your saved Steam sign-in has expired. Please sign in again.")
                return
            }
            account = creds.accountName
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
            if case .signIn = stage { return }
            stage = .paused
        } catch SteamDepotError.cancelled {
            stage = .paused
        } catch {
            if Task.isCancelled { if case .signIn = stage { return }; stage = .paused; return }
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
        case .signIn(let message):
            SteamSignInForm(model: model, message: message)
        case .guardCode(let type, let canApprove, let error):
            SteamGuardCodeForm(model: model, type: type, canApprove: canApprove, error: error)
        case .approve(let text):
            VStack(alignment: .leading, spacing: 14) {
                Text("Confirm the sign-in").font(.title2.bold())
                HStack(spacing: 12) { ProgressView(); Text(text) }
                Button("Cancel") { model.cancelSignIn() }
            }
        case .qr(let url):
            HStack(alignment: .top, spacing: 28) {
                if let img = qrImage(url) {
                    Image(uiImage: img).interpolation(.none).resizable().frame(width: 300, height: 300)
                        .clipShape(RoundedRectangle(cornerRadius: 12))
                }
                VStack(alignment: .leading, spacing: 10) {
                    Text("Sign in with a QR code").font(.title2.bold())
                    Text("Scan it with the Steam app on a phone (Steam Guard shield → “Scan a QR code”). "
                         + "A headset's own display can't be seen by a phone camera, so this mostly helps when "
                         + "mirroring the view; the same link is written to the device log as “[steam] QR url:”.")
                    Button("Back") { model.cancelSignIn() }
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

#if canImport(SteamDepot)
/// Primary sign-in: Steam account name + password, with iCloud Keychain AutoFill.
/// The password lives only in this view's state until it is handed to SteamDepot, which
/// RSA-encrypts it for Steam; it is cleared right after and never written anywhere.
struct SteamSignInForm: View {
    @ObservedObject var model: SteamContentModel
    let message: String?
    @State private var user = UserDefaults.standard.string(forKey: "kl.steam.lastAccount") ?? ""
    @State private var password = ""
    @State private var showOther = false
    @FocusState private var focus: Field?
    enum Field { case user, password }

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            Text("Sign in to Steam").font(.title2.bold())
            Text("Use the Steam account that owns Half-Life: Alyx.").foregroundStyle(.secondary)
            if let message { Label(message, systemImage: "exclamationmark.circle").foregroundStyle(.orange) }
            TextField("Steam account name", text: $user)
                .textContentType(.username)
                .textInputAutocapitalization(.never)
                .autocorrectionDisabled()
                .focused($focus, equals: .user)
                .submitLabel(.next)
                .onSubmit { focus = .password }
                .frame(maxWidth: 420)
            SecureField("Password", text: $password)
                .textContentType(.password)
                .focused($focus, equals: .password)
                .submitLabel(.go)
                .onSubmit(submit)
                .frame(maxWidth: 420)
            HStack(spacing: 16) {
                Button("Sign in", action: submit)
                    .buttonStyle(.borderedProminent)
                    .disabled(user.trimmingCharacters(in: .whitespaces).isEmpty || password.isEmpty)
                Button(showOther ? "Hide other ways" : "Other ways to sign in…") { showOther.toggle() }
            }
            Text("Your password is sent to Steam encrypted and is never stored; only a sign-in token is kept, in this device's Keychain.")
                .font(.footnote).foregroundStyle(.secondary)
            if showOther {
                VStack(alignment: .leading, spacing: 8) {
                    Button("Show a QR code (scan with the Steam app)") { model.signInWithQR() }
                    Text("From a Mac: sign in with the steamdepot tool, then copy its token.json into this app's "
                         + "Documents as steamdepot-token.json (devicectl) and press Download again.")
                        .font(.footnote).foregroundStyle(.secondary)
                }
                .padding(.top, 4)
            }
        }
        .onAppear { focus = user.isEmpty ? .user : .password }
    }

    private func submit() {
        let u = user.trimmingCharacters(in: .whitespaces)
        guard !u.isEmpty, !password.isEmpty else { return }
        UserDefaults.standard.set(u, forKey: "kl.steam.lastAccount")
        let p = password
        password = ""
        model.signIn(user: u, password: p)
    }
}

struct SteamGuardCodeForm: View {
    @ObservedObject var model: SteamContentModel
    let type: String
    let canApprove: Bool
    let error: String?
    @State private var code = ""
    @FocusState private var focused: Bool

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            Text("Steam Guard").font(.title2.bold())
            if canApprove {
                HStack(spacing: 10) {
                    ProgressView()
                    Text("Approve this sign-in in the Steam app on your phone — or enter the \(type) below.")
                }
            } else {
                Text("Enter the \(type).")
            }
            if let error { Label(error, systemImage: "exclamationmark.circle").foregroundStyle(.orange) }
            TextField("Code", text: $code)
                .textContentType(.oneTimeCode)
                .textInputAutocapitalization(.characters)
                .autocorrectionDisabled()
                .focused($focused)
                .onSubmit(submit)
                .frame(maxWidth: 220)
                .font(.title3.monospaced())
            HStack(spacing: 16) {
                Button("Submit", action: submit).buttonStyle(.borderedProminent).disabled(code.count < 5)
                Button("Cancel") { model.cancelSignIn() }
            }
        }
        .onAppear { focused = !canApprove }
    }

    private func submit() {
        let c = code.trimmingCharacters(in: .whitespaces)
        guard c.count >= 5 else { return }
        code = ""
        model.submitCode(c)
    }
}
#endif
