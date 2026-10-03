#ifdef _WIN32

#include "platform_time.h"
#include <windows.h>

uint64_t xeno_time_ns(void) {
    static LARGE_INTEGER freq;
    static int initialized = 0;

    if (!initialized) {
        QueryPerformanceFrequency(&freq);
        initialized = 1;
    }

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    return (uint64_t)((now.QuadPart * 1000000000ull) / freq.QuadPart);
}

#endif