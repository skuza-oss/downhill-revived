// UIKit input updates the native pad before SwiftUI redraws or haptics run.
// GPL-3.0-or-later
import SwiftUI
import UIKit

// Shared multi-touch coordinator: allows a finger to roll/drag between action buttons
// without lifting, activating the new button instantly while releasing the previous one.
final class TouchGlidingRouter {
    static let shared = TouchGlidingRouter()

    private final class Target {
        let id: TouchControlID
        weak var view: UIView?
        var haptics: Bool
        var setPressed: (Bool) -> Void
        init(id: TouchControlID, view: UIView, haptics: Bool, setPressed: @escaping (Bool) -> Void) {
            self.id = id
            self.view = view
            self.haptics = haptics
            self.setPressed = setPressed
        }
    }

    private var targets: [Target] = []
    private var activeTouches: [ObjectIdentifier: Set<TouchControlID>] = [:]

    func register(id: TouchControlID, view: UIView, haptics: Bool, setPressed: @escaping (Bool) -> Void) {
        unregister(view: view)
        targets.append(Target(id: id, view: view, haptics: haptics, setPressed: setPressed))
    }

    func unregister(view: UIView) {
        targets.removeAll { $0.view == nil || $0.view === view }
    }

    func touchBegan(_ touch: UITouch, on view: UIView, id: TouchControlID) {
        let touchId = ObjectIdentifier(touch)
        activeTouches[touchId] = [id]
        touch_input_set_button(id, true)
        if let target = targets.first(where: { $0.view === view }) {
            target.setPressed(true)
            if target.haptics {
                UIImpactFeedbackGenerator(style: .light).impactOccurred()
            }
        }
    }

    func touchMoved(_ touch: UITouch, from view: UIView) {
        guard let window = view.window else { return }
        let touchId = ObjectIdentifier(touch)
        guard var currentActive = activeTouches[touchId] else { return }
        let locInWindow = touch.location(in: window)

        for target in targets {
            guard let targetView = target.view, targetView.window === window else { continue }
            let locInTarget = targetView.convert(locInWindow, from: window)
            // Enter hit-box: button bounds expanded by 12 points for responsive rolling
            let enterBounds = targetView.bounds.insetBy(dx: -12, dy: -12)
            let isOver = enterBounds.contains(locInTarget)

            if isOver {
                if !currentActive.contains(target.id) {
                    currentActive.insert(target.id)
                    touch_input_set_button(target.id, true)
                    target.setPressed(true)
                    if target.haptics {
                        UIImpactFeedbackGenerator(style: .light).impactOccurred()
                    }
                }
            } else {
                // Exit hit-box: slightly larger hysteresis margin (18 points) to prevent jitter
                let exitBounds = targetView.bounds.insetBy(dx: -18, dy: -18)
                if !exitBounds.contains(locInTarget) && currentActive.contains(target.id) {
                    currentActive.remove(target.id)
                    let isHeldByAnother = activeTouches.contains { otherId, buttons in
                        otherId != touchId && buttons.contains(target.id)
                    }
                    if !isHeldByAnother {
                        touch_input_set_button(target.id, false)
                        target.setPressed(false)
                    }
                }
            }
        }
        activeTouches[touchId] = currentActive
    }

    func touchEnded(_ touch: UITouch) {
        let touchId = ObjectIdentifier(touch)
        guard let heldButtons = activeTouches.removeValue(forKey: touchId) else { return }
        for buttonId in heldButtons {
            let isHeldByAnother = activeTouches.contains { $0.value.contains(buttonId) }
            if !isHeldByAnother {
                touch_input_set_button(buttonId, false)
                for target in targets where target.id == buttonId {
                    target.setPressed(false)
                }
            }
        }
    }

    func reset() {
        activeTouches.removeAll()
        for target in targets {
            target.setPressed(false)
        }
    }
}

struct ImmediateTouchInput: UIViewRepresentable {
    var controlID: TouchControlID? = nil
    var haptics: Bool = false
    var onDown: (CGPoint, CGSize) -> Void
    var onMove: (CGPoint, CGPoint, CGSize) -> Void = { _, _, _ in }
    var onUp: () -> Void
    var onVisualPressed: ((Bool) -> Void)? = nil

    func makeUIView(context: Context) -> ImmediateTouchView {
        let view = ImmediateTouchView()
        view.backgroundColor = .clear
        view.controlID = controlID
        view.haptics = haptics
        view.onDown = onDown
        view.onMove = onMove
        view.onUp = onUp
        view.onVisualPressed = onVisualPressed
        return view
    }

    func updateUIView(_ view: ImmediateTouchView, context: Context) {
        view.controlID = controlID
        view.haptics = haptics
        view.onDown = onDown
        view.onMove = onMove
        view.onUp = onUp
        view.onVisualPressed = onVisualPressed
        if let cid = controlID, view.window != nil {
            TouchGlidingRouter.shared.register(id: cid, view: view, haptics: haptics) { [weak view] pressed in
                view?.onVisualPressed?(pressed)
            }
        }
    }

    static func dismantleUIView(_ view: ImmediateTouchView, coordinator: ()) {
        view.releaseInput()
        TouchGlidingRouter.shared.unregister(view: view)
    }
}

final class ImmediateTouchView: UIView {
    var controlID: TouchControlID?
    var haptics: Bool = false
    var onDown: (CGPoint, CGSize) -> Void = { _, _ in }
    var onMove: (CGPoint, CGPoint, CGSize) -> Void = { _, _, _ in }
    var onUp: () -> Void = {}
    var onVisualPressed: ((Bool) -> Void)?
    private var finger: UITouch?
    private var origin = CGPoint.zero

    override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent?) {
        guard finger == nil, let touch = touches.first else { return }
        finger = touch
        origin = touch.location(in: self)
        onDown(origin, bounds.size)
        if let cid = controlID {
            TouchGlidingRouter.shared.touchBegan(touch, on: self, id: cid)
        }
    }

    override func touchesMoved(_ touches: Set<UITouch>, with event: UIEvent?) {
        guard let finger = finger, touches.contains(finger) else { return }
        onMove(finger.location(in: self), origin, bounds.size)
        if controlID != nil {
            TouchGlidingRouter.shared.touchMoved(finger, from: self)
        }
    }

    override func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent?) {
        if let finger = finger, touches.contains(finger) { releaseInput() }
    }

    override func touchesCancelled(_ touches: Set<UITouch>, with event: UIEvent?) { releaseInput() }

    override func didMoveToWindow() {
        super.didMoveToWindow()
        if window == nil {
            releaseInput()
            TouchGlidingRouter.shared.unregister(view: self)
        } else if let cid = controlID {
            TouchGlidingRouter.shared.register(id: cid, view: self, haptics: haptics) { [weak self] pressed in
                self?.onVisualPressed?(pressed)
            }
        }
    }

    func releaseInput() {
        guard let touch = finger else { return }
        finger = nil
        if controlID != nil {
            TouchGlidingRouter.shared.touchEnded(touch)
        }
        onUp()
    }
}

// Observe every phone touch, including the black screen and the first reveal tap.
// This recognizer never recognizes, prevents, cancels or delays another control.
private final class TouchActivityRecognizer: UIGestureRecognizer {
    var activity: (Bool, Int) -> Void = { _, _ in }
    private var fingers = Set<UITouch>()
    override func canPrevent(_ preventedGestureRecognizer: UIGestureRecognizer) -> Bool { false }
    override func canBePrevented(by preventingGestureRecognizer: UIGestureRecognizer) -> Bool { false }
    override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent) {
        fingers.formUnion(touches)
        activity(true, fingers.count)
    }
    override func touchesMoved(_ touches: Set<UITouch>, with event: UIEvent) { activity(true, fingers.count) }
    override func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent) {
        fingers.subtract(touches)
        activity(false, fingers.count)
        if fingers.isEmpty { state = .failed }
    }
    override func touchesCancelled(_ touches: Set<UITouch>, with event: UIEvent) {
        fingers.subtract(touches)
        activity(false, fingers.count)
        if fingers.isEmpty { state = .failed }
    }
    override func reset() { fingers.removeAll(); super.reset() }
}

struct PhoneTouchActivityObserver: UIViewRepresentable {
    func makeUIView(context: Context) -> PhoneTouchObserverView { PhoneTouchObserverView() }
    func updateUIView(_ view: PhoneTouchObserverView, context: Context) {}
    static func dismantleUIView(_ view: PhoneTouchObserverView, coordinator: ()) { view.detach() }
}
final class PhoneTouchObserverView: UIView {
    private let observer = TouchActivityRecognizer()
    private weak var observedWindow: UIWindow?
    override init(frame: CGRect) {
        super.init(frame: frame)
        isUserInteractionEnabled = false
        observer.cancelsTouchesInView = false
        observer.delaysTouchesBegan = false
        observer.delaysTouchesEnded = false
        observer.activity = { began, count in
            LauncherModel.shared.phoneTouchActivity(began: began, count: count)
        }
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) has not been implemented") }
    override func didMoveToWindow() {
        super.didMoveToWindow()
        detach()
        observedWindow = window
        window?.addGestureRecognizer(observer)
    }
    func detach() {
        observedWindow?.removeGestureRecognizer(observer)
        observedWindow = nil
    }
}
