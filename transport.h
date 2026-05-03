#ifndef SMID_TRANSPORT_H
#define SMID_TRANSPORT_H

#include "common.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum smid_usb_xfer {
    SMID_USB_XFER_INTR = 1,
    SMID_USB_XFER_BULK = 3,
};

enum smid_tx_priority {
    SMID_TX_PRIO_CONTROL = 0,
    SMID_TX_PRIO_VIDEO = 1,
    SMID_TX_PRIO_CURSOR = 2,
    SMID_TX_PRIO_HEARTBEAT = 3,
    SMID_TX_PRIO_COUNT = 4,
};

enum {
    SMID_TX_INLINE_CAP = 128,
    SMID_TX_QUEUE_CAP = 256,
};

struct smid_tx_result {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    bool done;
    int rc;
    int transferred;
    uint64_t done_us;
};

struct smid_tx_item {
    enum smid_tx_priority priority;
    uint64_t serial;
    uint8_t endpoint;
    enum smid_usb_xfer xfer;
    uint32_t timeout_ms;
    const uint8_t *data;
    uint32_t len;
    uint8_t inline_data[SMID_TX_INLINE_CAP];
    bool owns_result;
    struct smid_tx_result *result;
    const char *name;
};

struct smid_transport {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    pthread_t thread;
    bool started;
    bool stop;
    void *usb_handle;
    uint64_t serial_next;
    atomic_uint_fast64_t last_success_us;
    unsigned dropped[SMID_TX_PRIO_COUNT];
    struct smid_tx_item queues[SMID_TX_PRIO_COUNT][SMID_TX_QUEUE_CAP];
    unsigned head[SMID_TX_PRIO_COUNT];
    unsigned tail[SMID_TX_PRIO_COUNT];
    unsigned count[SMID_TX_PRIO_COUNT];
};

void smid_tx_result_init(struct smid_tx_result *r);
void smid_tx_result_destroy(struct smid_tx_result *r);
void smid_tx_result_reset(struct smid_tx_result *r);
int smid_tx_result_wait(struct smid_tx_result *r);
int smid_tx_result_wait_done_us(struct smid_tx_result *r, uint64_t *done_us);
bool smid_tx_result_poll(struct smid_tx_result *r, int *rc, int *transferred);

int smid_transport_start(struct smid_transport *t, void *usb_handle);
void smid_transport_stop(struct smid_transport *t);
int smid_transport_submit(struct smid_transport *t, const struct smid_tx_item *item);
int smid_transport_submit_sync(struct smid_transport *t, struct smid_tx_item item);
bool smid_transport_recent_success(struct smid_transport *t, uint64_t window_us);

#endif
