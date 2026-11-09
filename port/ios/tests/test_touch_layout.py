"""Execute the production Swift layout and persistence code on macOS.

Only localization and the native input reset are substituted. SwiftUI gestures
and view sizing still require an Xcode/device check.
"""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
SWIFTC = shutil.which('swiftc')

HARNESS = r'''
import Foundation

enum L10n { static func text(_ key: String) -> String { key } }
var resets = 0
func touch_input_reset() { resets += 1 }

@main
struct LayoutChecks {
    static func placement(_ layout: TouchLayout, _ slot: TouchLayoutSlot) -> TouchControlPlacement {
        layout.controls.first { $0.slot == slot }!
    }

    static func main() throws {
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: directory) }

        switch CommandLine.arguments[1] {
        case "history":
            let original = TouchLayout.defaults(.landscape)
            var history = TouchLayoutEditHistory()
            var moved = original
            history.begin(original)
            for step in 1...12 {
                moved.controls[0].x = Double(step) / 20
                history.begin(moved) // Repeated move callbacks keep the first snapshot.
            }
            history.commit(moved)
            assert(history.undoLayouts.count == 1)
            assert(history.undo(moved) == original)
            assert(history.redo(original) == moved)
            assert(history.undo(moved) == original)
            var assigned = original
            assigned.assign(.circle, to: .square)
            history.record(from: original, to: assigned)
            assert(!history.canRedo)
            assert(history.undo(assigned) == original)
            history.begin(original)
            history.commit(original)
            assert(history.undoLayouts.isEmpty && history.canRedo)
            assert(history.redo(original) == assigned)
            history.begin(assigned)
            var resized = assigned
            resized.controls[0].scale = 1.4
            assert(history.undo(resized) == assigned)
            assert(history.redo(assigned) == resized)
            var bounded = TouchLayoutEditHistory()
            var current = original
            for step in 1...60 {
                var next = current
                next.controls[0].x = Double(step) / 100
                bounded.record(from: current, to: next)
                current = next
            }
            assert(bounded.undoLayouts.count == 40)
            for _ in 0..<40 { current = bounded.undo(current)! }
            assert(bounded.undo(current) == nil)
            for _ in 0..<40 { current = bounded.redo(current)! }
            assert(current.controls[0].x == 0.6 && bounded.redo(current) == nil)
            let portrait = TouchLayout.defaults(.portrait)
            var independent = TouchLayoutEditHistory()
            var changedPortrait = portrait
            changedPortrait.controls[0].y = 0.75
            independent.record(from: portrait, to: changedPortrait)
            assert(independent.undo(changedPortrait) == portrait)
            assert(bounded.undoLayouts.count == 40)
        case "viewport":
            // Window sizes in points: compact/large phones and several iPad sizes.
            for size in [CGSize(width: 320, height: 568), CGSize(width: 440, height: 956),
                         CGSize(width: 768, height: 1024), CGSize(width: 834, height: 1194),
                         CGSize(width: 1024, height: 1366)] {
                for dimensions in [size, CGSize(width: size.height, height: size.width)] {
                    let viewport = TouchLayoutViewport(size: dimensions)
                    assert(viewport.contentRect.minX == 16 && viewport.contentRect.minY == 16)
                    assert(viewport.contentRect.maxX == dimensions.width - 16)
                    assert(viewport.contentRect.maxY == dimensions.height - 16)
                    let current = TouchLayout.defaults(viewport.orientation)
                    assert(viewport.projected(current) == current)
                    let legacy = TouchLayout.legacyDefaults(viewport.orientation)
                    let mapped = viewport.projected(legacy)
                    assert(mapped.coordinateSpace == .screen)
                    assert(viewport.projected(mapped) == mapped)
                    assert(mapped.hasEssentialControls)
                    for control in mapped.controls {
                        assert((0...1).contains(control.x) && (0...1).contains(control.y))
                        let oldSize = placement(legacy, control.slot).size(fit: viewport.orientation.fit(in: viewport.legacyRect.size))
                        let newSize = control.size(fit: viewport.orientation.fit(in: viewport.contentRect.size) * CGFloat(mapped.baseScale))
                        assert(abs(oldSize.width - newSize.width) < 0.000001)
                        assert(abs(oldSize.height - newSize.height) < 0.000001)
                    }
                }
            }
            let portrait = TouchLayoutViewport(size: CGSize(width: 440, height: 860), portraitAspect: 16.0/9.0)
            let stick = placement(portrait.projected(.legacyDefaults(.portrait)), .stick)
            assert(abs(portrait.contentRect.minX + CGFloat(stick.x) * portrait.contentRect.width - 95.2) < 0.000001)
            assert(abs(portrait.contentRect.minY + CGFloat(stick.y) * portrait.contentRect.height - 634.255) < 0.000001)
            // Import the old JSON without the new coordinate-space fields.
            var old = TouchLayoutConfiguration()
            old.version = 1
            old.portrait = .legacyDefaults(.portrait)
            old.landscape = .legacyDefaults(.landscape)
            old.portrait.controls[0].x = 0.27
            var object = try JSONSerialization.jsonObject(with: JSONEncoder().encode(old)) as! [String: Any]
            for key in ["portrait", "landscape"] {
                var layout = object[key] as! [String: Any]
                layout.removeValue(forKey: "coordinateSpace")
                layout.removeValue(forKey: "baseScale")
                object[key] = layout
            }
            let oldURL = directory.appendingPathComponent("touch_layouts_v1.json")
            try JSONSerialization.data(withJSONObject: object).write(to: oldURL)
            let store = TouchLayoutStore(directory: directory)
            assert(store.configuration.version == 2)
            assert(store.configuration.portrait.coordinateSpace == .legacy)
            assert(store.configuration.portrait.controls[0].x == 0.27)
            var adopted = store.configuration
            adopted.portrait = portrait.projected(adopted.portrait)
            try store.save(adopted)
            assert(FileManager.default.fileExists(atPath: oldURL.path))
            assert(TouchLayoutStore(directory: directory).configuration == adopted)
        case "swap":
            for orientation in TouchLayoutOrientation.allCases {
                var layout = TouchLayout.defaults(orientation)
                let original = layout
                assert(!placement(layout, .l3).visible)
                layout.assign(.cross, to: .l3)
                assert(placement(layout, .l3).action == .cross)
                assert(placement(layout, .l3).visible && layout.hasEssentialControls)
                assert(placement(layout, .cross).action == .l3)
                layout.assign(.triangle, to: .cross)
                assert(layout.hasEssentialControls)
                let before = layout
                layout.assign(.stick, to: .cross)
                layout.assign(.cross, to: .menu)
                assert(before == layout)
                assert(Set(layout.controls.map(\.action)).count == layout.controls.count)
                for control in layout.controls {
                    let saved = placement(original, control.slot)
                    assert(control.x == saved.x && control.y == saved.y && control.scale == saved.scale)
                }
            }
        case "sanitize":
            var layout = TouchLayout.defaults(.portrait)
            for index in layout.controls.indices {
                layout.controls[index].x = .nan
                layout.controls[index].y = .infinity
                layout.controls[index].scale = -100
                layout.controls[index].visible = false
                layout.controls[index].action = .cross
            }
            let checked = layout.sanitized(.portrait)
            assert(checked.hasEssentialControls)
            assert(Set(checked.controls.map(\.action)).count == checked.controls.count)
            for control in checked.controls {
                assert(control.x.isFinite && (0...1).contains(control.x))
                assert(control.y.isFinite && (0...1).contains(control.y))
                assert(control.scale == 0.6)
            }
        case "persistence":
            let store = TouchLayoutStore(directory: directory)
            assert(store.configuration == TouchLayoutConfiguration())
            var draft = store.configuration
            draft.portrait.assign(.cross, to: .l3)
            draft.portrait.controls[0].x = 0.37
            draft.landscape.controls[0].x = 0.22
            draft.landscape.controls[1].y = 0.93
            try store.save(draft)
            assert(resets == 1)
            assert(TouchLayoutStore(directory: directory).configuration == draft)
            let reopened = TouchLayoutStore(directory: directory)
            let originalPositions = reopened.configuration
            var remapped = originalPositions
            remapped.portrait.assign(.circle, to: .square)
            remapped.landscape.assign(.triangle, to: .cross)
            for orientation in TouchLayoutOrientation.allCases {
                for control in remapped[orientation].controls {
                    let saved = placement(originalPositions[orientation], control.slot)
                    assert(control.x == saved.x && control.y == saved.y && control.scale == saved.scale)
                }
            }
            try reopened.save(remapped)
            assert(TouchLayoutStore(directory: directory).configuration == remapped)
            // Restore the first snapshot for the failed-write assertions below.
            try store.save(draft)
            let savedResets = resets
            assert(savedResets == 3)
            let saved = store.configuration
            var invalid = saved
            invalid.portrait.controls[0].visible = false
            do {
                try store.save(invalid)
                fatalError("An inaccessible layout must be refused")
            } catch TouchLayoutStore.LayoutError.missingControls { }
            assert(resets == savedResets && store.configuration == saved)
            assert(TouchLayoutStore(directory: directory).configuration == saved)
            let blocked = directory.appendingPathComponent("not-a-directory")
            try Data([1]).write(to: blocked)
            let unwritable = TouchLayoutStore(directory: blocked)
            let original = unwritable.configuration
            do {
                try unwritable.save(draft)
                fatalError("A failed write must throw")
            } catch { }
            assert(unwritable.configuration == original && resets == savedResets)
        case "migration":
            let legacy: [(Double, Double, Double, Bool)] = [
                (0.14,0.72,1,true),(0.86,0.78,1.1,true),(0.94,0.64,1,true),(0.78,0.64,1,true),(0.86,0.50,1,true),
                (0.12,0.32,1,true),(0.88,0.32,1,true),(0.12,0.16,1,true),(0.88,0.16,1,true),
                (0.60,0.10,0.9,true),(0.40,0.10,0.9,true),(0,0,0,false),(0,0,0,false)
            ]
            var controls: [[String: Any]] = legacy.enumerated().map { id, value in
                ["id": id, "x": value.0, "y": value.1, "scale": value.2, "visible": value.3]
            }
            let url = directory.appendingPathComponent("touch_controls_layout.json")
            func writeLegacy() throws {
                try JSONSerialization.data(withJSONObject: ["controls": controls]).write(to: url)
            }
            try writeLegacy()
            assert(TouchLayoutStore(directory: directory).configuration == TouchLayoutConfiguration())
            controls[2]["scale"] = 1.3
            try writeLegacy()
            var migrated = TouchLayoutStore(directory: directory)
            assert(placement(migrated.configuration.landscape, .circle).scale == 1.3)
            assert(placement(migrated.configuration.portrait, .circle).scale == 1.3)
            controls[2]["scale"] = 1.0
            controls[3]["visible"] = false
            try writeLegacy()
            migrated = TouchLayoutStore(directory: directory)
            assert(!placement(migrated.configuration.landscape, .square).visible)
            // After Save, the new file remains authoritative over the old snapshot.
            var latest = migrated.configuration
            latest.landscape.controls[0].x = 0.39
            try migrated.save(latest)
            controls[3]["visible"] = true
            try writeLegacy()
            assert(TouchLayoutStore(directory: directory).configuration == latest)
        default: fatalError("Unknown check")
        }
    }
}
'''


@unittest.skipUnless(sys.platform == 'darwin' and SWIFTC, 'requires macOS Swift and Combine')
class TouchLayoutChecks(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.directory.cleanup)
        source = Path(cls.directory.name) / 'checks.swift'
        source.write_text(HARNESS)
        cls.binary = Path(cls.directory.name) / 'checks'
        launcher = ROOT / 'port/ios/launcher'
        subprocess.run([SWIFTC, '-swift-version', '5',
                        str(launcher / 'TouchLayout.swift'), str(launcher / 'TouchLayoutStore.swift'),
                        str(source), '-o', str(cls.binary)], check=True, timeout=120)

    def run_check(self, name):
        subprocess.run([str(self.binary), name], check=True, timeout=20)

    def test_essential_action_moved_to_hidden_slot_remains_accessible(self):
        self.run_check('swap')

    def test_fullscreen_viewports_and_v1_migration_preserve_control_geometry(self):
        self.run_check('viewport')

    def test_editor_history_coalesces_gestures_bounds_memory_and_preserves_redo_rules(self):
        self.run_check('history')

    def test_corrupt_values_and_duplicate_actions_are_repaired(self):
        self.run_check('sanitize')

    def test_save_round_trip_and_failed_writes_preserve_active_layout(self):
        self.run_check('persistence')

    def test_legacy_size_and_visibility_edits_migrate_without_overriding_new_file(self):
        self.run_check('migration')
