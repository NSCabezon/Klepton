// The HUB: one app carrying several games, picked from an in-app library.
//
// Built only for the "hub" row of visionos/targets.py (compilation condition
// KL_HUB). The games it carries are generated into klHubGameRows
// (build/hub/KleptonHubManifest.swift) from that row's "games" list, and each
// game's translated frameworks sit in their own Frameworks/<game>/ (kl_app.c
// picks the selected one).
//
// How a game runs: Play sets KL_TARGET to the game's target name — the same
// override kl_app_configure and kl_app_target_name_or_default() already honour —
// and only then shows the boot window, which configures the runtime for it. Each
// game's data lives under its own container root, Documents/games/<target>/
// (Paths.containerURL), laid out exactly as a single-game build lays out
// Documents/.
//
// ONE GAME PER PROCESS. The runtime's tables are process-global and a guest
// cannot be unloaded, and visionOS gives an app no way to relaunch itself: when
// the game ends (or the app is backgrounded), Klepton exits, and the next launch
// shows this library again. The UI says so.
#if KL_HUB
import SwiftUI
import Foundation
import Combine
#if canImport(SteamDepot)
import SteamDepot
#endif
#if canImport(MetaStore)
import MetaStore
#endif

/// One game the hub carries (generated from targets.py; see gen_xcodeproj.py).
struct KLHubGameRow: Identifiable, Hashable {
    let target: String
    let display: String
    let kind: String
    let entry: String
    /// False in a UI-only build (KLEPTON_HUB_NO_GUESTS): no frameworks, cannot boot.
    let bundled: Bool
    /// targets.py's tree and apk: where the runtime looks, relative to the game's
    /// container root, for <tree>/assets, the metadata beside it, and the APK.
    let tree: String
    let apk: String
    /// apktool.yml / AndroidManifest.xml from the Mac's apktool tree, base64 (empty
    /// for a guest that has none). Written beside <tree>/assets by HubPrep.
    let meta: [String: String]
    var id: String { target }
}

enum HubLaunch {
    static let lastGameKey = "kl.hub.lastGame"

    /// KL_HUB_GAME=<target> in the launch environment skips the library and boots
    /// that game (scripted runs). Also runs the data migration, which must happen
    /// before anything creates a game's directories.
    static func scriptedGame() -> String? {
        HubStorage.migrateLegacyData()
        guard let t = ProcessInfo.processInfo.environment["KL_HUB_GAME"], !t.isEmpty else { return nil }
        guard let row = klHubGameRows.first(where: { $0.target == t && $0.bundled }) else {
            NSLog("[hub] KL_HUB_GAME=\(t) is not a game this build carries; showing the library")
            return nil
        }
        // Synchronous: there is no library to show progress in, and the runtime's
        // own report names whatever is still missing.
        if let err = HubPrep.prepare(row) { NSLog("[hub] \(t): could not prepare its data: \(err)") }
        select(t)
        return t
    }

    /// The selected game becomes the runtime's target. Must run before BootView is
    /// created (its state reads klTargetName()) and before kl_app_configure.
    static func select(_ target: String) {
        setenv("KL_TARGET", target, 1)
        UserDefaults.standard.set(target, forKey: lastGameKey)
        // The game's own presentation settings (full immersion, hand matting),
        // published now — before the boot window exists — so the immersive
        // scene is already .full with the real hands hidden when it opens.
        KleptonChroma.shared.loadGame(target)
        // ...and its controller calibration (the live tune the pose path reads).
        KleptonTuning.shared.loadGame(target)
        // Before boot: the guest's Steamworks stand-in asks the real account from
        // SteamAPI_Init on (KleptonSteamBridge.swift).
        #if canImport(SteamDepot)
        KleptonSteamBridge.register()
        #endif
        // A game is about to boot: from here on, backgrounding ends the process
        // as in a single-game build.
        Lifecycle.keepAlive = false
        NSLog("[hub] selected \(target) — KL_TARGET=\(klTargetName()), container \(Paths.containerURL(for: target).path)")
    }
}

/// Data that predates the hub, adopted in place.
///
/// Half-Life: Alyx's 72 GB already sit in this bundle id's container at
/// Documents/android-files/alyx (stage_alyx.sh, or the single-game build's
/// in-app download). The hub's layout for it is
/// Documents/games/alyx/android-files/alyx. Same volume, so moving it is one
/// rename(2) — no copy — and a relative symlink is left at the old path, so a
/// single-game alyx build installed over the hub still finds its data.
enum HubStorage {
    private static var done = false

    static func migrateLegacyData() {
        guard !done else { return }
        done = true
        #if canImport(SteamDepot)
        for row in klHubGameRows {
            guard let game = SteamGames.descriptor(for: row.target), let legacy = game.legacyDataSubpath else { continue }
            adopt(target: row.target, legacy: legacy, into: game.dataSubpath)
        }
        #endif
    }

    /// What "Delete game data" removes for a game: the Steam download root
    /// and, for an APK game, what HubPrep derived from it (the hard-linked APK
    /// and <tree>/). NOT android-files/ (an APK game's saves) nor
    /// Library/Application Support (a glibc game's saves and config).
    static func dataURLs(_ row: KLHubGameRow) -> [URL] {
        var urls: [URL] = []
        let c = Paths.containerURL(for: row.target)
        let derived = [c.appendingPathComponent(row.apk), c.appendingPathComponent(row.tree, isDirectory: true)]
        #if canImport(SteamDepot)
        if let game = SteamGames.descriptor(for: row.target) {
            urls.append(game.root)
            if game.apk != nil { urls += derived }
        }
        #endif
        #if canImport(MetaStore)
        // The Meta recipe's own files only (meta/, the OBB and add-on packs) — never the rest of android-files/.
        if let r = KLCatalog.metaRecipe(row.target) {
            urls += KLCatalog.metaStore(row.target).ownedURLs(r)
            urls += derived.filter { !urls.contains($0) }
        }
        #endif
        return urls
    }

    /// Bytes allocated under `url` (0 if absent). Hard links count once per
    /// path, so an APK game's derived APK shows twice its real cost — the
    /// number is what deleting frees at most, not exactly.
    static func size(of url: URL) -> UInt64 {
        let fm = FileManager.default
        var isDir: ObjCBool = false
        guard fm.fileExists(atPath: url.path, isDirectory: &isDir) else { return 0 }
        let keys: [URLResourceKey] = [.totalFileAllocatedSizeKey, .isRegularFileKey]
        if !isDir.boolValue {
            return UInt64((try? url.resourceValues(forKeys: Set(keys)))?.totalFileAllocatedSize ?? 0)
        }
        var total: UInt64 = 0
        if let e = fm.enumerator(at: url, includingPropertiesForKeys: keys) {
            for case let f as URL in e {
                if let v = try? f.resourceValues(forKeys: Set(keys)), v.isRegularFile == true {
                    total += UInt64(v.totalFileAllocatedSize ?? 0)
                }
            }
        }
        return total
    }

    /// Remove dataURLs(row). nil on success, else the first error.
    static func deleteData(_ row: KLHubGameRow) -> String? {
        var firstError: String?
        for u in dataURLs(row) where FileManager.default.fileExists(atPath: u.path) {
            do { try FileManager.default.removeItem(at: u) } catch {
                if firstError == nil { firstError = "\(error)" }
            }
        }
        NSLog("[hub] \(row.target): deleted its game data\(firstError.map { " (with an error: \($0))" } ?? "")")
        return firstError
    }

    /// A download or verify is running for the game.
    @MainActor static func isBusy(_ model: HubLibraryModel, _ target: String) -> Bool {
        #if canImport(MetaStore)
        if model.meta[target]?.isBusy == true { return true }
        #endif
        #if canImport(SteamDepot)
        return model.content[target]?.isBusy ?? false
        #else
        return false
        #endif
    }

    static func adopt(target: String, legacy: String, into dataSubpath: String) {
        let fm = FileManager.default
        let old = Paths.documents.appendingPathComponent(legacy, isDirectory: true)
        let new = Paths.containerURL(for: target).appendingPathComponent(dataSubpath, isDirectory: true)
        // Only a REAL directory is adopted: a symlink means this already ran.
        guard let type = (try? fm.attributesOfItem(atPath: old.path))?[.type] as? FileAttributeType,
              type == .typeDirectory else { return }
        // An empty directory at the destination (made by an earlier look at the
        // library) is not data; anything else is, and is never overwritten.
        if fm.fileExists(atPath: new.path) {
            if (try? fm.contentsOfDirectory(atPath: new.path))?.isEmpty == true {
                try? fm.removeItem(at: new)
            } else {
                NSLog("[hub] \(target): data at both \(old.path) and \(new.path) — leaving both alone")
                return
            }
        }
        do {
            try fm.createDirectory(at: new.deletingLastPathComponent(), withIntermediateDirectories: true)
            try fm.moveItem(at: old, to: new)      // same volume: a rename, not a copy
            let up = String(repeating: "../", count: max(0, legacy.split(separator: "/").count - 1))
            let rel = up + "games/\(target)/" + dataSubpath
            try fm.createSymbolicLink(atPath: old.path, withDestinationPath: rel)
            NSLog("[hub] \(target): adopted \(legacy) -> games/\(target)/\(dataSubpath) (rename; symlink left behind)")
        } catch {
            NSLog("[hub] \(target): could not adopt \(old.path): \(error)")
        }
    }
}

// MARK: - Session summaries

// klL (English or Spanish by the headset's language) lives in KleptonApp.swift:
// the boot window uses it too, in every build.

enum HubSessionText {
    /// "12 min", "45 s", "1 h 05 min".
    static func duration(_ s: Double) -> String {
        let t = Int(s.rounded())
        if t < 60 { return "\(t) s" }
        let m = t / 60
        if m < 60 { return "\(m) min" }
        return String(format: "%d h %02d min", m / 60, m % 60)
    }

    /// The card's line: "Última sesión: 52 fps (1% peor 31) · 12 min · 8 oct".
    static func card(_ r: KLSessionRecord) -> String {
        let head = klL("Last session", "Última sesión")
        let when = r.start.formatted(.dateTime.day().month(.abbreviated))
        guard let avg = r.fpsAvg else {
            return "\(head): \(klL("no game frames", "sin imagen del juego")) · \(duration(r.durationS)) · \(when)"
        }
        let low = r.fps1Low.map { " (\(klL("1% low", "1% peor")) \(Int($0.rounded())))" } ?? ""
        return "\(head): \(Int(avg.rounded())) fps\(low) · \(duration(r.playS)) · \(when)"
    }

    /// Why the session ended, in words.
    static func endReason(_ r: KLSessionRecord) -> String {
        if !r.final { return klL("did not close cleanly (killed or crashed)", "no se cerró limpiamente (cierre forzado o fallo)") }
        switch r.endReason {
        case "exit": return klL("game exited", "el juego terminó")
        case "background": return klL("app closed or backgrounded", "app cerrada o en segundo plano")
        case "render loop ended": return klL("immersive view closed", "vista inmersiva cerrada")
        default: return r.endReason
        }
    }

    static func ms(_ v: Double?) -> String { v.map { String(format: "%.1f ms", $0) } ?? "—" }
    static func fps(_ v: Double?) -> String { v.map { String(format: "%.0f", $0) } ?? "—" }
    static func memory(_ mb: Int) -> String { mb > 0 ? String(format: "%.1f GB", Double(mb) / 1024) : "—" }
}

// MARK: - Library model

@MainActor
final class HubLibraryModel: ObservableObject {
    let games: [KLHubGameRow] = klHubGameRows
    #if canImport(SteamDepot)
    /// One download/sign-in model per downloadable game, alive for the whole
    /// library session so a download keeps going while another game is looked at.
    let content: [String: SteamContentModel]
    /// Latest public manifest per depot, per game (filled by checkUpdates()).
    @Published var latest: [String: [UInt32: UInt64]] = [:]
    /// The account's other Steam VR games, classified (information only).
    @Published var owned: [LibraryEntry]?
    /// Every app the signed-in Steam account owns (nil = library not read yet): which
    /// catalog games Steam can install (KleptonCatalog.swift).
    @Published var steamOwned: Set<UInt32>?
    #endif
    #if canImport(MetaStore)
    /// One Meta download model per catalog game with a Meta recipe.
    private(set) var meta: [String: MetaContentModel] = [:]
    let metaAccount = MetaAccountModel.shared
    #endif
    @Published var account: String?
    /// Per game: why Play was refused (not owned), or the check in progress.
    @Published var playBlock: [String: String] = [:]
    @Published var verifying: String?
    /// The game whose APK-derived files are being prepared before boot (HubPrep).
    @Published var preparing: String?
    @Published var checking = false
    @Published var checkError: String?
    /// Each game's most recent session (KleptonSessionStats.swift), for its card.
    @Published var lastSession: [String: KLSessionRecord] = [:]
    private var relay: [AnyCancellable] = []

    init() {
        HubStorage.migrateLegacyData()
        #if canImport(SteamDepot)
        var c: [String: SteamContentModel] = [:]
        for g in klHubGameRows {
            if let d = SteamGames.descriptor(for: g.target) { c[g.target] = SteamContentModel(game: d) }
        }
        content = c
        // The cards show each game's live state; forward its models' changes to
        // the library (throttled: progress arrives per chunk).
        for m in c.values {
            relay.append(m.objectWillChange
                .throttle(for: .milliseconds(500), scheduler: RunLoop.main, latest: true)
                .sink { [weak self] _ in self?.objectWillChange.send() })
        }
        account = (try? KeychainCredentialStore(service: "SteamDepot.refreshToken", account: "steam").load())?.accountName
        #endif
        #if canImport(MetaStore)
        var mm: [String: MetaContentModel] = [:]
        for g in klHubGameRows {
            if let r = KLCatalog.metaRecipe(g.target) { mm[g.target] = MetaContentModel(target: g.target, recipe: r) }
        }
        meta = mm
        for m in mm.values {
            relay.append(m.objectWillChange
                .throttle(for: .milliseconds(500), scheduler: RunLoop.main, latest: true)
                .sink { [weak self] _ in self?.objectWillChange.send() })
        }
        relay.append(metaAccount.objectWillChange.sink { [weak self] _ in self?.objectWillChange.send() })
        #endif
        reloadSessions()
    }

    // MARK: Sources (the catalog's stores, by ownership)

    enum SourceState: Equatable {
        /// The store says the account owns it.
        case owned
        /// Not known: not signed in to that store, or not checked yet.
        case unknown(String)
        case notOwned
        /// The store sells a build this hub's port can't run.
        case incompatible(String)
    }

    func sourceState(_ g: KLHubGameRow, _ store: KLStore) -> SourceState {
        switch store {
        case .steam:
            #if canImport(SteamDepot)
            guard let app = KLCatalog.entry(g.target)?.steamApp else { return .incompatible("no Steam source") }
            guard let lib = steamOwned else { return .unknown(account == nil ? "Not signed in to Steam" : "Steam library not read yet") }
            return lib.contains(app) ? .owned : .notOwned
            #else
            return .incompatible("Steam is not in this build")
            #endif
        case .meta:
            #if canImport(MetaStore)
            if let why = KLCatalog.metaIncompatibility(g) { return .incompatible(why) }
            guard metaAccount.signedIn else { return .unknown("Not signed in to Meta (metastore login + push-token on the Mac)") }
            guard let app = KLCatalog.metaRecipe(g.target)?.appId, let owns = metaAccount.owns(appId: app) else {
                return .unknown(metaAccount.checking ? "Checking your Meta library…" : "Meta library not checked yet")
            }
            return owns ? .owned : .notOwned
            #else
            return .incompatible("Meta is not in this build")
            #endif
        }
    }

    /// The stores offered as install options: Steam unless Steam says it is not owned (signing in
    /// happens from Download, as always); Meta only when Meta confirms ownership.
    func offeredStores(_ g: KLHubGameRow) -> [KLStore] {
        KLCatalog.stores(g.target).filter { st in
            switch sourceState(g, st) {
            case .owned: return true
            case .unknown: return st == .steam
            case .notOwned, .incompatible: return false
            }
        }
    }

    /// The store the card shows and Play/Download use: one with data on the headset first
    /// (the player's pick when both), else the pick among the offered, else the only one offered.
    func activeStore(_ g: KLHubGameRow) -> KLStore? {
        let installed = KLCatalog.installedStores(g.target)
        let offered = offeredStores(g)
        let chosen = KLCatalog.chosenStore(g.target)
        if let c = chosen, installed.contains(c) || offered.contains(c) { return c }
        if let i = installed.first { return i }
        if let o = offered.first { return o }
        // Partial data from a store that is not offered (e.g. a Steam copy in progress): keep showing it.
        #if canImport(SteamDepot)
        if KLCatalog.stores(g.target).contains(.steam), SteamGames.descriptor(for: g.target)?.looksPresent == true { return .steam }
        #endif
        #if canImport(MetaStore)
        if meta[g.target]?.hasAnything == true || meta[g.target]?.isBusy == true { return .meta }
        #endif
        return nil
    }

    /// The stores the card lets the player pick between (owned/offered, or with data).
    func pickableStores(_ g: KLHubGameRow) -> [KLStore] {
        let set = Set(offeredStores(g) + KLCatalog.installedStores(g.target))
        return KLStore.allCases.filter(set.contains)
    }

    /// Why a catalog game is greyed out (owned nowhere / not signed in anywhere).
    func notInLibraryReason(_ g: KLHubGameRow) -> String {
        let parts = KLCatalog.stores(g.target).map { st -> String in
            switch sourceState(g, st) {
            case .owned: return "\(st.label): owned"
            case .notOwned: return "\(st.label): not owned"
            case .unknown(let w), .incompatible(let w): return "\(st.label): \(w)"
            }
        }
        return parts.isEmpty ? "No store this build can download from" : parts.joined(separator: "\n")
    }

    enum Status: Equatable {
        case notInBuild, notDownloaded, partial, working(String, Double?), verifyScheduled, updateAvailable, ready, manual, notInLibrary
        var label: String {
            switch self {
            case .notInBuild: return "Not in this build"
            case .notDownloaded: return "Not downloaded"
            case .partial: return "Incomplete — not verified"
            case .working(let what, let f): return f.map { "\(what) \(Int(($0 * 100).rounded()))%" } ?? what
            case .verifyScheduled: return "Verify scheduled"
            case .updateAvailable: return "Update available"
            case .ready: return "Ready"
            case .manual: return "Data staged by hand"
            case .notInLibrary: return "Not in your library"
            }
        }
        var color: Color {
            switch self {
            case .ready, .manual: return .green
            case .updateAvailable, .verifyScheduled, .partial: return .orange
            case .working: return .blue
            case .notDownloaded, .notInBuild, .notInLibrary: return .secondary
            }
        }
        var canPlay: Bool {
            switch self {
            case .ready, .manual, .updateAvailable, .partial, .verifyScheduled: return true
            default: return false
            }
        }
    }

    func status(_ g: KLHubGameRow) -> Status {
        if KLCatalog.entry(g.target) != nil {
            switch activeStore(g) {
            case .meta?:
                #if canImport(MetaStore)
                if let m = meta[g.target] { return metaStatus(m) }
                #endif
            case .steam?: break
            case nil: return .notInLibrary
            }
        }
        #if canImport(SteamDepot)
        guard let m = content[g.target] else { return g.bundled ? .manual : .notInBuild }
        if m.isBusy {
            switch m.stage {
            case .working:
                return .working(m.progress.phase == .verifying ? "Verifying" : "Downloading", m.activeFraction)
            default: return .working("Signing in…", nil)
            }
        }
        let local = m.local
        if local.isComplete {
            if UserDefaults.standard.bool(forKey: m.game.verifyNextLaunchKey) { return .verifyScheduled }
            if let newest = latest[g.target], newest.contains(where: { id, gid in local.markers[id] != gid }) {
                return .updateAvailable
            }
            return .ready
        }
        return m.game.looksPresent ? .partial : .notDownloaded
        #else
        return g.bundled ? .manual : .notInBuild
        #endif
    }

    #if canImport(MetaStore)
    private func metaStatus(_ m: MetaContentModel) -> Status {
        if m.isBusy {
            if case .working(let what) = m.stage, m.progress != nil {
                return .working(what.hasPrefix("Downloading") ? "Downloading" : what, m.activeFraction)
            }
            return .working("Checking with Meta…", nil)
        }
        if m.complete { return .ready }
        return m.hasAnything ? .partial : .notDownloaded
    }
    #endif

    func headerURL(_ g: KLHubGameRow) -> URL? {
        #if canImport(SteamDepot)
        return SteamGames.descriptor(for: g.target)?.headerImageURL
        #else
        return nil
        #endif
    }

    /// Play only what the signed-in account owns. Asks the Steam bridge (`hello`;
    /// offline it answers from its cache). An answer of "not owned" refuses; no
    /// answer at all lets the game boot, and the runtime decides (with the bridge
    /// required on device, SteamAPI_Init then fails and the game shows its own
    /// "no Steam" behaviour).
    func checkOwnership(_ target: String, then play: @escaping () -> Void) {
        #if canImport(MetaStore)
        if let g = games.first(where: { $0.target == target }), KLCatalog.entry(target) != nil, activeStore(g) == .meta,
           let app = KLCatalog.metaRecipe(target)?.appId {
            checkMetaOwnership(target, app: app, then: play)
            return
        }
        #endif
        #if canImport(SteamDepot)
        guard let app = SteamGames.descriptor(for: target)?.appID else { play(); return }
        guard verifying == nil else { return }
        verifying = target
        playBlock[target] = nil
        Task {
            defer { verifying = nil }
            do {
                let r = try await KleptonSteamBridge.owns(app: app)
                NSLog("[hub] \(target): app \(app) \(r.owned ? "owned" : "NOT owned") by \(r.name)\(r.offline ? " (offline, cached)" : "")")
                if r.owned { play() } else { playBlock[target] = "Not owned on this Steam account" }
            } catch {
                NSLog("[hub] \(target): ownership not verified (\(error)); booting, the runtime decides")
                play()
            }
        }
        #else
        play()
        #endif
    }

    #if canImport(MetaStore)
    /// Meta's answer (re-asked when older than a day; the last answer stands when Meta can't
    /// be reached). Only a "yes" lets the game boot.
    private func checkMetaOwnership(_ target: String, app: String, then play: @escaping () -> Void) {
        guard verifying == nil else { return }
        verifying = target
        playBlock[target] = nil
        metaAccount.refresh { [weak self] in
            guard let self else { return }
            self.verifying = nil
            switch self.metaAccount.owns(appId: app) {
            case true?:
                NSLog("[hub] \(target): Meta app \(app) owned by user \(self.metaAccount.userId ?? "?")")
                play()
            case false?:
                self.playBlock[target] = "Not owned on this Meta account"
            case nil:
                self.playBlock[target] = self.metaAccount.signedIn
                    ? "Could not confirm with Meta that you own it: \(self.metaAccount.error ?? "no answer")"
                    : "Sign in to Meta (metastore login + push-token on the Mac) to play"
            }
        }
    }
    #endif

    /// Derive what the runtime reads from the downloaded data (HubPrep: the APK
    /// under the runtime's name, the small assets/ entries, the metadata), off the
    /// main thread, then boot. Idempotent and cheap once done (a stamp per APK).
    func prepare(_ target: String, then play: @escaping () -> Void) {
        guard let row = games.first(where: { $0.target == target }), preparing == nil else { return }
        preparing = target
        playBlock[target] = nil
        Task {
            let err = await Task.detached(priority: .userInitiated) { HubPrep.prepare(row) }.value
            preparing = nil
            if let err {
                NSLog("[hub] \(target): could not prepare its data: \(err)")
                playBlock[target] = "Could not prepare the game's files: \(err)"
            } else {
                play()
            }
        }
    }

    /// Forget the saved Steam sign-in (Keychain refresh token).
    func signOut() {
        #if canImport(SteamDepot)
        for m in content.values { m.signOut() }
        try? KeychainCredentialStore(service: "SteamDepot.refreshToken", account: "steam").clear()
        owned = nil
        steamOwned = nil
        latest = [:]
        #endif
        account = nil
        NSLog("[hub] signed out of Steam")
    }

    /// Re-read every game's session history (after the settings sheet cleared one).
    func reloadSessions() {
        var m: [String: KLSessionRecord] = [:]
        for g in games { if let r = KLSessionHistory.load(g.target).last { m[g.target] = r } }
        lastSession = m
    }

    /// The game's files are gone: re-read its local state for the card.
    func refreshAfterDelete(_ target: String) {
        #if canImport(SteamDepot)
        content[target]?.refreshLocal()
        #endif
        #if canImport(MetaStore)
        meta[target]?.refreshLocal()
        if case .complete? = meta[target]?.stage { meta[target]?.stage = .idle }
        #endif
        objectWillChange.send()
    }

    /// With a saved Steam sign-in: compare each game's installed manifests with
    /// Steam's latest, and fetch the account's library for the info section.
    func checkUpdates() {
        #if canImport(SteamDepot)
        let store = KeychainCredentialStore(service: "SteamDepot.refreshToken", account: "steam")
        guard !checking, let creds = try? store.load() else { return }
        account = creds.accountName
        checking = true; checkError = nil
        let games = content.values.map(\.game)
        let hubApps = Set(games.map(\.appID))
        Task {
            let session = SteamSession(log: { NSLog("[hub] %@", $0) })
            defer { Task { await session.disconnect() } }
            do {
                try await session.connect()
                try await session.logOn(creds)
                let dl = SteamDepotDownloader(session: session)
                var found: [String: [UInt32: UInt64]] = [:]
                for g in games {
                    let ids = Set(g.knownDepots)
                    guard !ids.isEmpty else { continue }
                    var m: [UInt32: UInt64] = [:]
                    for d in try await dl.depots(app: g.appID, branch: g.branch) where ids.contains(d.depotID) {
                        if let gid = d.manifestID { m[d.depotID] = gid }
                    }
                    found[g.target] = m
                }
                latest = found
                for m in content.values { m.refreshLocal() }
                let lib = try await SteamLibrary.fetch(session: session, log: { NSLog("[hub] library: %@", $0) })
                steamOwned = Set(lib.map(\.appID))
                owned = lib.filter { $0.isVR && !hubApps.contains($0.appID) }
            } catch {
                NSLog("[hub] update check failed: \(error)")
                checkError = "\(error)"
            }
            checking = false
        }
        #endif
    }
}

// MARK: - Per-game settings

/// One game's settings (KLGameSettings, saved per target under
/// `klepton.game.<target>`): the language it boots in, how it is presented, the
/// controller calibration and (Alyx) render performance. Read when Play is
/// pressed, so a change applies to the next launch. Environment variables set
/// on a scripted launch still win over everything here.
struct HubGameSettingsView: View {
    let game: KLHubGameRow
    @Environment(\.dismiss) private var dismiss
    @State private var s = KLGameSettings()
    @State private var sessions: [KLSessionRecord] = []

    private var base: KLTune { KleptonTuning.shared.baseline }

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    Picker("Language", selection: Binding(
                        get: { KLGameLanguage(rawValue: s.language ?? "system") ?? .system },
                        set: { s.language = $0 == .system ? nil : $0.rawValue; save() })) {
                        ForEach(KLGameSettingsStore.languages(for: game.target)) { l in
                            Text(l.label).tag(l)
                        }
                    }
                } header: { Text("Language") } footer: {
                    Text("Menus, text and subtitles. System follows the headset's language. "
                         + "Spoken audio depends on what the game ships.")
                }

                Section {
                    Picker("Immersion", selection: Binding(
                        get: { s.fullImmersion ?? KLGameSettingsStore.defaultFull(game.target) },
                        set: { s.fullImmersion = $0; save() })) {
                        Text("Full VR").tag(true)
                        Text("Mixed (see the room)").tag(false)
                    }
                    Toggle("Real hands visible", isOn: Binding(
                        get: { s.handMatting ?? KLGameSettingsStore.defaultMatting(game.target) },
                        set: { s.handMatting = $0; save() }))
                    Toggle("Show controllers when not held", isOn: Binding(
                        get: { s.ghostControllers ?? KLGameSettingsStore.defaultGhostControllers },
                        set: { s.ghostControllers = $0; save() }))
                } header: { Text("Display") } footer: {
                    Text("The game draws its own hands, so your real ones are hidden by default. "
                         + "A controller you put down is shown as a grey outline where it rests, "
                         + "and disappears again when you pick it up.")
                }

                Section {
                    cm("Offset X (right)", 0)
                    cm("Offset Y (up)", 1)
                    cm("Offset Z (back)", 2)
                    deg("Pitch", \.sensePitch, \.sensePitch, -90 ... 90)
                    deg("Yaw", \.senseYaw, \.senseYaw, -180 ... 180)
                    deg("Roll", \.senseRoll, \.senseRoll, -180 ... 180)
                    Button("Reset calibration") { s.calibration = nil; save() }
                } header: { Text("Controller calibration") } footer: {
                    Text("Where the game puts your hands relative to the PlayStation VR2 Sense "
                         + "controllers, in the controller's own frame (−Z points forward); X, yaw "
                         + "and roll are mirrored for the left hand. The same values as Controller "
                         + "alignment in the game's window — tuning there is saved here.")
                }

                if KLGameSettingsStore.hasPerformance(game.target) {
                    Section {
                        slider("Render scale", value: s.performance?.renderScale ?? KLGameSettingsStore.defaultRenderScale,
                               range: 0.6 ... 1.2, step: 0.05, format: "%.2f×") { v in
                            perf { $0.renderScale = v }
                        }
                        Picker(klL("Antialiasing", "Antialiasing"), selection: Binding(
                            get: { s.performance?.msaa ?? KLGameSettingsStore.defaultMSAA },
                            set: { v in perf { $0.msaa = v } })) {
                            Text("2×").tag(2)
                            Text("4×").tag(4)
                        }
                        Toggle(klL("Foveated rendering", "Renderizado foveado"), isOn: Binding(
                            get: { s.performance?.foveate ?? KLGameSettingsStore.defaultFoveate },
                            set: { v in perf { $0.foveate = v } }))
                        slider("Overscan margin", value: s.performance?.overscan ?? KLGameSettingsStore.defaultOverscan,
                               range: 0 ... 0.2, step: 0.01, format: "%.2f") { v in
                            perf { $0.overscan = v }
                        }
                        Toggle("Extra pixels for the margin", isOn: Binding(
                            get: { s.performance?.overscanRT ?? KLGameSettingsStore.defaultOverscanRT },
                            set: { v in perf { $0.overscanRT = v } }))
                        Button("Reset performance") { s.performance = nil; save() }
                    } header: { Text("Performance") } footer: {
                        Text("Lower render scale = faster and blurrier. The overscan margin renders "
                             + "past the edge of the view so fast head turns show picture instead of "
                             + "black; extra pixels keep the centre sharp at a GPU cost.\n\n"
                             + klL("Antialiasing 4× looks smoother but costs ~5 ms of GPU per frame "
                                   + "(about 30 fps instead of 45 on M2). Foveated rendering keeps the "
                                   + "centre sharp and lowers the resolution at the edges, saving GPU.",
                                   "Antialiasing 4× se ve más suave pero cuesta ~5 ms de GPU por imagen "
                                   + "(unos 30 fps en vez de 45 en M2). El renderizado foveado mantiene "
                                   + "nítido el centro y baja la resolución en los bordes, ahorrando GPU."))
                    }
                }

                #if canImport(SteamDepot)
                KLCloudSaveSection(target: game.target)
                #endif

                if KLGameSettingsStore.hasPrecompile(game.target) {
                    Section {
                        Toggle(klL("Precompile graphics", "Precompilar gráficos"), isOn: Binding(
                            get: { s.precompileGraphics ?? false },
                            set: { s.precompileGraphics = $0 ? true : nil; save() }))
                        let have = FileManager.default.fileExists(
                            atPath: KLGameSettingsStore.fozURL(game.target).path)
                        Text(have ? klL("Pipeline list found.", "Lista de pipelines encontrada.")
                                  : klL("No pipeline list on the headset yet.",
                                        "Aún no hay lista de pipelines en el visor."))
                            .font(.footnote).foregroundStyle(.secondary)
                    } header: { Text(klL("Graphics", "Gráficos")) } footer: {
                        Text(klL("Before the game starts, compiles the shaders it will need, so walking "
                                 + "into a new area does not freeze the picture. Takes a while each "
                                 + "launch; the progress shows in the game's window.",
                                 "Antes de arrancar el juego compila los shaders que va a usar, para "
                                 + "que la imagen no se congele al entrar en una zona nueva. Tarda un "
                                 + "rato en cada arranque; el progreso sale en la ventana del juego."))
                    }
                }

                Section {
                    Toggle(klL("Performance overlay", "Visor de rendimiento"), isOn: Binding(
                        get: { s.perfOverlay ?? false },
                        set: { s.perfOverlay = $0 ? true : nil; save() }))
                } header: { Text(klL("In-game", "Dentro del juego")) } footer: {
                    Text(klL("A small panel in the game's view with fps, frame and GPU time, reprojected "
                             + "frames, memory and temperature. Siri can turn it on and off while you play "
                             + "(“Turn on the FPS overlay in AVP Hub”), and “Open settings in AVP Hub” opens "
                             + "quick settings inside the game.",
                             "Un panel pequeño en la vista del juego con fps, tiempo de imagen y de GPU, "
                             + "imágenes reproyectadas, memoria y temperatura. Siri lo activa y desactiva "
                             + "mientras juegas («Activa el visor de FPS en AVP Hub»), y «Abre los ajustes en "
                             + "AVP Hub» abre los ajustes rápidos dentro del juego."))
                }

                sessionsSection

                Section {
                    Button("Reset all settings for this game", role: .destructive) {
                        s = KLGameSettings(); save()
                    }
                }
            }
            .navigationTitle(game.display)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) { Button("Done") { dismiss() } }
            }
        }
        .frame(minWidth: 560, minHeight: 640)
        .onAppear {
            s = KLGameSettingsStore.load(game.target)
            sessions = Array(KLSessionHistory.load(game.target).suffix(6).reversed())
        }
    }

    /// The last session in detail, then the five before it, and a way to forget them.
    @ViewBuilder private var sessionsSection: some View {
        if let r = sessions.first {
            Section {
                LabeledContent(klL("Date", "Fecha"),
                               value: r.start.formatted(date: .abbreviated, time: .shortened))
                LabeledContent(klL("Play time", "Tiempo de juego"),
                               value: HubSessionText.duration(r.playS)
                                   + (r.loadingS >= 1 ? " · " + klL("loading", "cargando") + " "
                                      + HubSessionText.duration(r.loadingS) : ""))
                LabeledContent(klL("Frame rate (avg / median)", "Imágenes por segundo (media / mediana)"),
                               value: "\(HubSessionText.fps(r.fpsAvg)) / \(HubSessionText.fps(r.frameMsP50.map { 1000 / $0 })) fps")
                LabeledContent(klL("1% low", "1% peor"), value: "\(HubSessionText.fps(r.fps1Low)) fps")
                if let g = r.gpuMsAvg {
                    LabeledContent(klL("GPU per frame", "GPU por imagen"), value: HubSessionText.ms(g))
                }
                if r.compositorFrames > 0 {
                    LabeledContent(klL("Reprojected", "Reproyectadas"),
                                   value: String(format: "%.0f %%", 100 * Double(r.reprojectedFrames) / Double(r.compositorFrames))
                                       + (r.displayHz.map { String(format: " · %.0f Hz", $0) } ?? ""))
                }
                LabeledContent(klL("Peak memory", "Memoria máxima"), value: HubSessionText.memory(r.peakFootprintMB))
                LabeledContent(klL("Ended", "Final"), value: HubSessionText.endReason(r))
            } header: { Text(klL("Last session", "Última sesión")) }
            .monospacedDigit()
        }
        Section {
            if sessions.isEmpty {
                Text(klL("No sessions recorded yet.", "Aún no hay sesiones registradas."))
                    .foregroundStyle(.secondary)
            } else if sessions.count == 1 {
                Text(klL("No earlier sessions.", "No hay sesiones anteriores."))
                    .foregroundStyle(.secondary)
            } else {
                Grid(alignment: .leading, horizontalSpacing: 16, verticalSpacing: 6) {
                    GridRow {
                        Text(klL("Date", "Fecha"))
                        Text(klL("Play", "Juego"))
                        Text(klL("Avg", "Media"))
                        Text(klL("1% low", "1% peor"))
                        Text("GPU")
                        Text(klL("Peak mem.", "Memoria máx."))
                    }
                    .font(.caption.bold()).foregroundStyle(.secondary)
                    ForEach(sessions.dropFirst()) { r in
                        GridRow {
                            Text(r.start.formatted(date: .abbreviated, time: .shortened))
                                + Text(r.final ? "" : " *").foregroundStyle(.orange)
                            Text(HubSessionText.duration(r.playS))
                            Text(r.fpsAvg.map { String(format: "%.0f fps", $0) } ?? "—")
                            Text(HubSessionText.fps(r.fps1Low))
                            Text(r.gpuMsAvg.map { String(format: "%.1f ms", $0) } ?? "—")
                            Text(HubSessionText.memory(r.peakFootprintMB))
                        }
                        .font(.callout).monospacedDigit()
                    }
                }
            }
            if !sessions.isEmpty {
                Button(klL("Clear history", "Borrar historial"), role: .destructive) {
                    KLSessionHistory.clear(game.target)
                    sessions = []
                }
            }
        } header: { Text(klL("Earlier sessions", "Sesiones anteriores")) } footer: {
            Text(klL("Frame rate the game delivered while playing (loading screens excluded). "
                     + "1% low: the slowest 1% of frames ran at or below this. GPU: the game's own GPU time "
                     + "per frame, recorded only while the performance overlay (or KL_GPU_TIME) was on. "
                     + "* = the session did not close cleanly; its figures are from up to 30 s before the end.",
                     "Imágenes por segundo que dio el juego mientras se jugaba (sin pantallas de carga). "
                     + "1% peor: el 1% más lento de las imágenes fue a esta velocidad o menos. GPU: tiempo de "
                     + "GPU del juego por imagen, solo registrado con el visor de rendimiento (o KL_GPU_TIME) activo. "
                     + "* = la sesión no se cerró limpiamente; sus datos son de hasta 30 s antes del final."))
        }
    }

    private func save() { KLGameSettingsStore.save(s, for: game.target) }

    private func perf(_ f: (inout KLPerformance) -> Void) {
        var p = s.performance ?? KLPerformance()
        f(&p)
        s.performance = p
        save()
    }

    private func cal(_ f: (inout KLCalibration) -> Void) {
        var c = s.calibration ?? KLCalibration()
        f(&c)
        s.calibration = c == KLCalibration() ? nil : c
        save()
    }

    /// One axis of the Sense offset, shown in centimetres, ±30 cm.
    private func cm(_ label: String, _ i: Int) -> some View {
        let b = [base.sensePos.x, base.sensePos.y, base.sensePos.z]
        let cur = s.calibration?.senseOffset ?? b
        return slider(label, value: cur[i] * 100, range: -30 ... 30, step: 0.5, format: "%+.1f cm") { v in
            cal { c in
                var o = c.senseOffset ?? b
                o[i] = v / 100
                c.senseOffset = o
            }
        }
    }

    private func deg(_ label: String, _ kc: WritableKeyPath<KLCalibration, Float?>,
                     _ kt: KeyPath<KLTune, Float>, _ range: ClosedRange<Float>) -> some View {
        slider(label, value: s.calibration?[keyPath: kc] ?? base[keyPath: kt],
               range: range, step: 0.5, format: "%+.1f°") { v in
            cal { $0[keyPath: kc] = v }
        }
    }

    private func slider(_ label: String, value: Float, range: ClosedRange<Float>, step: Float,
                        format: String, set: @escaping (Float) -> Void) -> some View {
        HStack {
            Text(label).frame(width: 160, alignment: .leading)
            Slider(value: Binding(get: { value }, set: set), in: range, step: step)
            Text(String(format: format, value))
                .font(.system(.body, design: .monospaced)).frame(width: 96, alignment: .trailing)
        }
    }

    /// The one-line summary on the game's card.
    static func summary(_ target: String) -> String {
        let lang = KLGameSettingsStore.language(for: target)
        let full = KLGameSettingsStore.fullImmersion(for: target)
        let hands = KLGameSettingsStore.handMatting(for: target)
        let cal = KLGameSettingsStore.load(target).calibration != nil ? " · Calibrated" : ""
        return "\(lang.label) · \(full ? "Full VR" : "Mixed") · "
            + "\(hands ? "Real hands shown" : "Real hands hidden")\(cal)"
    }
}

// MARK: - App settings

/// The hub's own settings: the Steam account, and each game's storage with a
/// way to delete the downloaded data.
struct HubAppSettingsView: View {
    @ObservedObject var model: HubLibraryModel
    @Environment(\.dismiss) private var dismiss
    @State private var sizes: [String: UInt64] = [:]
    @State private var confirmDelete: KLHubGameRow?
    @State private var confirmSignOut = false
    @State private var error: String?

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    if let a = model.account {
                        LabeledContent("Signed in as", value: a)
                        Button("Sign out of Steam", role: .destructive) { confirmSignOut = true }
                    } else {
                        Text("Not signed in. Signing in happens from a game's Download button.")
                            .foregroundStyle(.secondary)
                    }
                } header: { Text("Steam account") }

                #if canImport(MetaStore)
                MetaAccountSection()
                #endif

                Section {
                    ForEach(model.games) { g in
                        HStack {
                            VStack(alignment: .leading) {
                                Text(g.display)
                                Text(sizes[g.target].map { ByteCountFormatter.string(fromByteCount: Int64($0), countStyle: .file) }
                                     ?? "Measuring…")
                                    .font(.caption).foregroundStyle(.secondary).monospacedDigit()
                            }
                            Spacer()
                            Button("Delete game data", role: .destructive) { confirmDelete = g }
                                .disabled((sizes[g.target] ?? 0) == 0 || HubStorage.isBusy(model, g.target))
                        }
                    }
                    if let error { Text(error).font(.caption).foregroundStyle(.red) }
                } header: { Text("Storage") } footer: {
                    Text("Deletes the files downloaded from Steam or Meta and those derived from them. "
                         + "Saved games and settings are kept. Download again from the game's card.")
                }
            }
            .navigationTitle("Settings")
            .toolbar {
                ToolbarItem(placement: .confirmationAction) { Button("Done") { dismiss() } }
            }
        }
        .frame(minWidth: 560, minHeight: 480)
        .task { await measure() }
        .confirmationDialog("Sign out of Steam?", isPresented: $confirmSignOut, titleVisibility: .visible) {
            Button("Sign out", role: .destructive) { model.signOut() }
        } message: {
            Text("Downloads and ownership checks will need you to sign in again.")
        }
        .alert(confirmDelete.map { "Delete \($0.display) data?" } ?? "",
               isPresented: Binding(get: { confirmDelete != nil }, set: { if !$0 { confirmDelete = nil } }),
               presenting: confirmDelete) { g in
            Button("Delete \(sizeText(g))", role: .destructive) { delete(g) }
            Button("Cancel", role: .cancel) {}
        } message: { g in
            Text("This permanently deletes \(sizeText(g)) of \(g.display) from this headset. "
                 + "Playing it again means downloading it all again"
                 + (g.target == "alyx" ? " — about 72 GB." : "."))
        }
    }

    private func sizeText(_ g: KLHubGameRow) -> String {
        sizes[g.target].map { ByteCountFormatter.string(fromByteCount: Int64($0), countStyle: .file) } ?? "the data"
    }

    private func measure() async {
        let rows = model.games
        let r = await Task.detached(priority: .utility) { () -> [String: UInt64] in
            var out: [String: UInt64] = [:]
            for g in rows { out[g.target] = HubStorage.dataURLs(g).reduce(0) { $0 + HubStorage.size(of: $1) } }
            return out
        }.value
        sizes = r
    }

    private func delete(_ g: KLHubGameRow) {
        error = nil
        sizes[g.target] = nil
        Task {
            let err = await Task.detached(priority: .userInitiated) { HubStorage.deleteData(g) }.value
            if let err { error = "Could not delete all of \(g.display): \(err)" }
            model.refreshAfterDelete(g.target)
            await measure()
        }
    }
}

// MARK: - Library UI

struct HubLibraryView: View {
    let onPlay: (String) -> Void
    @StateObject private var model = HubLibraryModel()
    #if canImport(SteamDepot)
    @ObservedObject private var cloud = KLCloudSaveModel.shared
    @MainActor private static var openedOnce = false
    #endif
    @State private var detail: String?
    /// Which store's download page `detail` shows.
    @State private var detailStore: KLStore = .steam
    /// The game whose settings sheet is open.
    @State private var settingsFor: KLHubGameRow?
    @State private var showAppSettings = false

    var body: some View {
        Group {
            if let t = detail, let page = detailPage(t) {
                page
            } else {
                library
            }
        }
        // No game has booted: nothing to tear down on backgrounding, and exiting
        // would kill a download. HubLaunch.select hands this back.
        .onAppear {
            Lifecycle.keepAlive = true; model.checkUpdates()
            #if canImport(MetaStore)
            model.metaAccount.refresh()
            #endif
            #if canImport(SteamDepot)
            // Once per app open: a game session ends by closing Klepton, so this is "after quit" —
            // where a two-way sync sends that session's saves up.
            if !Self.openedOnce { Self.openedOnce = true; KLCloudSaveModel.shared.syncAll(why: "app open") }
            #endif
        }
        .sheet(item: $settingsFor, onDismiss: { model.reloadSessions() }) { g in HubGameSettingsView(game: g) }
        .sheet(isPresented: $showAppSettings) { HubAppSettingsView(model: model) }
    }

    /// The download page of the store the card opened.
    private func detailPage(_ t: String) -> AnyView? {
        #if canImport(MetaStore)
        if detailStore == .meta, let m = model.meta[t] {
            return AnyView(MetaContentView(model: m, onPlay: { play(t) }, onBack: { detail = nil; m.refreshLocal() }))
        }
        #endif
        #if canImport(SteamDepot)
        if detailStore == .steam, let m = model.content[t] {
            return AnyView(SteamContentView(model: m, onPlay: { play(t) }, onBack: { detail = nil; m.refreshLocal() }))
        }
        #endif
        return nil
    }

    private func play(_ target: String) {
        detail = nil          // back to the card, where a refusal is shown
        #if canImport(SteamDepot)
        // Steam Cloud saves (off unless turned on for this game): pulled in before the game's
        // files are prepared, bounded, and finished or cancelled before boot (KleptonCloudSaves.swift).
        model.checkOwnership(target) {
            KLCloudSaveModel.shared.syncBeforePlay(target) { model.prepare(target) { onPlay(target) } }
        }
        #else
        model.checkOwnership(target) { model.prepare(target) { onPlay(target) } }
        #endif
    }

    private var library: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 24) {
                header
                LazyVGrid(columns: [GridItem(.adaptive(minimum: 320, maximum: 420), spacing: 24)],
                          alignment: .leading, spacing: 24) {
                    ForEach(model.games) { g in card(g) }
                }
                Label("One game per session: when a game ends, Klepton closes. Open Klepton again "
                      + "to come back to this library and pick another game.",
                      systemImage: "info.circle")
                    .font(.footnote).foregroundStyle(.secondary)
                ownedSection
            }
            .padding(32)
        }
    }

    private var header: some View {
        HStack(alignment: .firstTextBaseline) {
            Text("Klepton").font(.extraLargeTitle2.bold())
            Text("Library").font(.title).foregroundStyle(.secondary)
            Spacer()
            if let a = model.account {
                Text("Steam: \(a)").font(.footnote).foregroundStyle(.secondary)
            }
            #if canImport(MetaStore)
            if model.metaAccount.signedIn {
                Text("Meta: \(model.metaAccount.userId ?? "signed in")").font(.footnote).foregroundStyle(.secondary)
            }
            #endif
            if model.checking {
                ProgressView().controlSize(.small)
            } else if model.account != nil {
                Button("Check for updates") { model.checkUpdates() }.font(.footnote)
            }
            Button { showAppSettings = true } label: { Label("Settings", systemImage: "gearshape") }
                .font(.footnote)
        }
    }

    private func card(_ g: KLHubGameRow) -> some View {
        let st = model.status(g)
        let active = model.activeStore(g)
        let pickable = model.pickableStores(g)
        return VStack(alignment: .leading, spacing: 10) {
            cover(g)
            HStack(spacing: 8) {
                Text(g.display).font(.title3.bold()).lineLimit(1)
                Spacer(minLength: 4)
                // One pill per store this game can come from: owned = highlighted, otherwise dimmed.
                ForEach(KLCatalog.stores(g.target)) { store in
                    storeBadge(store, owned: model.sourceState(g, store) == .owned, active: store == active)
                }
            }
            HStack(spacing: 6) {
                Circle().fill(st.color).frame(width: 8, height: 8)
                Text(st.label).font(.callout).foregroundStyle(st.color)
                    .monospacedDigit()
            }
            // Owned in more than one store: the player picks which one installs/plays it (remembered).
            if pickable.count > 1, let a = active {
                Picker("Store", selection: Binding(get: { a }, set: { KLCatalog.choose($0, for: g.target); model.objectWillChange.send() })) {
                    ForEach(pickable) { Text($0.label).tag($0) }
                }
                .pickerStyle(.segmented)
                .disabled(HubStorage.isBusy(model, g.target))
            }
            if st == .notInLibrary {
                Text(model.notInLibraryReason(g)).font(.caption).foregroundStyle(.secondary)
            }
            if case .working(_, let f?) = st { ProgressView(value: f) }
            if let why = model.playBlock[g.target] {
                Label(why, systemImage: "xmark.octagon").font(.callout).foregroundStyle(.red)
            } else if model.verifying == g.target {
                HStack(spacing: 6) { ProgressView().controlSize(.small); Text("Checking your Steam license…").font(.callout) }
            } else if model.preparing == g.target {
                HStack(spacing: 6) { ProgressView().controlSize(.small); Text("Preparing the game's files…").font(.callout) }
            }
            #if canImport(SteamDepot)
            if let c = klCloudSaveCardLine(g.target) {
                Label(c.0, systemImage: "icloud").font(.callout).foregroundStyle(c.1)
            }
            #endif
            if !g.bundled {
                Text("This build carries no game code (library-only build).").font(.caption).foregroundStyle(.secondary)
            }
            HStack(spacing: 12) {
                Button { play(g.target) } label: { Label("Play", systemImage: "play.fill") }
                    .buttonStyle(.borderedProminent)
                    .disabled(!st.canPlay || !g.bundled || model.verifying != nil || model.preparing != nil || cloudBusy(g.target))
                if let a = active, hasDownloadPage(g.target, a), st != .notInLibrary {
                    Button(manageTitle(st)) { detailStore = a; detail = g.target }
                }
                Spacer()
                Button { settingsFor = g } label: { Label("Settings", systemImage: "gearshape") }
                    .labelStyle(.iconOnly)
                    .help("Language and immersion for this game")
            }
            Text(HubGameSettingsView.summary(g.target))
                .font(.caption).foregroundStyle(.secondary)
            if let r = model.lastSession[g.target] {
                Text(HubSessionText.card(r))
                    .font(.caption).foregroundStyle(.secondary).monospacedDigit()
            }
        }
        .padding(16)
        .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 24))
        .opacity(st == .notInLibrary ? 0.55 : 1)
    }

    /// A plain text pill per store (no logo artwork): Steam slate blue, Meta bright blue.
    private func storeBadge(_ store: KLStore, owned: Bool, active: Bool) -> some View {
        let tint = store == .steam ? Color(red: 0.16, green: 0.29, blue: 0.48) : Color(red: 0.0, green: 0.42, blue: 0.95)
        return Label(store.label, systemImage: store == .steam ? "gamecontroller.fill" : "eyeglasses")
            .labelStyle(.titleAndIcon)
            .font(.caption.bold())
            .padding(.horizontal, 8).padding(.vertical, 3)
            .foregroundStyle(owned ? Color.white : Color.secondary)
            .background(owned ? tint : Color.gray.opacity(0.18), in: Capsule())
            .overlay(Capsule().strokeBorder(active ? Color.white.opacity(0.8) : .clear, lineWidth: 1))
            .opacity(owned ? 1 : 0.6)
            .help(owned ? "Owned on \(store.label)" : "\(store.label): not owned or not checked")
    }

    private func hasDownloadPage(_ target: String, _ store: KLStore) -> Bool {
        switch store {
        case .steam:
            #if canImport(SteamDepot)
            return model.content[target] != nil
            #else
            return false
            #endif
        case .meta:
            #if canImport(MetaStore)
            return model.meta[target] != nil
            #else
            return false
            #endif
        }
    }

    /// A cloud-save sync for this game is running (Play waits for it).
    private func cloudBusy(_ target: String) -> Bool {
        #if canImport(SteamDepot)
        return cloud.running.contains(target)
        #else
        return false
        #endif
    }

    private func manageTitle(_ st: HubLibraryModel.Status) -> String {
        switch st {
        case .notDownloaded: return "Download"
        case .partial: return "Complete download"
        case .updateAvailable: return "Update"
        case .working: return "Show progress"
        default: return "Manage"
        }
    }

    private func cover(_ g: KLHubGameRow) -> some View {
        // Steam header art is 460x215.
        ZStack {
            RoundedRectangle(cornerRadius: 14).fill(.quaternary)
            Text(g.display).font(.headline).foregroundStyle(.secondary).padding()
            if let url = model.headerURL(g) {
                AsyncImage(url: url) { phase in
                    if let img = phase.image { img.resizable().scaledToFill() } else { Color.clear }
                }
            }
        }
        .aspectRatio(460.0 / 215.0, contentMode: .fit)
        .clipShape(RoundedRectangle(cornerRadius: 14))
    }

    @ViewBuilder private var ownedSection: some View {
        #if canImport(SteamDepot)
        DisclosureGroup("Your other Steam VR games") {
            VStack(alignment: .leading, spacing: 8) {
                Text("Information only: what your Steam account owns and whether it could run here "
                     + "(an ARM64 build exists) — each game still needs its own port before it can join this library.")
                    .font(.footnote).foregroundStyle(.secondary)
                if let owned = model.owned {
                    if owned.isEmpty { Text("No other VR games on this account.").foregroundStyle(.secondary) }
                    ForEach(owned) { e in
                        HStack {
                            Text(e.name).lineLimit(1)
                            Spacer()
                            Text(ownedLabel(e)).font(.caption.bold())
                                .padding(.horizontal, 8).padding(.vertical, 3)
                                .background(ownedColor(e).opacity(0.25), in: Capsule())
                        }
                    }
                } else if model.checking {
                    HStack { ProgressView(); Text("Reading your Steam library…") }
                } else if let err = model.checkError {
                    Text("Could not read your Steam library: \(err)").font(.footnote).foregroundStyle(.orange)
                } else if model.account == nil {
                    Text("Sign in to Steam (Download on any game) to see the rest of your library.")
                        .font(.footnote).foregroundStyle(.secondary)
                }
            }
            .padding(.top, 8)
        }
        .font(.callout)
        #endif
    }

    #if canImport(SteamDepot)
    private func ownedLabel(_ e: LibraryEntry) -> String {
        switch e.status {
        case .playable: return "Has a port (\(e.kleptonTarget ?? "?")), not in this build"
        case .candidate: return "ARM64 build — candidate"
        case .unsupported: return "x86 only"
        }
    }
    private func ownedColor(_ e: LibraryEntry) -> Color {
        switch e.status {
        case .playable: return .green
        case .candidate: return .blue
        case .unsupported: return .gray
        }
    }
    #endif
}
#endif
