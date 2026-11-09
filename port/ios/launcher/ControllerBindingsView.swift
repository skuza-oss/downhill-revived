// Shared 0.7.12 controller bindings exposed in the native launcher. GPL-3.0-or-later.
import SwiftUI

struct ControllerBindingsView: View {
    @ObservedObject private var model = LauncherModel.shared
    @State private var player: Int32 = 1
    @State private var layout = [UInt8](repeating: 0, count: 34)
    @State private var errorMessage: String?
    @State private var confirmRestore = false

    var body: some View {
        Form {
            Section {
                Picker("Controller", selection: $player) {
                    Text(L10n.format("Player %lld", Int64(1))).tag(Int32(1))
                    Text(L10n.format("Player %lld", Int64(2))).tag(Int32(2))
                }
                Text("Changes apply immediately. Essential menu buttons stay assigned.")
                    .font(.footnote).foregroundStyle(.secondary)
            }
            Section("Button bindings") {
                ForEach(0..<16, id: \.self) { button in
                    VStack(alignment: .leading, spacing: 4) {
                        Text(L10n.text(String(cString: ios_controller_target_label(Int32(button)))))
                            .font(.headline)
                        bindingPicker(button: button, slot: 0, title: "Main input")
                        bindingPicker(button: button, slot: 1, title: "Alternate input")
                    }
                }
            }
            Section("Analog Sticks") {
                Toggle("Swap Sticks", isOn: Binding(get: { layout[32] != 0 },
                    set: { change(edit: 3, input: $0 ? 1 : 0) }))
                ForEach(0..<4, id: \.self) { axis in
                    Toggle(L10n.text(["Invert left X", "Invert left Y", "Invert right X", "Invert right Y"][axis]),
                        isOn: Binding(get: { layout[33] & UInt8(1 << axis) != 0 },
                            set: { change(edit: 4, target: Int32(1 << axis), input: $0 ? 1 : 0) }))
                }
            }
            Button("Restore controller bindings", role: .destructive) { confirmRestore = true }
        }
        .navigationTitle("Controller button bindings")
        .navigationBarTitleDisplayMode(.inline)
        .onAppear { reload() }
        .onChange(of: player) { _, _ in reload() }
        .confirmationDialog("Restore controller bindings?", isPresented: $confirmRestore, titleVisibility: .visible) {
            Button("Restore controller bindings", role: .destructive) { change(edit: 2) }
            Button("Cancel", role: .cancel) { }
        } message: {
            Text(L10n.format("Only Player %lld will be reset.", Int64(player)))
        }
        .alert("Could not update bindings", isPresented: Binding(
            get: { errorMessage != nil }, set: { if !$0 { errorMessage = nil } }
        )) { Button("OK") { errorMessage = nil } } message: { Text(errorMessage ?? "") }
    }

    private func bindingPicker(button: Int, slot: Int, title: String) -> some View {
        Picker(L10n.text(title), selection: Binding(
            get: { Int32(layout[button * 2 + slot]) },
            set: { change(edit: 0, target: Int32(button), slot: Int32(slot), input: $0) }
        )) {
            Text("None").tag(Int32(0))
            ForEach(0..<Int(ios_controller_input_count()), id: \.self) { index in
                Text(String(cString: ios_controller_input_label(Int32(index), !model.useXboxGlyphs)))
                    .tag(Int32(ios_controller_input_id(Int32(index))))
            }
        }
    }

    private func reload() {
        var values = [UInt8](repeating: 0, count: 34)
        let ok = values.withUnsafeMutableBufferPointer { buffer in
            ios_controller_layout(player, buffer.baseAddress, Int32(buffer.count))
        }
        if ok != 0 { layout = values }
    }

    private func change(edit: Int32, target: Int32 = 0, slot: Int32 = 0, input: Int32 = 0) {
        var error = [CChar](repeating: 0, count: 256)
        let result = error.withUnsafeMutableBufferPointer { buffer in
            ios_controller_edit(player, edit, target, slot, input, buffer.baseAddress, Int32(buffer.count))
        }
        if result < 0 {
            errorMessage = error.withUnsafeBufferPointer { buffer in
                L10n.text(String(cString: buffer.baseAddress!))
            }
        }
        reload()
    }
}
