// UIKit input updates the native pad before SwiftUI redraws or haptics run.
// GPL-3.0-or-later
import SwiftUI
import UIKit

struct ImmediateTouchInput: UIViewRepresentable {
    var onDown: (CGPoint, CGSize) -> Void
    var onMove: (CGPoint, CGPoint, CGSize) -> Void = { _, _, _ in }
    var onUp: () -> Void

    func makeUIView(context: Context) -> ImmediateTouchView {
        let view = ImmediateTouchView()
        view.backgroundColor = .clear
        return view
    }
    func updateUIView(_ view: ImmediateTouchView, context: Context) {
        view.onDown = onDown
        view.onMove = onMove
        view.onUp = onUp
    }
    static func dismantleUIView(_ view: ImmediateTouchView, coordinator: ()) { view.releaseInput() }
}

final class ImmediateTouchView: UIView {
    var onDown: (CGPoint, CGSize) -> Void = { _, _ in }
    var onMove: (CGPoint, CGPoint, CGSize) -> Void = { _, _, _ in }
    var onUp: () -> Void = {}
    private var finger: UITouch?
    private var origin = CGPoint.zero
    override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent?) {
        guard finger == nil, let touch = touches.first else { return }
        finger = touch
        origin = touch.location(in: self)
        onDown(origin, bounds.size)
    }
    override func touchesMoved(_ touches: Set<UITouch>, with event: UIEvent?) {
        guard let finger = finger, touches.contains(finger) else { return }
        onMove(finger.location(in: self), origin, bounds.size)
    }
    override func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent?) {
        if let finger = finger, touches.contains(finger) { releaseInput() }
    }
    override func touchesCancelled(_ touches: Set<UITouch>, with event: UIEvent?) { releaseInput() }
    override func didMoveToWindow() {
        super.didMoveToWindow()
        if window == nil { releaseInput() }
    }
    func releaseInput() {
        guard finger != nil else { return }
        finger = nil
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
