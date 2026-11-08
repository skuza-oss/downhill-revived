// GPL-3.0-or-later
import SwiftUI

struct ControlsCustomizationView: View {
    @ObservedObject private var model = LauncherModel.shared
    @Environment(\.dismiss) private var dismiss
    @State private var showTouchEditor = false

    var body: some View {
        NavigationStack {
            Form {
                Section("Touchscreen Controls") {
                    Button("Arrange and remap touch controls") {
                        model.beginControlsEditing()
                        showTouchEditor = true
                    }
                    Text("Drag controls, choose their actions and save a layout for each orientation.")
                        .font(.footnote).foregroundStyle(.secondary)
                }
                Section("Physical Controllers") {
                    NavigationLink("Controller button bindings") { ControllerBindingsView() }
                }
            }
            .navigationTitle("Customize Controls")
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("Done") { dismiss() } }
            }
        }
        .fullScreenCover(isPresented: $showTouchEditor, onDismiss: { model.endControlsEditing() }) {
            TouchLayoutEditorView()
        }
        .preferredColorScheme(.dark)
        .interactiveDismissDisabled()
        .onAppear { model.beginControlsEditing() }
        .onDisappear { model.endControlsEditing() }
    }
}

private struct TouchLayoutEditorView: View {
    @ObservedObject private var model = LauncherModel.shared
    @ObservedObject private var store = TouchLayoutStore.shared
    @Environment(\.dismiss) private var dismiss
    @Environment(\.scenePhase) private var scenePhase
    @State private var draft = TouchLayoutStore.shared.configuration
    @State private var baseline = TouchLayoutStore.shared.configuration
    @State private var orientation = TouchLayoutOrientation.portrait
    @State private var prepared = Set<TouchLayoutOrientation>()
    @State private var selected = TouchLayoutSlot.cross
    @State private var isDragging = false
    @State private var showOptions = false
    @State private var errorMessage: String?
    @State private var confirmDiscard = false
    @State private var confirmRestore = false
    @State private var loaded = false
    @State private var histories: [TouchLayoutOrientation: TouchLayoutEditHistory] = [:]
    @State private var toolsExpanded = true

    private var hasChanges: Bool { draft != baseline }
    private var selectedIndex: Int? { draft[orientation].controls.firstIndex { $0.slot == selected } }
    private var toolsAtBottom: Bool {
        guard let index = selectedIndex else { return false }
        return draft[orientation].controls[index].y < 0.5
    }

    var body: some View {
        GeometryReader { geo in
            let viewport = TouchLayoutViewport(geometry: geo, model: model)
            let rect = viewport.contentRect
            ZStack(alignment: .topLeading) {
                Color.black.ignoresSafeArea()
                TouchControlSurface(layout: viewport.projected(draft[viewport.orientation]),
                                    orientation: viewport.orientation, editing: true, selected: selected,
                                    onSelect: { select($0) },
                                    onMove: { move($0, $1, $2, orientation: viewport.orientation) },
                                    onDragStateChange: { dragStateChanged($0, orientation: viewport.orientation) })
                    .frame(width: rect.width, height: rect.height)
                    .coordinateSpace(name: "touch-layout")
                    .offset(x: rect.minX, y: rect.minY)
                    .id("\(viewport.size.width):\(viewport.size.height)")

                if !isDragging {
                    VStack {
                        if toolsAtBottom { Spacer(minLength: 0) }
                        toolbox(viewport: viewport)
                        if !toolsAtBottom { Spacer(minLength: 0) }
                    }
                    .padding(12)
                    .frame(width: geo.size.width, height: geo.size.height)
                }
            }
            .frame(width: geo.size.width, height: geo.size.height)
            .onAppear {
                if !loaded {
                    draft = store.configuration
                    baseline = draft
                    prepared.removeAll()
                    loaded = true
                }
                prepare(viewport)
            }
            .onChange(of: viewport) { _, next in
                finishEdit(orientation)
                showOptions = false
                isDragging = false
                prepare(next)
            }
        }
        .statusBarHidden()
        .preferredColorScheme(.dark)
        .interactiveDismissDisabled()
        .onChange(of: scenePhase) { _, phase in
            if phase != .active {
                finishEdit(orientation)
                isDragging = false
                showOptions = false
            }
        }
        .confirmationDialog("Discard unsaved changes?", isPresented: $confirmDiscard, titleVisibility: .visible) {
            Button("Discard Changes", role: .destructive) { dismiss() }
            Button("Keep Editing", role: .cancel) { }
        }
        .confirmationDialog("Restore this layout?", isPresented: $confirmRestore, titleVisibility: .visible) {
            Button("Restore this layout", role: .destructive) { edit { $0 = .defaults(orientation) } }
            Button("Cancel", role: .cancel) { }
        } message: {
            Text("The other orientation will keep its current layout.")
        }
        .alert("Could not save controls", isPresented: Binding(
            get: { errorMessage != nil }, set: { if !$0 { errorMessage = nil } }
        )) { Button("OK") { errorMessage = nil } } message: { Text(errorMessage ?? "") }
    }

    private func toolbox(viewport: TouchLayoutViewport) -> some View {
        VStack(spacing: 8) {
            HStack(spacing: 8) {
                Button("Cancel") {
                    if hasChanges { confirmDiscard = true }
                    else { dismiss() }
                }
                .frame(minHeight: 44)
                Spacer(minLength: 0)
                Menu {
                    Picker("Control", selection: Binding(get: { selected }, set: { select($0) })) {
                        ForEach(TouchLayoutSlot.allCases) { slot in Text(L10n.text(slot.title)).tag(slot) }
                    }
                    Button("Control options") { showOptions = true }
                    Button("Undo") { undo() }.disabled(!(histories[orientation]?.canUndo ?? false))
                    Button("Redo") { redo() }.disabled(!(histories[orientation]?.canRedo ?? false))
                    Button("Restore this layout", role: .destructive) { confirmRestore = true }
                } label: {
                    Image(systemName: "slider.horizontal.3").frame(width: 44, height: 44)
                }
                .accessibilityLabel(L10n.text("Control options"))
                .popover(isPresented: $showOptions) {
                    controlOptions
                        .frame(width: min(360, max(260, viewport.size.width - 32)),
                               height: min(460, max(160, viewport.size.height - 40)))
                        .presentationCompactAdaptation(.popover)
                }
                Button { toolsExpanded.toggle() } label: {
                    Image(systemName: toolsExpanded ? "chevron.up" : "chevron.down")
                        .frame(width: 44, height: 44)
                }
                .accessibilityLabel(L10n.text(toolsExpanded ? "Collapse tools" : "Expand tools"))
                Spacer(minLength: 0)
                Button("Done") {
                    finishEdit(orientation)
                    do { try store.save(draft); dismiss() }
                    catch { errorMessage = error.localizedDescription }
                }
                .fontWeight(.bold).frame(minHeight: 44)
            }
            if toolsExpanded, let index = selectedIndex {
                let control = draft[orientation].controls[index]
                HStack {
                    VStack(alignment: .leading, spacing: 2) {
                        Text(L10n.text(control.action.title)).fontWeight(.semibold)
                        Text(L10n.text(orientation.title)).foregroundStyle(.secondary)
                    }
                    if !control.visible { Text("Hidden").foregroundStyle(.secondary) }
                    Spacer()
                    Button { undo() } label: { Image(systemName: "arrow.uturn.backward").frame(width: 44, height: 44) }
                        .accessibilityLabel(L10n.text("Undo"))
                        .disabled(!(histories[orientation]?.canUndo ?? false))
                    Button { redo() } label: { Image(systemName: "arrow.uturn.forward").frame(width: 44, height: 44) }
                        .accessibilityLabel(L10n.text("Redo"))
                        .disabled(!(histories[orientation]?.canRedo ?? false))
                }
                .font(.caption)
                adjustment("Size", value: valueBinding(index, \.scale), range: 0.6...1.5)
                Text("Drag to move. Rotate the device to edit the other orientation.")
                    .font(.caption2).foregroundStyle(.secondary)
                    .multilineTextAlignment(.center)
            }
        }
        .padding(.horizontal, 12)
        .padding(.bottom, 12)
        .frame(maxWidth: 420)
        .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 16))
        .frame(maxWidth: .infinity)
    }

    private var controlOptions: some View {
        Form {
            Picker("Control", selection: Binding(get: { selected }, set: { select($0) })) {
                ForEach(TouchLayoutSlot.allCases) { slot in Text(L10n.text(slot.title)).tag(slot) }
            }
            if let index = selectedIndex {
                let control = draft[orientation].controls[index]
                if selected.isButton {
                    Picker("Action", selection: Binding(
                        get: { draft[orientation].controls[index].action },
                        set: { action in edit { $0.assign(action, to: selected) } }
                    )) {
                        ForEach(TouchLayoutSlot.allCases.filter(\.isButton)) { action in
                            Text(L10n.text(action.title)).tag(action)
                        }
                    }
                }
                Toggle("Visible", isOn: Binding(
                    get: { draft[orientation].controls[index].visible },
                    set: { visible in edit { $0.controls[index].visible = visible } }
                ))
                .disabled(!selected.isButton || control.action.essential)
                adjustment("Horizontal position", value: valueBinding(index, \.x))
                adjustment("Vertical position", value: valueBinding(index, \.y))
            }
            Button("Back") { showOptions = false }
        }
        .preferredColorScheme(.dark)
    }

    private func adjustment(_ title: String, value: Binding<Double>, range: ClosedRange<Double> = 0...1) -> some View {
        let mode = orientation
        return VStack(alignment: .leading) {
            HStack {
                Text(L10n.text(title))
                Spacer()
                Text(value.wrappedValue, format: .percent.precision(.fractionLength(0))).monospacedDigit()
            }
            .font(.caption)
            Slider(value: value, in: range, step: 0.01, onEditingChanged: { active in
                if active && mode == orientation { beginEdit(mode) }
                else if !active { finishEdit(mode) }
            })
            .accessibilityLabel(L10n.text(title))
        }
    }

    private func prepare(_ viewport: TouchLayoutViewport) {
        orientation = viewport.orientation
        guard !prepared.contains(orientation) else { return }
        draft[orientation] = viewport.projected(draft[orientation])
        baseline[orientation] = draft[orientation]
        prepared.insert(orientation)
    }

    private func move(_ slot: TouchLayoutSlot, _ x: Double, _ y: Double, orientation: TouchLayoutOrientation) {
        guard orientation == self.orientation,
              let index = draft[orientation].controls.firstIndex(where: { $0.slot == slot }) else { return }
        beginEdit(orientation)
        draft[orientation].controls[index].x = x
        draft[orientation].controls[index].y = y
    }

    private func select(_ slot: TouchLayoutSlot) {
        finishEdit(orientation)
        selected = slot
        toolsExpanded = true
    }

    private func dragStateChanged(_ active: Bool, orientation: TouchLayoutOrientation) {
        if !active { finishEdit(orientation) }
        if orientation == self.orientation { isDragging = active }
    }

    private func valueBinding(_ index: Int, _ path: WritableKeyPath<TouchControlPlacement, Double>) -> Binding<Double> {
        let mode = orientation
        return Binding(get: { draft[mode].controls[index][keyPath: path] }, set: { value in
            guard mode == orientation else { return }
            beginEdit(mode)
            draft[mode].controls[index][keyPath: path] = value
        })
    }

    private func beginEdit(_ orientation: TouchLayoutOrientation) {
        var history = histories[orientation, default: TouchLayoutEditHistory()]
        history.begin(draft[orientation])
        histories[orientation] = history
    }

    private func finishEdit(_ orientation: TouchLayoutOrientation) {
        guard var history = histories[orientation] else { return }
        history.commit(draft[orientation])
        histories[orientation] = history
    }

    private func edit(_ change: (inout TouchLayout) -> Void) {
        finishEdit(orientation)
        let previous = draft[orientation]
        var current = previous
        change(&current)
        var history = histories[orientation, default: TouchLayoutEditHistory()]
        history.record(from: previous, to: current)
        histories[orientation] = history
        draft[orientation] = current
    }

    private func undo() {
        var history = histories[orientation, default: TouchLayoutEditHistory()]
        if let previous = history.undo(draft[orientation]) { draft[orientation] = previous }
        histories[orientation] = history
    }

    private func redo() {
        var history = histories[orientation, default: TouchLayoutEditHistory()]
        if let next = history.redo(draft[orientation]) { draft[orientation] = next }
        histories[orientation] = history
    }
}
