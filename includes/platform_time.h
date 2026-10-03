#ifndef PLATFORM_TIME_H
#define PLATFORM_TIME_H

#include <stdint.h>

#ifdef _WIN32

// Windows: returns nanoseconds via 64-bit integer
uint64_t xeno_time_ns(void);

#else

#include <time.h>

// Linux: fills timespec (native precision)
static inline void xeno_time_now(struct timespec *ts) {
    clock_gettime(CLOCK_MONOTONIC, ts);
}

static inline uint64_t xeno_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

#endif

#endif /* PLATFORM_TIME_H */