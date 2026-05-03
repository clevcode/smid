#ifndef SMID_COMMON_H
#define SMID_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SMID_ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

enum {
    SMID_USB_VID = 0x090c,
    SMID_USB_PID = 0x0760,
    SMID_USB_IFACE = 0,
    SMID_USB_ALTSETTING = 1,
    SMID_EP_BULK_OUT = 0x02,
    SMID_EP_INTR_OUT = 0x01,
    SMID_EP_INTR_IN = 0x81,
};

static inline uint64_t smid_min_u64(uint64_t a, uint64_t b) {
    return a < b ? a : b;
}

#endif
