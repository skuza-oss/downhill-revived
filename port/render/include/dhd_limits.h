/* Renderer limits that the native launchers also show, written once here (finding 235).
   GPL-3.0-or-later. */
#ifndef DHD_LIMITS_H
#define DHD_LIMITS_H
/* Phones stop at 6x: on a Snapdragon 888 (Mi 11i) 0.7.7 draws the bench race at 93 fields/s at 4x
   and 33 at 8x, so 7x and 8x only make a phone slow and hot. iPhones and iPads too (finding 224).
   The renderer clamps to it (gpu_rust_shared.inc); the phone launchers' snapshot.scale_max says it
   (Android through Native.phoneScaleMax, iOS launcher_calls.c). */
#define NATIVE_PHONE_SCALE_MAX 6u
#endif
