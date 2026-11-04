/* Presented-frame intervals, shared by the renderer and the iOS launcher.
   GPL-3.0-or-later. These are submission intervals, not GPU execution times. */
#ifndef DHD_IOS_PERFORMANCE_STATS_H
#define DHD_IOS_PERFORMANCE_STATS_H
void ios_perf_record_present(double seconds);
void ios_perf_reset(void);
void ios_perf_snapshot(double *fps, double *p99_ms);
#endif
