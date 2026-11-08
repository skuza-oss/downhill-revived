// Persistent, orientation-specific touch layouts. GPL-3.0-or-later.
import Foundation
import CoreGraphics

enum TouchLayoutOrientation: String, Codable, CaseIterable, Identifiable, Hashable {
    case portrait, landscape
    var id: String { rawValue }
    var title: String { self == .portrait ? "Portrait" : "Landscape" }
    // The editor preview shares gameplay's reference dimensions for control scaling.
    var referenceSize: CGSize {
        self == .portrait ? CGSize(width: 390, height: 280) : CGSize(width: 780, height: 300)
    }

    func fit(in size: CGSize) -> CGFloat {
        max(0.1, min(1, min(size.width / referenceSize.width, size.height / referenceSize.height)))
    }
}

enum TouchLayoutCoordinateSpace: String, Codable, Equatable { case legacy, screen }

enum TouchLayoutSlot: String, Codable, CaseIterable, Identifiable, Hashable {
    case stick, cross, circle, square, triangle, l1, r1, l2, r2, start, select, l3, r3, menu
    var id: String { rawValue }
    var isButton: Bool { self != .stick && self != .menu }
    var essential: Bool { [.cross, .triangle, .start, .r1].contains(self) }
    var title: String {
        switch self {
        case .stick: return "Analog Stick"
        case .cross: return "Cross"
        case .circle: return "Circle"
        case .square: return "Square"
        case .triangle: return "Triangle"
        case .start: return "START"
        case .select: return "SELECT"
        case .menu: return "Menu"
        default: return rawValue.uppercased()
        }
    }
}

struct TouchControlPlacement: Codable, Identifiable, Equatable {
    var slot: TouchLayoutSlot
    var x: Double
    var y: Double
    var scale: Double = 1
    var visible: Bool = true
    var action: TouchLayoutSlot
    var id: TouchLayoutSlot { slot }

    func size(fit: CGFloat) -> CGSize {
        let factor = CGFloat(scale) * fit
        switch slot {
        case .stick: return CGSize(width: 130 * factor, height: 130 * factor)
        case .l1, .l2, .r1, .r2: return CGSize(width: 66 * factor, height: 38 * factor)
        case .select, .start, .menu: return CGSize(width: 76 * factor, height: 34 * factor)
        default: return CGSize(width: 50 * factor, height: 50 * factor)
        }
    }
}

struct TouchLayout: Codable, Equatable {
    var controls: [TouchControlPlacement]
    var coordinateSpace: TouchLayoutCoordinateSpace = .screen
    var baseScale: Double = 1

    init(controls: [TouchControlPlacement], coordinateSpace: TouchLayoutCoordinateSpace = .screen, baseScale: Double = 1) {
        self.controls = controls
        self.coordinateSpace = coordinateSpace
        self.baseScale = baseScale
    }

    private enum CodingKeys: String, CodingKey { case controls, coordinateSpace, baseScale }

    init(from decoder: Decoder) throws {
        let values = try decoder.container(keyedBy: CodingKeys.self)
        controls = try values.decode([TouchControlPlacement].self, forKey: .controls)
        coordinateSpace = try values.decodeIfPresent(TouchLayoutCoordinateSpace.self, forKey: .coordinateSpace) ?? .legacy
        baseScale = try values.decodeIfPresent(Double.self, forKey: .baseScale) ?? 1
    }

    static func defaults(_ orientation: TouchLayoutOrientation) -> TouchLayout {
        let portrait = orientation == .portrait
        let positions: [(TouchLayoutSlot, Double, Double)] = portrait ? [
            (.stick, 0.21, 0.73),
            (.cross, 0.78, 0.80), (.circle, 0.89, 0.73), (.square, 0.67, 0.73), (.triangle, 0.78, 0.66),
            (.l2, 0.11, 0.57), (.l1, 0.11, 0.51), (.r1, 0.89, 0.51), (.r2, 0.89, 0.57),
            (.select, 0.37, 0.93), (.start, 0.63, 0.93),
            (.l3, 0.21, 0.88), (.r3, 0.78, 0.88), (.menu, 0.49, 0.51)
        ] : [
            (.stick, 0.14, 0.76),
            (.cross, 0.84, 0.85), (.circle, 0.90, 0.74), (.square, 0.78, 0.74), (.triangle, 0.84, 0.63),
            (.l2, 0.11, 0.52), (.l1, 0.11, 0.38), (.r2, 0.89, 0.52), (.r1, 0.89, 0.38),
            (.select, 0.11, 0.08), (.start, 0.11, 0.20),
            (.l3, 0.26, 0.76), (.r3, 0.72, 0.74), (.menu, 0.89, 0.08)
        ]
        return TouchLayout(controls: positions.map { slot, x, y in
            TouchControlPlacement(slot: slot, x: x, y: y,
                                  visible: slot != .l3 && slot != .r3, action: slot)
        }, coordinateSpace: .screen)
    }

    static func legacyDefaults(_ orientation: TouchLayoutOrientation) -> TouchLayout {
        let portrait = orientation == .portrait
        let positions: [(TouchLayoutSlot, Double, Double)] = portrait ? [
            (.stick, 0.20, 0.62), (.cross, 0.80, 0.80), (.circle, 0.92, 0.62),
            (.square, 0.68, 0.62), (.triangle, 0.80, 0.44),
            (.l2, 0.10, 0.13), (.l1, 0.30, 0.13), (.r1, 0.70, 0.13), (.r2, 0.90, 0.13),
            (.select, 0.50, 0.48), (.start, 0.50, 0.66),
            (.l3, 0.20, 0.92), (.r3, 0.80, 0.92), (.menu, 0.50, 0.13)
        ] : [
            (.stick, 0.12, 0.76), (.cross, 0.88, 0.93), (.circle, 0.95, 0.76),
            (.square, 0.81, 0.76), (.triangle, 0.88, 0.59),
            (.l2, 0.12, 0.23), (.l1, 0.12, 0.40), (.r2, 0.88, 0.23), (.r1, 0.88, 0.40),
            (.select, 0.44, 0.08), (.start, 0.56, 0.08),
            (.l3, 0.30, 0.90), (.r3, 0.70, 0.90), (.menu, 0.50, 0.27)
        ]
        return TouchLayout(controls: positions.map { slot, x, y in
            TouchControlPlacement(slot: slot, x: x, y: y,
                                  visible: slot != .l3 && slot != .r3, action: slot)
        }, coordinateSpace: .legacy)
    }

    mutating func assign(_ action: TouchLayoutSlot, to slot: TouchLayoutSlot) {
        guard slot.isButton, action.isButton,
              let destination = controls.firstIndex(where: { $0.slot == slot }),
              let source = controls.firstIndex(where: { $0.action == action }) else { return }
        let previous = controls[destination].action
        controls[destination].action = action
        controls[source].action = previous
        if action.essential { controls[destination].visible = true }
        if previous.essential { controls[source].visible = true }
    }

    var hasEssentialControls: Bool {
        controls.contains { $0.slot == .menu && $0.visible } &&
        controls.contains { $0.slot == .stick && $0.visible } &&
        TouchLayoutSlot.allCases.filter(\.essential).allSatisfy { action in
            controls.contains { $0.action == action && $0.visible }
        }
    }

    func sanitized(_ orientation: TouchLayoutOrientation) -> TouchLayout {
        var result = coordinateSpace == .legacy ? Self.legacyDefaults(orientation) : Self.defaults(orientation)
        result.baseScale = baseScale.isFinite ? min(2, max(0.1, baseScale)) : 1
        for index in result.controls.indices {
            let fallback = result.controls[index]
            guard let saved = controls.first(where: { $0.slot == fallback.slot }) else { continue }
            result.controls[index] = TouchControlPlacement(
                slot: fallback.slot,
                x: saved.x.isFinite ? min(1, max(0, saved.x)) : fallback.x,
                y: saved.y.isFinite ? min(1, max(0, saved.y)) : fallback.y,
                scale: saved.scale.isFinite ? min(1.5, max(0.6, saved.scale)) : 1,
                visible: saved.visible, action: fallback.slot.isButton && saved.action.isButton ? saved.action : fallback.action
            )
        }
        let actions = result.controls.filter { $0.slot.isButton }.map(\.action)
        if Set(actions).count != actions.count {
            for index in result.controls.indices { result.controls[index].action = result.controls[index].slot }
        }
        for index in result.controls.indices {
            let control = result.controls[index]
            if control.action.essential || control.slot == .menu || control.slot == .stick {
                result.controls[index].visible = true
            }
        }
        return result
    }
}

struct TouchLayoutConfiguration: Codable, Equatable {
    var version = 2
    var portrait = TouchLayout.defaults(.portrait)
    var landscape = TouchLayout.defaults(.landscape)

    subscript(_ orientation: TouchLayoutOrientation) -> TouchLayout {
        get { orientation == .portrait ? portrait : landscape }
        set {
            if orientation == .portrait { portrait = newValue }
            else { landscape = newValue }
        }
    }
}

// Session-only history. Each orientation owns one instance, so undo never
// restores coordinates from the other orientation or a pre-migration canvas.
struct TouchLayoutEditHistory {
    private(set) var undoLayouts: [TouchLayout] = []
    private(set) var redoLayouts: [TouchLayout] = []
    private var startingLayout: TouchLayout?
    var hasTransaction: Bool { startingLayout != nil }
    var canUndo: Bool { !undoLayouts.isEmpty || hasTransaction }
    var canRedo: Bool { !redoLayouts.isEmpty && !hasTransaction }

    mutating func begin(_ current: TouchLayout) {
        if startingLayout == nil { startingLayout = current }
    }

    mutating func commit(_ current: TouchLayout) {
        guard let previous = startingLayout else { return }
        startingLayout = nil
        record(from: previous, to: current)
    }

    mutating func record(from previous: TouchLayout, to current: TouchLayout) {
        guard previous != current else { return }
        undoLayouts.append(previous)
        if undoLayouts.count > 40 { undoLayouts.removeFirst(undoLayouts.count - 40) }
        redoLayouts.removeAll()
    }

    mutating func undo(_ current: TouchLayout) -> TouchLayout? {
        commit(current)
        guard let previous = undoLayouts.popLast() else { return nil }
        redoLayouts.append(current)
        return previous
    }

    mutating func redo(_ current: TouchLayout) -> TouchLayout? {
        commit(current)
        guard let next = redoLayouts.popLast() else { return nil }
        undoLayouts.append(current)
        return next
    }
}

// Geometry is measured in the window's safe content area. The system excludes
// the notch, Dynamic Island and home indicator before this shared 16-point inset.
// The editor and game use this same viewport; floating tools never resize it.
struct TouchLayoutViewport: Equatable {
    var size: CGSize
    var safeTop: CGFloat = 0
    var safeBottom: CGFloat = 0
    var safeLeading: CGFloat = 0
    var safeTrailing: CGFloat = 0
    var portraitAspect: CGFloat = 4.0 / 3.0
    var externalDisplay = false

    var orientation: TouchLayoutOrientation { size.height > size.width ? .portrait : .landscape }

    var contentRect: CGRect {
        CGRect(x: 16, y: 16, width: max(1, size.width - 32), height: max(1, size.height - 32))
    }

    var portraitScreenSize: CGSize {
        let aspect = portraitAspect.isFinite && portraitAspect > 0 ? portraitAspect : 4.0 / 3.0
        let width = max(1, min(size.width - 20, 500))
        let height = min(width / aspect, size.height * 0.44)
        return CGSize(width: height * aspect, height: height)
    }

    // The pre-fullscreen layouts used a padded deck in portrait and a separately
    // padded overlay in landscape. Keep their visible positions when importing.
    var legacyRect: CGRect {
        if orientation == .portrait {
            let deckTop = externalDisplay ? size.height * 0.5 : portraitScreenSize.height + 36
            return CGRect(x: 12, y: deckTop + 10, width: max(1, size.width - 24),
                          height: max(1, size.height - deckTop - 20))
        }
        let left = max(16, safeLeading), right = max(16, safeTrailing)
        let top = max(44, safeTop + 16), bottom = max(16, safeBottom)
        return CGRect(x: left, y: top, width: max(1, size.width - left - right),
                      height: max(1, size.height - top - bottom))
    }

    func projected(_ layout: TouchLayout) -> TouchLayout {
        guard layout.coordinateSpace == .legacy else { return layout }
        let old = legacyRect, current = contentRect
        let oldFit = orientation.fit(in: old.size), newFit = orientation.fit(in: current.size)
        var result = layout
        result.coordinateSpace = .screen
        result.baseScale *= Double(oldFit / newFit)
        for index in result.controls.indices {
            let control = layout.controls[index]
            let bounds = control.size(fit: oldFit * CGFloat(layout.baseScale))
            let x = min(old.width - bounds.width / 2, max(bounds.width / 2, CGFloat(control.x) * old.width))
            let y = min(old.height - bounds.height / 2, max(bounds.height / 2, CGFloat(control.y) * old.height))
            result.controls[index].x = Double(min(1, max(0, (old.minX + x - current.minX) / current.width)))
            result.controls[index].y = Double(min(1, max(0, (old.minY + y - current.minY) / current.height)))
        }
        return result
    }
}
