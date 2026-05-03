#include "time.h"

#include <errno.h>
#include <time.h>

uint64_t smid_mono_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

void smid_sleep_until_us(uint64_t target_us) {
    for (;;) {
        uint64_t now_us = smid_mono_us();
        if (now_us >= target_us) {
            return;
        }
        uint64_t delta_us = target_us - now_us;
        struct timespec ts = {
            .tv_sec = (time_t)(delta_us / 1000000u),
            .tv_nsec = (long)((delta_us % 1000000u) * 1000u),
        };
        while (nanosleep(&ts, &ts) < 0) {
            if (errno != EINTR) {
                return;
            }
        }
    }
}

