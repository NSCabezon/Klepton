// iCloud backups of the Meta games' saves.
//
// Meta has no cloud saves this port can reach, so the hub keeps its own: each game's save files
// (SaveKit's SaveTable: which files under the hub's data container are saves) are copied into
// iCloud Drive, <ubiquity container>/Documents/Partidas/<target>/<stamp>/ (SaveKit's SaveVault:
// manifest + files, same format as the Mac app's copies). Only for a game played from its Meta copy
// (KLCatalog.activeStore == .meta): a game played from Steam keeps Steam Cloud (KleptonCloudSaves).
//
// On per game by default (UserDefaults `kl.icloudsaves.<target>`). When it runs:
// - when the library appears (the next app open after a session: one game per process, so that
//   is "after quit"), and again when the hub comes back to the front (throttled): a new backup
//   ("auto") only when the saves changed since the newest one, then the oldest beyond 10 go;
// - right before Play: a game with NO saves on this headset gets the newest backup restored
//   (brought down from iCloud first, bounded: at the deadline it boots without it and says so).
//   A game with saves here is never restored automatically.
// - by hand in the game's settings: "Back up now", and "Restore" of any backup (the current saves
//   are backed up first as "pre-restore", so a restore can always be undone).
//
// No iCloud (no account, or a build without KLEPTON_ICLOUD=1: no entitlement, so
// url(forUbiquityContainerIdentifier:) is nil): the section says so and nothing else runs.
// Vault writes and restores run under NSFileCoordinator at the game's directory, off the main actor.
#if KL_HUB && canImport(SaveKit)
import SwiftUI
import UIKit
import Foundation
import SaveKit

enum KLICloudSaves {
    /// Backups kept per game (the newest always stays).
    static let keep = 10
    /// How long Play waits for a backup to come down from iCloud before booting without it.
    static let playDownloadLimit: Double = 20
    /// A download started by hand from the settings.
    static let manualDownloadLimit: Double = 120

    static func key(_ target: String) -> String { "kl.icloudsaves.\(target)" }

    /// On unless turned off for this game.
    static func enabled(_ target: String) -> Bool { UserDefaults.standard.object(forKey: key(target)) as? Bool ?? true }

    /// A game with a Meta store copy whose saves the save table knows.
    static func supported(_ target: String) -> Bool {
        KLCatalog.stores(target).contains(.meta) && SaveTable.location(for: target) != nil
    }

    /// Played from its Meta copy (see the header). Stats the game's files: once per run / sheet.
    static func playsFromMeta(_ target: String) -> Bool { KLCatalog.activeStore(target) == .meta }

    /// The hub's data container, where the games' saves are ($HOME on the headset).
    static var containerRoot: URL { URL(fileURLWithPath: NSHomeDirectory(), isDirectory: true) }

    /// The vault's root in iCloud Drive, or nil without iCloud. Blocks (Apple: never on the main thread).
    static func vaultRoot() -> URL? {
        guard let c = FileManager.default.url(forUbiquityContainerIdentifier: nil) else { return nil }
        let root = c.appendingPathComponent("Documents/Partidas", isDirectory: true)
        try? FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        return root
    }

    /// `body` under a coordinated write of the game's vault directory (snapshot, prune). The
    /// directory is never moved, so the accessor's URL is the vault's own.
    static func writing<T>(_ vault: SaveVault, _ target: String, _ body: () throws -> T) throws -> T {
        var err: NSError?
        var out: Swift.Result<T, Error>?
        NSFileCoordinator(filePresenter: nil).coordinate(writingItemAt: vault.directory(target), options: [], error: &err) { _ in
            out = Swift.Result { try body() }
        }
        if let err { throw err }
        guard let out else { throw CocoaError(.fileWriteUnknown) }
        return try out.get()
    }

    /// `body` reading backup `stamp` and writing the game's vault directory (restore: it also
    /// makes the "pre-restore" backup there).
    static func restoring<T>(_ vault: SaveVault, _ target: String, _ stamp: String, _ body: () throws -> T) throws -> T {
        var err: NSError?
        var out: Swift.Result<T, Error>?
        let dir = vault.directory(target)
        NSFileCoordinator(filePresenter: nil).coordinate(readingItemAt: dir.appendingPathComponent(stamp, isDirectory: true), options: [],
                                                         writingItemAt: dir, options: [], error: &err) { _, _ in
            out = Swift.Result { try body() }
        }
        if let err { throw err }
        guard let out else { throw CocoaError(.fileReadUnknown) }
        return try out.get()
    }

    static func reasonLabel(_ r: String) -> String {
        switch r {
        case "auto": return klL("Automatic", "Automática")
        case "manual": return klL("Manual", "Manual")
        case "pre-restore": return klL("Before a restore", "Antes de restaurar")
        case "all": return klL("All games", "Todos los juegos")
        default: return r
        }
    }

    static func ago(_ d: Date) -> String {
        let f = RelativeDateTimeFormatter()
        f.locale = Locale(identifier: klL("en", "es"))
        f.unitsStyle = .short
        return f.localizedString(for: min(d, Date()), relativeTo: Date())
    }
}

@MainActor
final class KLICloudSaveModel: ObservableObject {
    static let shared = KLICloudSaveModel()

    enum Availability { case unknown, available, unavailable }
    @Published private(set) var availability: Availability = .unknown
    /// Per game: its backups, newest first (after each run, and when the settings open).
    @Published private(set) var entries: [String: [SaveVault.Entry]] = [:]
    /// Per game: the run in progress, in a few words (backup, download, restore).
    @Published private(set) var working: [String: String] = [:]
    /// Per game: the last run's one-line result.
    @Published private(set) var lastResult: [String: String] = [:]
    /// Per game: what the card should say after Play (a backup that did not come down in time).
    @Published private(set) var notice: [String: String] = [:]
    /// Games on and played from Meta at the last check: the ones whose card shows the backup line.
    @Published private(set) var eligible: Set<String> = []

    private var vault: SaveVault?
    private var resolving: Task<SaveVault?, Never>?
    private var tasks: [String: Task<Void, Never>] = [:]
    private var lastUploadAll: Date?
    /// The headset's name, written into each backup's manifest.
    private let device = UIDevice.current.name

    private init() {}

    /// The vault, resolving iCloud once (off the main thread). `retry`: ask again after "unavailable"
    /// (signed in to iCloud since).
    func resolve(retry: Bool = false) async -> SaveVault? {
        if let vault { return vault }
        if availability == .unavailable, !retry { return nil }
        if let resolving { return await resolving.value }
        let t = Task.detached(priority: .utility) { KLICloudSaves.vaultRoot().map { SaveVault(root: $0, stagingRoot: FileManager.default.temporaryDirectory.appendingPathComponent("icloud-staging", isDirectory: true)) } }
        resolving = t
        let v = await t.value
        resolving = nil
        vault = v
        availability = v == nil ? .unavailable : .available
        NSLog("%@", v == nil ? "[icloud] iCloud not available (no iCloud account, or a build without KLEPTON_ICLOUD=1)"
                             : "[icloud] iCloud available; save backups on")
        return v
    }

    func setEnabled(_ target: String, _ on: Bool) {
        UserDefaults.standard.set(on, forKey: KLICloudSaves.key(target))
        if on { eligible.insert(target) } else { eligible.remove(target) }
        NSLog("%@", "[icloud] \(target): automatic backups \(on ? "on" : "off")")
    }

    /// Library shown (app open): back up every game that is on and played from Meta.
    func uploadAll(why: String) {
        lastUploadAll = Date()
        let on = klHubGameRows.map(\.target).filter {
            KLICloudSaves.supported($0) && KLICloudSaves.enabled($0) && KLICloudSaves.playsFromMeta($0)
        }
        eligible = Set(on)
        guard !on.isEmpty else { return }
        Task {
            guard await resolve(retry: true) != nil else { return }
            for t in on { upload(t, reason: "auto", why: why) }
        }
    }

    /// Back in the front: the same, at most every few minutes (a session ends by closing the app,
    /// so this rarely finds anything new; it also notices an iCloud sign-in).
    func foregrounded() {
        if let t = lastUploadAll, Date().timeIntervalSince(t) < 300 { return }
        uploadAll(why: "foreground")
    }

    func upload(_ target: String, reason: String, why: String) {
        guard tasks[target] == nil, let vault else { return }
        working[target] = klL("Backing up saves to iCloud…", "Subiendo copia de las partidas a iCloud…")
        let device = self.device
        tasks[target] = Task {
            let msg = await Task.detached(priority: .utility) {
                Self.runUpload(vault, target: target, device: device, reason: reason, why: why)
            }.value
            lastResult[target] = msg
            await finish(target, vault)
        }
    }

    /// Before Play: a game with no saves here gets its newest backup back (see the header), then
    /// `play`. Always ends in `play`: no iCloud, nothing to restore or a failure boot as usual.
    func restoreBeforePlay(_ target: String, then play: @escaping () -> Void) {
        guard KLICloudSaves.supported(target), KLICloudSaves.enabled(target), KLICloudSaves.playsFromMeta(target) else { play(); return }
        notice[target] = nil
        Task {
            // A backup of this game still running (library just opened): let it finish first.
            if let t = tasks[target] { await t.value }
            guard let vault = await resolve() else { play(); return }
            working[target] = klL("Checking iCloud backups…", "Comprobando copias en iCloud…")
            let job = Task { await autoRestore(vault, target) }
            tasks[target] = job
            await job.value
            await finish(target, vault)
            play()
        }
    }

    private func autoRestore(_ vault: SaveVault, _ target: String) async {
        let (local, newest) = await Task.detached(priority: .userInitiated) { () -> (Bool, SaveVault.Entry?) in
            if vault.hasLocalSaves(target: target, containerRoot: KLICloudSaves.containerRoot) { return (true, nil) }
            return (false, vault.list(target: target).first)
        }.value
        if local { return }
        guard let newest else {
            NSLog("%@", "[icloud] \(target): no saves here and no backup in iCloud; nothing to restore")
            return
        }
        NSLog("%@", "[icloud] \(target): no saves here; restoring backup \(newest.stamp) before play")
        guard await bringDown(vault, target, newest.stamp, limit: KLICloudSaves.playDownloadLimit) else {
            NSLog("%@", "[icloud] \(target): backup \(newest.stamp) not downloaded in \(Int(KLICloudSaves.playDownloadLimit)) s; playing without it")
            notice[target] = klL("The iCloud backup did not download in time: played without it.",
                                 "La copia de iCloud no se descargó a tiempo: se jugó sin ella.")
            return
        }
        working[target] = klL("Restoring saves from iCloud…", "Restaurando partidas desde iCloud…")
        let device = self.device
        let msg = await Task.detached(priority: .userInitiated) {
            Self.runRestore(vault, target: target, stamp: newest.stamp, device: device, why: "before play")
        }.value
        lastResult[target] = msg
    }

    /// Restore a backup by hand (settings; the game is not running: one game per process).
    func restore(_ target: String, stamp: String) {
        guard tasks[target] == nil, let vault else { return }
        working[target] = klL("Restoring…", "Restaurando…")
        let device = self.device
        tasks[target] = Task {
            let msg = await Task.detached(priority: .userInitiated) {
                Self.runRestore(vault, target: target, stamp: stamp, device: device, why: "manual")
            }.value
            lastResult[target] = msg
            await finish(target, vault)
        }
    }

    /// Bring a backup down from iCloud by hand (settings).
    func download(_ target: String, stamp: String) {
        guard tasks[target] == nil, let vault else { return }
        tasks[target] = Task {
            let ok = await bringDown(vault, target, stamp, limit: KLICloudSaves.manualDownloadLimit)
            lastResult[target] = ok ? klL("Backup downloaded.", "Copia descargada.")
                                    : klL("Still downloading from iCloud; try again later.", "Aún descargándose de iCloud; vuelve a intentarlo más tarde.")
            await finish(target, vault)
        }
    }

    /// Re-read a game's backups (settings opened).
    func refresh(_ target: String) {
        Task {
            guard let vault = await resolve() else { return }
            await reload(target, vault)
        }
    }

    /// Asks iCloud for every item of backup `stamp` still a placeholder and waits until none is
    /// left (true) or `limit` seconds pass (false). The manifest comes first; its files' placeholders
    /// show up after it, so each round asks for the new ones.
    private func bringDown(_ vault: SaveVault, _ target: String, _ stamp: String, limit: Double) async -> Bool {
        let deadline = Date().addingTimeInterval(limit)
        var asked = Set<URL>()
        while true {
            let already = asked
            let pending = await Task.detached(priority: .userInitiated) { () -> [URL] in
                let p = vault.pendingDownloads(target: target, stamp: stamp)
                for u in p where !already.contains(u) {
                    do { try FileManager.default.startDownloadingUbiquitousItem(at: u) } catch {
                        NSLog("%@", "[icloud] \(target): could not start downloading \(u.lastPathComponent): \(error)")
                    }
                }
                return p
            }.value
            if pending.isEmpty { return true }
            asked.formUnion(pending)
            working[target] = klL("Downloading the iCloud backup… (\(pending.count) file\(pending.count == 1 ? "" : "s") left)",
                                  "Descargando la copia de iCloud… (\(pending.count == 1 ? "queda 1 archivo" : "quedan \(pending.count) archivos"))")
            if Date() >= deadline { return false }
            try? await Task.sleep(nanoseconds: 500_000_000)
        }
    }

    private func finish(_ target: String, _ vault: SaveVault) async {
        tasks[target] = nil
        working[target] = nil
        await reload(target, vault)
    }

    private func reload(_ target: String, _ vault: SaveVault) async {
        entries[target] = await Task.detached(priority: .utility) { vault.list(target: target) }.value
    }

    nonisolated private static func runUpload(_ vault: SaveVault, target: String, device: String, reason: String, why: String) -> String {
        do {
            let (r, pruned) = try KLICloudSaves.writing(vault, target) { () -> (SaveVault.Result, [String]) in
                let r = try vault.snapshot(target: target, containerRoot: KLICloudSaves.containerRoot, device: device, reason: reason)
                return (r, vault.prune(target: target, keep: KLICloudSaves.keep))
            }
            let tail = pruned.isEmpty ? "" : "; pruned \(pruned)"
            switch r {
            case .noSaves:
                NSLog("%@", "[icloud] \(target) (\(why), \(reason)): no saves on this headset\(tail)")
                return klL("No saves on this headset yet.", "Aún no hay partidas en este visor.")
            case .unchanged(let s):
                NSLog("%@", "[icloud] \(target) (\(why), \(reason)): unchanged since \(s)\(tail)")
                return klL("Up to date: no changes since the last backup.", "Al día: sin cambios desde la última copia.")
            case .created(let s):
                NSLog("%@", "[icloud] \(target) (\(why), \(reason)): backup \(s) created\(tail)")
                return klL("Backed up ", "Copia subida ") + Date().formatted(date: .omitted, time: .shortened)
            }
        } catch {
            NSLog("%@", "[icloud] \(target) (\(why), \(reason)) backup failed: \(error)")
            return klL("Backup failed: ", "Error al subir la copia: ") + "\(error)"
        }
    }

    nonisolated private static func runRestore(_ vault: SaveVault, target: String, stamp: String, device: String, why: String) -> String {
        do {
            let pruned = try KLICloudSaves.restoring(vault, target, stamp) { () -> [String] in
                try vault.restore(target: target, stamp: stamp, containerRoot: KLICloudSaves.containerRoot, device: device)
                return vault.prune(target: target, keep: KLICloudSaves.keep, protecting: [stamp])
            }
            NSLog("%@", "[icloud] \(target) (\(why)): restored backup \(stamp)" + (pruned.isEmpty ? "" : "; pruned \(pruned)"))
            return klL("Restored the backup of ", "Restaurada la copia del ") + stamp
        } catch {
            NSLog("%@", "[icloud] \(target) (\(why)): restore of \(stamp) failed: \(error)")
            return klL("Restore failed: ", "Error al restaurar: ") + "\(error)"
        }
    }
}

/// The settings sheet's section for one game.
struct KLICloudSaveSection: View {
    let target: String
    @ObservedObject private var model = KLICloudSaveModel.shared
    @State private var enabled: Bool
    /// A Meta game played from Meta (checked once when the sheet opens: it stats the game's files).
    @State private var shown: Bool
    @State private var confirm: SaveVault.Entry?

    init(target: String) {
        self.target = target
        _enabled = State(initialValue: KLICloudSaves.enabled(target))
        _shown = State(initialValue: KLICloudSaves.supported(target) && KLICloudSaves.playsFromMeta(target))
    }

    private var busy: Bool { model.working[target] != nil }

    var body: some View {
        if shown {
            Section {
                switch model.availability {
                case .unknown:
                    HStack { ProgressView().controlSize(.small); Text(klL("Checking iCloud…", "Comprobando iCloud…")).foregroundStyle(.secondary) }
                case .unavailable:
                    Text(klL("iCloud is not available in this build, or no iCloud account is signed in.",
                             "iCloud no disponible en esta compilación o sin sesión de iCloud."))
                        .font(.callout).foregroundStyle(.secondary)
                case .available:
                    available
                }
            } header: {
                Text(klL("iCloud backups", "Copias en iCloud"))
            } footer: {
                if model.availability == .available {
                    Text(klL("Your saves for this game are copied to iCloud Drive when AVP Game Hub opens (only when they "
                             + "changed; the newest \(KLICloudSaves.keep) are kept). If this headset has no saves for it, Play "
                             + "first restores the newest backup. Saves already here are never replaced without asking.",
                             "Las partidas de este juego se copian a iCloud Drive al abrir AVP Game Hub (solo si cambiaron; "
                             + "se guardan las \(KLICloudSaves.keep) más recientes). Si este visor no tiene partidas suyas, Jugar "
                             + "restaura antes la copia más reciente. Las partidas que ya hay aquí nunca se sustituyen sin preguntar."))
                }
            }
            .onAppear { model.refresh(target) }
            .confirmationDialog(klL("Restore this backup?", "¿Restaurar esta copia?"),
                                isPresented: Binding(get: { confirm != nil }, set: { if !$0 { confirm = nil } }),
                                titleVisibility: .visible, presenting: confirm) { e in
                Button(klL("Restore", "Restaurar"), role: .destructive) { model.restore(target, stamp: e.stamp) }
                Button(klL("Cancel", "Cancelar"), role: .cancel) {}
            } message: { e in
                Text(klL("The saves on this headset are first backed up to iCloud (\"Before a restore\"), then the files of the "
                         + "backup from \(e.date.formatted(date: .abbreviated, time: .shortened)) replace them.",
                         "Primero se sube a iCloud una copia de las partidas de este visor (\"Antes de restaurar\"); después los "
                         + "archivos de la copia del \(e.date.formatted(date: .abbreviated, time: .shortened)) las sustituyen."))
            }
        }
    }

    @ViewBuilder private var available: some View {
        Toggle(klL("Back up automatically", "Copia automática"),
               isOn: Binding(get: { enabled }, set: { enabled = $0; model.setEnabled(target, $0) }))
        HStack {
            if let w = model.working[target] {
                ProgressView().controlSize(.small)
                Text(w).font(.callout).foregroundStyle(.secondary)
            } else if let r = model.lastResult[target] {
                Text(r).font(.callout).foregroundStyle(.secondary)
            }
            Spacer()
            Button(klL("Back up now", "Subir ahora")) { model.upload(target, reason: "manual", why: "manual") }
                .disabled(busy)
        }
        let list = model.entries[target] ?? []
        if list.isEmpty {
            Text(klL("No backups in iCloud yet.", "Aún no hay copias en iCloud.")).font(.callout).foregroundStyle(.secondary)
        }
        ForEach(list) { e in
            HStack {
                VStack(alignment: .leading, spacing: 2) {
                    Text(e.date.formatted(date: .abbreviated, time: .shortened))
                    Text(details(e)).font(.caption).foregroundStyle(.secondary).lineLimit(1)
                }
                Spacer()
                if e.isDownloaded {
                    Button(klL("Restore", "Restaurar")) { confirm = e }.disabled(busy)
                } else {
                    Label(klL("Not downloaded", "No descargada"), systemImage: "icloud.and.arrow.down")
                        .font(.caption).foregroundStyle(.secondary)
                    Button(klL("Download", "Descargar")) { model.download(target, stamp: e.stamp) }.disabled(busy)
                }
            }
        }
    }

    private func details(_ e: SaveVault.Entry) -> String {
        // A backup not downloaded yet: only its date (the stamp) is known.
        guard !e.reason.isEmpty || e.fileCount > 0 else { return klL("Details once downloaded", "Detalles al descargarla") }
        var parts: [String] = []
        if let d = e.device, !d.isEmpty { parts.append(d) }
        parts.append(ByteCountFormatter.string(fromByteCount: e.totalBytes, countStyle: .file))
        parts.append(KLICloudSaves.reasonLabel(e.reason))
        return parts.joined(separator: " · ")
    }
}

/// One line for the library card (nil = nothing to say): a run in progress, Play's notice, or
/// how old the newest backup is.
@MainActor
func klICloudSaveCardLine(_ target: String) -> (String, Color)? {
    let m = KLICloudSaveModel.shared
    if let w = m.working[target] { return (w, .blue) }
    if let n = m.notice[target] { return (n, .orange) }
    guard m.eligible.contains(target), let newest = m.entries[target]?.first else { return nil }
    return ("iCloud · " + KLICloudSaves.ago(newest.date), .secondary)
}
#endif
