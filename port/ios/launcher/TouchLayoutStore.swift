// GPL-3.0-or-later
import Foundation
import Combine

final class TouchLayoutStore: ObservableObject {
    static let shared = TouchLayoutStore()
    @Published private(set) var configuration = TouchLayoutConfiguration()
    private let fileURL: URL

    init(directory: URL = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]) {
        fileURL = directory.appendingPathComponent("touch_layouts_v2.json")
        let previousURL = directory.appendingPathComponent("touch_layouts_v1.json")
        let isDefaultDocDir = (directory == FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0])
        let defaults = UserDefaults.standard
        if isDefaultDocDir && defaults.integer(forKey: "DHD_TOUCH_LAYOUT_REVISION") < 2 {
            configuration = TouchLayoutConfiguration()
            try? save(configuration)
            defaults.set(2, forKey: "DHD_TOUCH_LAYOUT_REVISION")
        } else if var saved = Self.read(fileURL, version: 2) ?? Self.read(previousURL, version: 1) {
            saved.version = 2
            saved.portrait = saved.portrait.sanitized(.portrait)
            saved.landscape = saved.landscape.sanitized(.landscape)
            configuration = saved
        } else {
            migrateLegacyLayout(directory.appendingPathComponent("touch_controls_layout.json"))
        }
    }

    private static func read(_ url: URL, version: Int) -> TouchLayoutConfiguration? {
        guard let data = try? Data(contentsOf: url),
              let saved = try? JSONDecoder().decode(TouchLayoutConfiguration.self, from: data),
              saved.version == version else { return nil }
        return saved
    }

    func save(_ draft: TouchLayoutConfiguration) throws {
        guard draft.portrait.hasEssentialControls && draft.landscape.hasEssentialControls else {
            throw LayoutError.missingControls
        }
        var checked = draft
        checked.version = 2
        checked.portrait = draft.portrait.sanitized(.portrait)
        checked.landscape = draft.landscape.sanitized(.landscape)
        let data = try JSONEncoder().encode(checked)
        try data.write(to: fileURL, options: .atomic)
        touch_input_reset()
        configuration = checked
    }

    private func migrateLegacyLayout(_ url: URL) {
        guard let data = try? Data(contentsOf: url),
              let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let controls = object["controls"] as? [[String: Any]] else { return }
        // Old launchers saved the native defaults on every preference change. Only
        // migrate a genuinely edited layout, so those snapshots keep the familiar UI.
        let legacyDefaults: [(x: Double, y: Double, scale: Double, visible: Bool)] = [
            (0.14,0.72,1,true),(0.86,0.78,1.1,true),(0.94,0.64,1,true),(0.78,0.64,1,true),(0.86,0.50,1,true),
            (0.12,0.32,1,true),(0.88,0.32,1,true),(0.12,0.16,1,true),(0.88,0.16,1,true),
            (0.60,0.10,0.9,true),(0.40,0.10,0.9,true),(0,0,0,false),(0,0,0,false)
        ]
        let legacySlots: [TouchLayoutSlot] = [.stick,.cross,.circle,.square,.triangle,.l1,.r1,.l2,.r2,.start,.select,.l3,.r3]
        let edited = controls.contains { control in
            guard let id = control["id"] as? Int, legacyDefaults.indices.contains(id),
                  let x = control["x"] as? Double, let y = control["y"] as? Double else { return false }
            let original = legacyDefaults[id]
            let visible = control["visible"] as? Bool ?? original.visible
            let scale = control["scale"] as? Double ?? original.scale
            return abs(x - original.x) > 0.001 || abs(y - original.y) > 0.001 ||
                visible != original.visible || ((visible || original.visible) && abs(scale - original.scale) > 0.001)
        }
        guard edited else { return }
        for orientation in TouchLayoutOrientation.allCases {
            var layout = TouchLayout.legacyDefaults(orientation)
            for control in controls {
                guard let id = control["id"] as? Int, legacySlots.indices.contains(id),
                      let index = layout.controls.firstIndex(where: { $0.slot == legacySlots[id] }),
                      let x = control["x"] as? Double, let y = control["y"] as? Double else { continue }
                layout.controls[index].x = x
                layout.controls[index].y = y
                layout.controls[index].scale = control["scale"] as? Double ?? 1
                layout.controls[index].visible = control["visible"] as? Bool ?? true
            }
            configuration[orientation] = layout.sanitized(orientation)
        }
    }

    enum LayoutError: LocalizedError {
        case missingControls
        var errorDescription: String? {
            L10n.text("Keep the stick, Menu, Cross, Triangle, START and R1 visible.")
        }
    }
}
