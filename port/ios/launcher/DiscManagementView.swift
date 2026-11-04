// DiscManagementView.swift - ISO Selection, Validation & Diagnostics for iOS
// GPL-3.0-or-later
import SwiftUI
import UniformTypeIdentifiers

struct DiscManagementView: View {
    @ObservedObject var model = LauncherModel.shared
    @Environment(\.dismiss) private var dismiss
    @State private var showingFilePicker = false

    private var supportedContentTypes: [UTType] {
        var types: [UTType] = [.data, .item, .diskImage]
        if let iso = UTType(filenameExtension: "iso") { types.append(iso) }
        if let bin = UTType(filenameExtension: "bin") { types.append(bin) }
        if let img = UTType(filenameExtension: "img") { types.append(img) }
        return types
    }

    var body: some View {
        GeometryReader { geo in
            let isPortrait = geo.size.height > geo.size.width

            ZStack {
                Color(red: 0.07, green: 0.08, blue: 0.10)
                    .ignoresSafeArea()

                ScrollView(showsIndicators: false) {
                    if isPortrait {
                        VStack(spacing: 16) {
                            actionStatusCard
                            infoRequirementsCard
                        }
                        .padding(.horizontal, 18)
                        .padding(.vertical, 12)
                    } else {
                        HStack(alignment: .top, spacing: 20) {
                            infoRequirementsCard
                            actionStatusCard
                                .frame(maxWidth: geo.size.width * 0.44)
                        }
                        .padding(.horizontal, 20)
                        .padding(.vertical, 12)
                    }
                }
            }
        }
        .navigationTitle("Disc Management")
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .navigationBarTrailing) {
                Button("Scan / Refresh") {
                    model.refreshStorageAndDisc()
                }
                .font(.system(size: 13, weight: .semibold))
                .foregroundColor(.orange)
            }
        }
        .fileImporter(isPresented: $showingFilePicker, allowedContentTypes: supportedContentTypes, allowsMultipleSelection: false) { result in
            switch result {
            case .success(let urls):
                if let url = urls.first {
                    model.importISO(from: url)
                }
            case .failure(let error):
                model.importError = error.localizedDescription
            }
        }
    }

    private var infoRequirementsCard: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack(spacing: 8) {
                Image(systemName: "info.circle.fill")
                    .foregroundColor(.blue)
                Text("Game Disc Requirement")
                    .font(.system(size: 15, weight: .bold))
                    .foregroundColor(.white)
            }

            Text("Use your PlayStation 2 disc image of Downhill Domination USA (SCUS-97177). The game loads its assets directly from that image.")
                .font(.system(size: 12))
                .foregroundColor(.white.opacity(0.75))
                .lineSpacing(2)

            Divider().background(Color.white.opacity(0.12))

            // Storage Status
            HStack {
                VStack(alignment: .leading, spacing: 2) {
                    Text("SANDBOX STORAGE")
                        .font(.system(size: 10, weight: .bold))
                        .foregroundColor(.white.opacity(0.5))
                    Text(L10n.format("%llu GB free on device", model.availableStorageMB / 1024))
                        .font(.system(size: 13, weight: .semibold))
                        .foregroundColor(.white)
                }
                Spacer()
                Image(systemName: "internaldrive")
                    .foregroundColor(.white.opacity(0.4))
            }
            .padding(10)
            .background(Color.white.opacity(0.04))
            .cornerRadius(10)

            Divider().background(Color.white.opacity(0.12))

            // Direct Files App Guide
            VStack(alignment: .leading, spacing: 6) {
                HStack(spacing: 6) {
                    Image(systemName: "folder.badge.gearshape")
                        .foregroundColor(.cyan)
                    Text("Manual Copy via Files App:")
                        .font(.system(size: 12, weight: .bold))
                        .foregroundColor(.white.opacity(0.85))
                }
                Text("In Files, copy the ISO to On My iPhone > Downhill Revived. Then tap Scan / Refresh.")
                    .font(.system(size: 11))
                    .foregroundColor(.white.opacity(0.65))
                    .lineSpacing(2)
            }
            .padding(10)
            .background(Color.white.opacity(0.03))
            .cornerRadius(10)
        }
        .padding(14)
        .background(Color.white.opacity(0.04))
        .cornerRadius(14)
    }

    private var actionStatusCard: some View {
        VStack(spacing: 14) {
            ZStack {
                Circle()
                    .fill(model.discReady ? Color.green.opacity(0.15) : Color.orange.opacity(0.15))
                    .frame(width: 68, height: 68)
                Image(systemName: model.discReady ? "checkmark.seal.fill" : "opticaldisc")
                    .font(.system(size: 32))
                    .foregroundColor(model.discReady ? .green : .orange)
            }

            Text(L10n.text(model.discReady ? "Disc Ready to Play" : "No ISO Installed"))
                .font(.system(size: 16, weight: .bold))
                .foregroundColor(.white)

            Text(model.discStatusMessage)
                .font(.system(size: 12))
                .foregroundColor(.white.opacity(0.65))
                .multilineTextAlignment(.center)
                .lineLimit(2)

            if let err = model.importError {
                HStack(spacing: 8) {
                    Image(systemName: "exclamationmark.triangle.fill")
                        .foregroundColor(.orange)
                        .font(.system(size: 14))
                    Text(err)
                        .font(.system(size: 11, weight: .medium))
                        .foregroundColor(.orange.opacity(0.95))
                        .lineLimit(3)
                        .minimumScaleFactor(0.8)
                }
                .padding(10)
                .background(Color.orange.opacity(0.12))
                .cornerRadius(10)
                .overlay(RoundedRectangle(cornerRadius: 10).stroke(Color.orange.opacity(0.3), lineWidth: 1))
            }

            if model.isImporting {
                VStack(spacing: 6) {
                    ProgressView(value: model.importProgress, total: 1.0)
                        .progressViewStyle(LinearProgressViewStyle(tint: .blue))
                    Text(model.importStatusText)
                        .font(.system(size: 11))
                        .foregroundColor(.white.opacity(0.8))
                }
                .padding(.horizontal)
            } else {
                Button(action: {
                    showingFilePicker = true
                }) {
                    HStack(spacing: 8) {
                        Image(systemName: "square.and.arrow.down.fill")
                        Text(L10n.text(model.discReady ? "Select Different ISO" : "Select ISO from Files"))
                    }
                    .font(.system(size: 14, weight: .bold))
                    .frame(maxWidth: .infinity)
                    .frame(height: 46)
                    .background(Color.blue)
                    .foregroundColor(.white)
                    .cornerRadius(12)
                }

                if model.discReady {
                    Button(action: {
                        model.deleteActiveDisc()
                    }) {
                        HStack(spacing: 6) {
                            Image(systemName: "trash")
                            Text("Remove Disc from Sandbox")
                        }
                        .font(.system(size: 11, weight: .semibold))
                        .foregroundColor(.red.opacity(0.8))
                    }
                    .padding(.top, 2)
                }
            }

            // Quick 3-Step Guide
            VStack(alignment: .leading, spacing: 6) {
                Text("Quick Steps:")
                    .font(.system(size: 11, weight: .bold))
                    .foregroundColor(.white.opacity(0.85))
                stepRow(num: "1", text: "Have SCUS-97177.iso in Files, iCloud, or Downloads")
                stepRow(num: "2", text: "Tap 'Select ISO from Files' and choose the file")
                stepRow(num: "3", text: "After automatic verification, tap Play Game.")
            }
            .padding(10)
            .background(Color.black.opacity(0.2))
            .cornerRadius(10)
        }
        .padding(16)
        .frame(maxWidth: .infinity)
        .background(Color.white.opacity(0.05))
        .cornerRadius(14)
    }

    private func stepRow(num: String, text: String) -> some View {
        HStack(spacing: 6) {
            Text(num)
                .font(.system(size: 10, weight: .bold))
                .foregroundColor(.blue)
                .frame(width: 16, height: 16)
                .background(Color.blue.opacity(0.2))
                .clipShape(Circle())
            Text(L10n.text(text))
                .font(.system(size: 11))
                .foregroundColor(.white.opacity(0.7))
        }
    }
}
