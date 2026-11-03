// Force-included into every daemon source file (-include ios9_compat.h).
//
// clock_gettime() only exists since iOS 10, but BTstack's posix run loop uses it.
// Always route it to mach_absolute_time() so the binary runs on iOS 6-9 no matter
// which SDK it was built with (a newer SDK would weak-link the real one -> crash on 9).
#ifndef IOS9_COMPAT_H
#define IOS9_COMPAT_H

#include <time.h>
#include <stdint.h>
#include <mach/mach_time.h>

#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 6
#endif

static inline int a2dpsink_clock_gettime(int clock_id, struct timespec * ts){
    (void) clock_id;
    static mach_timebase_info_data_t timebase;
    if (timebase.denom == 0) mach_timebase_info(&timebase);
    uint64_t ns = mach_absolute_time() * timebase.numer / timebase.denom;
    ts->tv_sec  = (time_t) (ns / 1000000000ull);
    ts->tv_nsec = (long)   (ns % 1000000000ull);
    return 0;
}
#define clock_gettime(clock_id, ts) a2dpsink_clock_gettime((int) (clock_id), (ts))

#endif
