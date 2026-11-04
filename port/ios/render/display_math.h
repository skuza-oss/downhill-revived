/* Presentation bounds shared by the iOS bridge and portable checks. GPL-3.0-or-later. */
#ifndef DHD_IOS_DISPLAY_MATH_H
#define DHD_IOS_DISPLAY_MATH_H
#include <math.h>
#include <stdbool.h>
static inline void ios_display_limit(double width, double height, int output_limit,
                                      bool external, double *w, double *h) {
    *w = *h = 0;
    if (!isfinite(width) || !isfinite(height) || width <= 0 || height <= 0) return;
    if (external) {
        height = fmin(height, width * 9 / 16);
        width = height * 16 / 9;
    }
    double ratio = 1;
    if (output_limit > 0)
        ratio = fmin(1, output_limit / fmin(width, height));
    *w = fmax(1, floor(width * ratio));
    *h = fmax(1, floor(height * ratio));
}
/* A 120 Hz TV is useful only when the active HDMI route exposes that rate to UIKit. */
static inline int ios_display_target_fps(int requested, int maximum, bool external, bool restricted) {
    return maximum >= 120 && (external || requested >= 120) && !restricted ? 120 : 60;
}
#endif
