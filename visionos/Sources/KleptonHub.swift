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

/// One game the hub carries (generated from targets.py; see gen_xcodeproj.py).
struct KLHubGameRow: Identifiable, Hashable {
    let target: String
    let display: String
    let kind: String
    let entry: String
    /// False in a UI-only build (KLEPTON_HUB_NO_GUESTS): no frameworks, cannot boot.
    let bundled: Bool
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
        guard klHubGameRows.contains(where: { $0.target == t && $0.bundled }) else {
            NSLog("[hub] KL_HUB_GAME=\(t) is not a game this build carries; showing the library")
            return nil
        }
        select(t)
        return t
    }

    /// The selected game becomes the runtime's target. Must run before BootView is
    /// created (its state reads klTargetName()) and before kl_app_configure.
    static func select(_ target: String) {
        setenv("KL_TARGET", target, 1)
        UserDefaults.standard.set(target, forKey: lastGameKey)
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
    #endif
    @Published var account: String?
    @Published var checking = false
    @Published var checkError: String?
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
    }

    enum Status: Equatable {
        case notInBuild, notDownloaded, partial, working(String, Double?), verifyScheduled, updateAvailable, ready, manual
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
            }
        }
        var color: Color {
            switch self {
            case .ready, .manual: return .green
            case .updateAvailable, .verifyScheduled, .partial: return .orange
            case .working: return .blue
            case .notDownloaded, .notInBuild: return .secondary
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

    func headerURL(_ g: KLHubGameRow) -> URL? {
        #if canImport(SteamDepot)
        return SteamGames.descriptor(for: g.target)?.headerImageURL
        #else
        return nil
        #endif
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

// MARK: - Library UI

struct HubLibraryView: View {
    let onPlay: (String) -> Void
    @StateObject private var model = HubLibraryModel()
    @State private var detail: String?

    var body: some View {
        Group {
            #if canImport(SteamDepot)
            if let t = detail, let m = model.content[t] {
                SteamContentView(model: m, onPlay: { play(t) }, onBack: { detail = nil; m.refreshLocal() })
            } else {
                library
            }
            #else
            library
            #endif
        }
        // No game has booted: nothing to tear down on backgrounding, and exiting
        // would kill a download. HubLaunch.select hands this back.
        .onAppear { Lifecycle.keepAlive = true; model.checkUpdates() }
    }

    private func play(_ target: String) { onPlay(target) }

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
            if model.checking {
                ProgressView().controlSize(.small)
            } else if model.account != nil {
                Button("Check for updates") { model.checkUpdates() }.font(.footnote)
            }
        }
    }

    private func card(_ g: KLHubGameRow) -> some View {
        let st = model.status(g)
        return VStack(alignment: .leading, spacing: 10) {
            cover(g)
            Text(g.display).font(.title3.bold()).lineLimit(1)
            HStack(spacing: 6) {
                Circle().fill(st.color).frame(width: 8, height: 8)
                Text(st.label).font(.callout).foregroundStyle(st.color)
                    .monospacedDigit()
            }
            if case .working(_, let f?) = st { ProgressView(value: f) }
            if !g.bundled {
                Text("This build carries no game code (library-only build).").font(.caption).foregroundStyle(.secondary)
            }
            HStack(spacing: 12) {
                Button { play(g.target) } label: { Label("Play", systemImage: "play.fill") }
                    .buttonStyle(.borderedProminent)
                    .disabled(!st.canPlay || !g.bundled)
                #if canImport(SteamDepot)
                if model.content[g.target] != nil {
                    Button(manageTitle(st)) { detail = g.target }
                }
                #endif
            }
        }
        .padding(16)
        .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 24))
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
