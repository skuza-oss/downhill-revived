// One control surface for gameplay and the layout editor. GPL-3.0-or-later.
import SwiftUI

extension TouchLayoutSlot {
    var inputID: TouchControlID {
        switch self {
        case .cross: return TOUCH_CTRL_CROSS
        case .circle: return TOUCH_CTRL_CIRCLE
        case .square: return TOUCH_CTRL_SQUARE
        case .triangle: return TOUCH_CTRL_TRIANGLE
        case .l1: return TOUCH_CTRL_L1
        case .r1: return TOUCH_CTRL_R1
        case .l2: return TOUCH_CTRL_L2
        case .r2: return TOUCH_CTRL_R2
        case .start: return TOUCH_CTRL_START
        case .select: return TOUCH_CTRL_SELECT
        case .l3: return TOUCH_CTRL_L3
        case .r3: return TOUCH_CTRL_R3
        default: return TOUCH_CTRL_STICK
        }
    }

    func glyph(xbox: Bool) -> String {
        switch self {
        case .cross: return xbox ? "A" : "✕"
        case .circle: return xbox ? "B" : "○"
        case .square: return xbox ? "X" : "□"
        case .triangle: return xbox ? "Y" : "△"
        case .l1: return xbox ? "LB" : "L1"
        case .r1: return xbox ? "RB" : "R1"
        case .l2: return xbox ? "LT" : "L2"
        case .r2: return xbox ? "RT" : "R2"
        default: return L10n.text(title)
        }
    }

    var color: Color {
        switch self {
        case .cross: return .blue
        case .circle: return .red
        case .square: return .pink
        case .triangle: return .green
        default: return .white
        }
    }
}

struct TouchControlSurface: View {
    @ObservedObject private var model = LauncherModel.shared
    let layout: TouchLayout
    let orientation: TouchLayoutOrientation
    var editing = false
    var selected: TouchLayoutSlot?
    var onSelect: (TouchLayoutSlot) -> Void = { _ in }
    var onMove: (TouchLayoutSlot, Double, Double) -> Void = { _, _, _ in }
    var onDragStateChange: (Bool) -> Void = { _ in }
    @Environment(\.scenePhase) private var scenePhase
    @GestureState private var dragStart: (slot: TouchLayoutSlot, point: CGPoint)? = nil
    @State private var inputGeneration = 0
    @State private var draggingSlot: TouchLayoutSlot?

    init(layout: TouchLayout, orientation: TouchLayoutOrientation, editing: Bool = false,
         selected: TouchLayoutSlot? = nil, onSelect: @escaping (TouchLayoutSlot) -> Void = { _ in },
         onMove: @escaping (TouchLayoutSlot, Double, Double) -> Void = { _, _, _ in },
         onDragStateChange: @escaping (Bool) -> Void = { _ in }) {
        self.layout = layout
        self.orientation = orientation
        self.editing = editing
        self.selected = selected
        self.onSelect = onSelect
        self.onMove = onMove
        self.onDragStateChange = onDragStateChange
    }

    var body: some View {
        GeometryReader { geo in
            let fit = orientation.fit(in: geo.size) * CGFloat(layout.baseScale)
            ZStack {
                ForEach(layout.controls) { placement in
                    if placement.visible || (editing && selected == placement.slot) {
                        let size = placement.size(fit: fit)
                        let position = controlPosition(placement, size: size, canvas: geo.size)
                        control(placement, size: size, canvas: geo.size)
                            .frame(width: size.width, height: size.height)
                            .position(position)
                            .zIndex(editing && selected == placement.slot ? 1 : 0)
                    }
                }
            }
            .frame(width: geo.size.width, height: geo.size.height, alignment: .topLeading)
            .onChange(of: geo.size) { _, _ in resetInput() }
        }
        .onDisappear {
            touch_input_reset()
            TouchGlidingRouter.shared.reset()
            if editing { onDragStateChange(false) }
        }
        .onChange(of: layout) { _, _ in resetInput() }
        .onChange(of: orientation) { _, _ in resetInput() }
        .onChange(of: model.showInGameMenu) { _, shown in if shown { resetInput() } }
        .onChange(of: model.controlsEditorActive) { _, active in if active { resetInput() } }
        .onChange(of: model.shouldShowTouchControls) { _, shown in if !shown { resetInput() } }
        .onChange(of: scenePhase) { _, phase in if phase != .active { resetInput() } }
        .onChange(of: dragStart != nil) { _, active in
            if !active { draggingSlot = nil }
            onDragStateChange(active)
        }
    }

    private func resetInput() {
        touch_input_reset()
        TouchGlidingRouter.shared.reset()
        // Recreate UIKit touch owners too: a finger held across a menu, rotation
        // or background transition must not resume sending moves after the reset.
        if !editing { inputGeneration += 1 }
    }

    private func controlPosition(_ placement: TouchControlPlacement, size: CGSize, canvas: CGSize) -> CGPoint {
        CGPoint(x: min(canvas.width - size.width / 2, max(size.width / 2, CGFloat(placement.x) * canvas.width)),
                y: min(canvas.height - size.height / 2, max(size.height / 2, CGFloat(placement.y) * canvas.height)))
    }

    @ViewBuilder
    private func control(_ placement: TouchControlPlacement, size: CGSize, canvas: CGSize) -> some View {
        if editing {
            TouchControlArtwork(placement: placement, xbox: model.useXboxGlyphs)
                .opacity(placement.visible ? 1 : 0.3)
                .overlay {
                    if selected == placement.slot {
                        RoundedRectangle(cornerRadius: 10).stroke(Color.orange, lineWidth: 3)
                    }
                }
                .contentShape(Rectangle())
                // Selecting or remapping must not rewrite a position clamped at
                // the canvas edge. Only an actual drag changes the saved coordinates.
                .onTapGesture { onSelect(placement.slot) }
                .gesture(DragGesture(minimumDistance: 4, coordinateSpace: .named("touch-layout"))
                    .updating($dragStart) { _, state, _ in
                        if state?.slot != placement.slot {
                            state = (placement.slot, controlPosition(placement, size: size, canvas: canvas))
                        }
                    }
                    .onChanged { value in
                        if draggingSlot != placement.slot {
                            draggingSlot = placement.slot
                            onSelect(placement.slot)
                        }
                        let origin = dragStart?.point ?? controlPosition(placement, size: size, canvas: canvas)
                        let x = min(canvas.width - size.width / 2, max(size.width / 2, origin.x + value.translation.width))
                        let y = min(canvas.height - size.height / 2, max(size.height / 2, origin.y + value.translation.height))
                        onMove(placement.slot, Double(x / max(1, canvas.width)), Double(y / max(1, canvas.height)))
                    }
                    .onEnded { _ in
                        draggingSlot = nil
                        onDragStateChange(false)
                    })
                .accessibilityLabel(L10n.text(placement.slot.title))
        } else if placement.slot == .menu {
            Button { model.showInGameMenu = true } label: {
                TouchControlArtwork(placement: placement, xbox: model.useXboxGlyphs)
            }
            .buttonStyle(.plain)
            .opacity(model.menuButtonVisible ? 1 : 0)
            .allowsHitTesting(model.menuButtonVisible && !model.showInGameMenu && !model.controlsEditorActive)
            .accessibilityLabel(L10n.text("Menu"))
        } else {
            TouchControlInput(placement: placement, xbox: model.useXboxGlyphs,
                              floating: model.floatingStick, haptics: model.hapticsEnabled)
                .id("\(placement.action.rawValue):\(inputGeneration)")
                .opacity(model.shouldShowTouchControls ? model.touchOpacity : 0)
                .allowsHitTesting(model.shouldShowTouchControls && !model.showInGameMenu && !model.controlsEditorActive)
        }
    }
}

struct TouchControlArtwork: View {
    let placement: TouchControlPlacement
    let xbox: Bool
    var pressed = false
    var offset = CGSize.zero

    var body: some View {
        GeometryReader { geo in
            if placement.slot == .stick {
                Circle().fill(Color.black.opacity(0.45))
                    .overlay(Circle().stroke(Color.white.opacity(0.4), lineWidth: 2))
                    .overlay {
                        Circle().fill(Color.white.opacity(pressed ? 0.8 : 0.5))
                            .frame(width: geo.size.width * 0.46, height: geo.size.height * 0.46)
                            .offset(offset)
                    }
            } else {
                let color = placement.action.color
                let round = [.cross, .circle, .square, .triangle, .l3, .r3].contains(placement.slot)
                RoundedRectangle(cornerRadius: round ? geo.size.width / 2 : 10)
                    .fill(pressed ? color.opacity(0.4) : Color.black.opacity(0.6))
                    .overlay(RoundedRectangle(cornerRadius: round ? geo.size.width / 2 : 10)
                        .stroke(color.opacity(0.85), lineWidth: 2))
                    .overlay {
                        Text(placement.action.glyph(xbox: xbox))
                            .font(.system(size: min(20, geo.size.height * 0.40), weight: .black))
                            .foregroundStyle(color)
                            .lineLimit(1).minimumScaleFactor(0.45)
                    }
            }
        }
    }
}

private struct TouchControlInput: View {
    let placement: TouchControlPlacement
    let xbox: Bool
    let floating: Bool
    let haptics: Bool
    @State private var pressed = false
    @State private var offset = CGSize.zero

    init(placement: TouchControlPlacement, xbox: Bool, floating: Bool, haptics: Bool) {
        self.placement = placement
        self.xbox = xbox
        self.floating = floating
        self.haptics = haptics
    }

    var body: some View {
        TouchControlArtwork(placement: placement, xbox: xbox, pressed: pressed, offset: offset)
            .contentShape(Rectangle())
            .overlay(ImmediateTouchInput(
                controlID: placement.slot == .stick ? nil : placement.action.inputID,
                haptics: haptics,
                onDown: { point, size in
                    pressed = true
                    if placement.slot == .stick {
                        updateStick(point, origin: floating ? point : CGPoint(x: size.width / 2, y: size.height / 2), size: size)
                    }
                },
                onMove: { point, origin, size in
                    if placement.slot == .stick {
                        updateStick(point, origin: floating ? origin : CGPoint(x: size.width / 2, y: size.height / 2), size: size)
                    }
                },
                onUp: {
                    if placement.slot == .stick { touch_input_set_stick(0, 0) }
                    pressed = false
                    offset = .zero
                },
                onVisualPressed: { isPressed in
                    pressed = isPressed
                }
            ))
            .accessibilityLabel(L10n.text(placement.action.title))
    }

    private func updateStick(_ point: CGPoint, origin: CGPoint, size: CGSize) {
        let radius = max(1, min(size.width, size.height) * 0.35)
        let dx = point.x - origin.x, dy = point.y - origin.y
        touch_input_set_stick(Float(dx / radius), Float(dy / radius))
        let distance = hypot(dx, dy)
        let gain = distance > radius ? radius / distance : 1
        offset = CGSize(width: dx * gain, height: dy * gain)
    }
}
