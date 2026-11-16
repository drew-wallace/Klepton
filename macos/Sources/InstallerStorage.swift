import SwiftUI

struct InstallerStorageEntry: Decodable, Identifiable {
    let id: String
    let path: String
    let category: String
    let name: String
    let detail: String
    let bytes: Int64
    let protected: Bool
    let date: String
}

struct InstallerStorageReport: Decodable {
    let root: String
    let entries: [InstallerStorageEntry]
    let totalBytes: Int64
    let freeBytes: Int64
    let removableBytes: Int64
    let warnings: [String]
    let removedBytes: Int64?
    let removedCount: Int?
}

private func storageBytes(_ bytes: Int64) -> String {
    ByteCountFormatter.string(fromByteCount: bytes, countStyle: .file)
}

struct InstallerStorageView: View {
    @ObservedObject var model: InstallerModel
    @State private var selected: Set<String> = []
    @State private var search = ""
    @State private var category = "All files"
    @State private var confirming = false
    @Environment(\.dismiss) private var dismiss

    private var entries: [InstallerStorageEntry] { model.storageReport?.entries ?? [] }
    private var categories: [String] { Array(Set(entries.map(\.category))).sorted() }
    private var visible: [InstallerStorageEntry] {
        entries.filter {
            (category == "All files" || category == $0.category) &&
            (search.isEmpty || [$0.name, $0.detail, $0.category].joined(separator: " ").localizedCaseInsensitiveContains(search))
        }.sorted { $0.bytes == $1.bytes ? $0.path < $1.path : $0.bytes > $1.bytes }
    }
    private var selection: [InstallerStorageEntry] { entries.filter { selected.contains($0.id) && !$0.protected } }
    private var selectedBytes: Int64 { selection.reduce(0) { $0 + $1.bytes } }

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            HStack {
                VStack(alignment: .leading, spacing: 4) {
                    Text("Storage").font(.title2.bold())
                    Text("Downloads and installer files on this Mac").foregroundStyle(.secondary)
                }
                Spacer()
                Button("Refresh", systemImage: "arrow.clockwise") { model.refreshStorage() }.disabled(model.busy)
                Button("Done") { dismiss() }.keyboardShortcut(.cancelAction).disabled(model.busy)
            }
            if let report = model.storageReport {
                HStack(spacing: 12) {
                    metric("Installer storage", bytes: report.totalBytes, icon: "internaldrive")
                    metric("Available to clean", bytes: report.removableBytes, icon: "trash")
                    metric("Free on disk", bytes: report.freeBytes, icon: "externaldrive")
                }
                HStack {
                    Text(report.root).font(.caption).foregroundStyle(.secondary).lineLimit(1).truncationMode(.middle)
                    Spacer()
                    Button("Show in Finder") { model.reveal(report.root) }.controlSize(.small)
                }
                ForEach(report.warnings, id: \.self) { warning in
                    Label(warning, systemImage: "lock.shield").font(.callout).foregroundStyle(.orange)
                }
                if let count = report.removedCount, let bytes = report.removedBytes {
                    Label("Deleted \(count) \(count == 1 ? "item" : "items") (\(storageBytes(bytes))).", systemImage: "checkmark.circle.fill")
                        .foregroundStyle(.green)
                }
            }
            HStack {
                TextField("Search games, builds or accounts", text: $search).textFieldStyle(.roundedBorder)
                Picker("Show", selection: $category) {
                    Text("All files").tag("All files")
                    ForEach(categories, id: \.self) { Text($0).tag($0) }
                }.frame(width: 240)
            }.disabled(model.busy)
            storageList
            Text("Cleanup deletes local copies. Games installed on Vision Pro and original files you supplied stay in place. Backups, receipts and logs are kept; these account for any remaining storage. File sizes may differ from space recovered on disk.")
                .font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            if !model.error.isEmpty {
                Text(model.error).foregroundStyle(.red).font(.callout).textSelection(.enabled)
            }
            HStack {
                if model.busy {
                    ProgressView().controlSize(.small)
                    Text(model.message).font(.callout).lineLimit(2)
                } else {
                    Text("\(selection.count) selected · \(storageBytes(selectedBytes))").foregroundStyle(.secondary)
                }
                Spacer()
                Button("Delete Selected…", role: .destructive) { confirming = true }
                    .disabled(model.busy || selection.isEmpty)
                    .accessibilityIdentifier("deleteInstallerStorage")
            }
        }
        .padding(24)
        .frame(width: 850, height: 700)
        .interactiveDismissDisabled(model.busy)
        .onChange(of: entries.map(\.id)) { _, ids in selected.formIntersection(ids) }
        .onChange(of: search) { _, _ in selected.formIntersection(visible.map(\.id)) }
        .onChange(of: category) { _, _ in selected.formIntersection(visible.map(\.id)) }
        .alert("Delete \(selection.count) \(selection.count == 1 ? "item" : "items")?", isPresented: $confirming) {
            Button("Cancel", role: .cancel) {}
            Button("Delete", role: .destructive) { model.cleanStorage(selected) }
        } message: {
            Text("Permanently delete \(storageBytes(selectedBytes)) from this Mac:\n\n" +
                 selection.prefix(4).map { "• \($0.name) — \($0.category)" }.joined(separator: "\n") +
                 (selection.count > 4 ? "\n…and \(selection.count - 4) more." : "") +
                 "\n\nDeleted downloads must be downloaded again. Deleted imports must be imported again. The current APK selection will be cleared.")
        }
    }

    private var storageList: some View {
        VStack(spacing: 0) {
            HStack {
                Text("\(visible.count) \(visible.count == 1 ? "item" : "items") · Largest first").font(.caption).foregroundStyle(.secondary)
                Spacer()
                Button("Select Visible") { selected.formUnion(visible.filter { !$0.protected }.map(\.id)) }
                Button("Clear Selection") { selected.removeAll() }
            }.controlSize(.small).padding(10).disabled(model.busy)
            Divider()
            if visible.isEmpty {
                ContentUnavailableView(model.storageReport == nil ? "Measuring storage…" : "No files to show",
                                       systemImage: "internaldrive",
                                       description: Text(entries.isEmpty ? "Downloaded games and imported APKs will appear here." : "Try another search or category."))
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
            } else {
                List(visible) { item in
                    HStack(alignment: .center, spacing: 12) {
                        Toggle(isOn: Binding(get: { selected.contains(item.id) }, set: { value in
                            if value { selected.insert(item.id) } else { selected.remove(item.id) }
                        })) { EmptyView() }
                            .toggleStyle(.checkbox).labelsHidden().disabled(item.protected || model.busy)
                            .accessibilityLabel("Select \(item.name)")
                        VStack(alignment: .leading, spacing: 4) {
                            HStack {
                                Text(item.name).font(.headline).lineLimit(1)
                                if item.protected { Image(systemName: "lock.fill").help("Protected while recovery is pending or receipts are unreadable") }
                            }
                            Text(item.category + " · " + item.detail).font(.caption).foregroundStyle(.secondary).lineLimit(2)
                            Text(storageDate(item.date)).font(.caption).foregroundStyle(.secondary)
                        }
                        Spacer(minLength: 8)
                        Text(storageBytes(item.bytes)).monospacedDigit()
                        Button { model.reveal(item.path) } label: { Image(systemName: "folder") }
                            .buttonStyle(.borderless).help("Show in Finder")
                            .accessibilityLabel("Show \(item.name) in Finder")
                    }.padding(.vertical, 6)
                }.listStyle(.inset)
            }
        }
        .background(.background.secondary)
        .clipShape(RoundedRectangle(cornerRadius: 8))
        .overlay(RoundedRectangle(cornerRadius: 8).stroke(.quaternary))
    }

    private func metric(_ title: String, bytes: Int64, icon: String) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            Label(title, systemImage: icon).font(.caption).foregroundStyle(.secondary)
            Text(storageBytes(bytes)).font(.title2.bold()).monospacedDigit()
        }.frame(maxWidth: .infinity, alignment: .leading).padding(14)
            .background(.quaternary.opacity(0.5), in: RoundedRectangle(cornerRadius: 10))
    }

    private func storageDate(_ value: String) -> String {
        let formatter = ISO8601DateFormatter()
        formatter.formatOptions = [.withInternetDateTime, .withFractionalSeconds]
        let date = formatter.date(from: value) ?? ISO8601DateFormatter().date(from: value)
        return date?.formatted(date: .abbreviated, time: .shortened) ?? "Date unavailable"
    }
}
