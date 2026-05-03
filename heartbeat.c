#include "heartbeat.h"

#include "common.h"
#include "log.h"
#include "protocol.h"
#include "time.h"

#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <unistd.h>

static void make_heartbeat_packet(uint8_t packet[44]) {
    memset(packet, 0, 44);
    memcpy(packet, SMID_MAGIC, 12);
    smid_put32(packet + 12, 44);
    smid_put32(packet + 16, 6);
    packet[20] = 0xec;
}

static void *heartbeat_thread_main(void *arg) {
    struct smid_heartbeat *h = arg;
    uint64_t next_us = smid_mono_us() + (uint64_t)h->interval_ms * 1000u;
    while (!atomic_load_explicit(&h->stop, memory_order_acquire)) {
        smid_sleep_until_us(next_us);
        if (atomic_load_explicit(&h->stop, memory_order_acquire)) {
            break;
        }
        next_us += (uint64_t)h->interval_ms * 1000u;

        if (smid_transport_recent_success(h->tx, (uint64_t)h->quiet_ms * 1000u)) {
            continue;
        }

        uint8_t packet[44];
        make_heartbeat_packet(packet);
        struct smid_tx_item item = {
            .priority = SMID_TX_PRIO_HEARTBEAT,
            .endpoint = SMID_EP_BULK_OUT,
            .xfer = SMID_USB_XFER_BULK,
            .timeout_ms = 100,
            .data = packet,
            .len = sizeof(packet),
            .name = "heartbeat",
        };
        if (smid_transport_submit(h->tx, &item)) {
            smid_logf("heartbeat enqueue failed\n");
        }

        uint64_t now_us = smid_mono_us();
        if (now_us > next_us + (uint64_t)h->interval_ms * 1000u) {
            next_us = now_us + (uint64_t)h->interval_ms * 1000u;
        }
    }
    return NULL;
}

int smid_heartbeat_start(struct smid_heartbeat *h, struct smid_transport *tx) {
    memset(h, 0, sizeof(*h));
    h->tx = tx;
    h->interval_ms = 1000;
    h->quiet_ms = 900;
    atomic_init(&h->stop, false);
    if (pthread_create(&h->thread, NULL, heartbeat_thread_main, h)) {
        return -1;
    }
    h->started = true;
    return 0;
}

void smid_heartbeat_stop(struct smid_heartbeat *h) {
    if (!h->started) {
        return;
    }
    atomic_store_explicit(&h->stop, true, memory_order_release);
    pthread_join(h->thread, NULL);
    memset(h, 0, sizeof(*h));
}
