// One real-size control viewport for iPhone, iPad and external-display play.
// GPL-3.0-or-later
import SwiftUI

extension TouchLayoutViewport {
    init(geometry: GeometryProxy, model: LauncherModel) {
        self.init(size: geometry.size,
                  safeTop: geometry.safeAreaInsets.top, safeBottom: geometry.safeAreaInsets.bottom,
                  safeLeading: geometry.safeAreaInsets.leading, safeTrailing: geometry.safeAreaInsets.trailing,
                  portraitAspect: model.aspectRatio.portraitRatio, externalDisplay: model.externalDisplayConnected)
    }
}

struct DeviceTouchControlsView: View {
    @ObservedObject private var model = LauncherModel.shared
    @ObservedObject private var layouts = TouchLayoutStore.shared
    var body: some View {
        GeometryReader { geo in
            let viewport = TouchLayoutViewport(geometry: geo, model: model)
            let rect = viewport.contentRect
            TouchControlSurface(layout: viewport.projected(layouts.configuration[viewport.orientation]),
                                orientation: viewport.orientation)
                .frame(width: rect.width, height: rect.height)
                .offset(x: rect.minX, y: rect.minY)
        }
    }
}
