#ifndef SMID_TIME_H
#define SMID_TIME_H

#include <stdint.h>

uint64_t smid_mono_us(void);
void smid_sleep_until_us(uint64_t target_us);

#endif

