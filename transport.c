#include "transport.h"

#include "log.h"
#include "time.h"

#include <libusb.h>
#include <pthread.h>
#include <string.h>

static int priority_valid(enum smid_tx_priority prio) {
    return prio >= 0 && prio < SMID_TX_PRIO_COUNT;
}

void smid_tx_result_init(struct smid_tx_result *r) {
    memset(r, 0, sizeof(*r));
    pthread_mutex_init(&r->lock, NULL);
    pthread_cond_init(&r->cond, NULL);
}

void smid_tx_result_destroy(struct smid_tx_result *r) {
    pthread_cond_destroy(&r->cond);
    pthread_mutex_destroy(&r->lock);
}

void smid_tx_result_reset(struct smid_tx_result *r) {
    pthread_mutex_lock(&r->lock);
    r->done = false;
    r->rc = 0;
    r->transferred = 0;
    r->done_us = 0;
    pthread_mutex_unlock(&r->lock);
}

int smid_tx_result_wait_done_us(struct smid_tx_result *r, uint64_t *done_us) {
    pthread_mutex_lock(&r->lock);
    while (!r->done) {
        pthread_cond_wait(&r->cond, &r->lock);
    }
    int rc = r->rc;
    if (done_us) {
        *done_us = r->done_us;
    }
    pthread_mutex_unlock(&r->lock);
    return rc;
}

int smid_tx_result_wait(struct smid_tx_result *r) {
    return smid_tx_result_wait_done_us(r, NULL);
}

bool smid_tx_result_poll(struct smid_tx_result *r, int *rc, int *transferred) {
    pthread_mutex_lock(&r->lock);
    bool done = r->done;
    if (done) {
        if (rc) {
            *rc = r->rc;
        }
        if (transferred) {
            *transferred = r->transferred;
        }
    }
    pthread_mutex_unlock(&r->lock);
    return done;
}

static void tx_result_complete(struct smid_tx_result *r, int rc, int transferred) {
    if (!r) {
        return;
    }
    pthread_mutex_lock(&r->lock);
    r->rc = rc;
    r->transferred = transferred;
    r->done_us = smid_mono_us();
    r->done = true;
    pthread_cond_broadcast(&r->cond);
    pthread_mutex_unlock(&r->lock);
}

static bool queue_pop_locked(struct smid_transport *t, struct smid_tx_item *out) {
    for (int prio = 0; prio < SMID_TX_PRIO_COUNT; prio++) {
        if (!t->count[prio]) {
            continue;
        }
        *out = t->queues[prio][t->tail[prio]];
        if (out->len <= SMID_TX_INLINE_CAP) {
            memcpy(out->inline_data, t->queues[prio][t->tail[prio]].inline_data, out->len);
            out->data = out->inline_data;
        }
        t->tail[prio] = (t->tail[prio] + 1u) % SMID_TX_QUEUE_CAP;
        t->count[prio]--;
        return true;
    }
    return false;
}

static int send_item(struct smid_transport *t, const struct smid_tx_item *item,
                     int *transferred) {
    *transferred = 0;
    if (!t->usb_handle) {
        return LIBUSB_ERROR_NO_DEVICE;
    }
    if (item->xfer == SMID_USB_XFER_INTR) {
        return libusb_interrupt_transfer(t->usb_handle, item->endpoint,
                (uint8_t *)item->data, (int)item->len, transferred, item->timeout_ms);
    }
    return libusb_bulk_transfer(t->usb_handle, item->endpoint,
            (uint8_t *)item->data, (int)item->len, transferred, item->timeout_ms);
}

static void *transport_thread_main(void *arg) {
    struct smid_transport *t = arg;
    for (;;) {
        struct smid_tx_item item;
        pthread_mutex_lock(&t->lock);
        while (!t->stop && !queue_pop_locked(t, &item)) {
            pthread_cond_wait(&t->cond, &t->lock);
        }
        bool stop = t->stop;
        pthread_mutex_unlock(&t->lock);
        if (stop) {
            break;
        }

        int transferred = 0;
        int rc = send_item(t, &item, &transferred);
        if (!rc && transferred == (int)item.len) {
            atomic_store_explicit(&t->last_success_us, smid_mono_us(), memory_order_release);
        } else {
            int report_rc = rc ? rc : LIBUSB_ERROR_IO;
            smid_logf("usb tx failed prio=%d ep=0x%02x len=%u tx=%d rc=%s name=%s\n",
                    item.priority, item.endpoint, item.len, transferred,
                    libusb_error_name(report_rc), item.name ? item.name : "-");
            rc = report_rc;
        }
        tx_result_complete(item.result, rc, transferred);
    }

    pthread_mutex_lock(&t->lock);
    for (int prio = 0; prio < SMID_TX_PRIO_COUNT; prio++) {
        while (t->count[prio]) {
            struct smid_tx_item item = t->queues[prio][t->tail[prio]];
            t->tail[prio] = (t->tail[prio] + 1u) % SMID_TX_QUEUE_CAP;
            t->count[prio]--;
            tx_result_complete(item.result, LIBUSB_ERROR_INTERRUPTED, 0);
        }
    }
    pthread_mutex_unlock(&t->lock);
    return NULL;
}

int smid_transport_start(struct smid_transport *t, void *usb_handle) {
    memset(t, 0, sizeof(*t));
    pthread_mutex_init(&t->lock, NULL);
    pthread_cond_init(&t->cond, NULL);
    t->usb_handle = usb_handle;
    if (pthread_create(&t->thread, NULL, transport_thread_main, t)) {
        pthread_cond_destroy(&t->cond);
        pthread_mutex_destroy(&t->lock);
        return -1;
    }
    t->started = true;
    return 0;
}

void smid_transport_stop(struct smid_transport *t) {
    if (!t->started) {
        return;
    }
    pthread_mutex_lock(&t->lock);
    t->stop = true;
    pthread_cond_broadcast(&t->cond);
    pthread_mutex_unlock(&t->lock);
    pthread_join(t->thread, NULL);
    pthread_mutex_destroy(&t->lock);
    pthread_cond_destroy(&t->cond);
    memset(t, 0, sizeof(*t));
}

int smid_transport_submit(struct smid_transport *t, const struct smid_tx_item *item) {
    if (!priority_valid(item->priority) || !item->data || !item->len) {
        return -1;
    }
    if (item->len > SMID_TX_INLINE_CAP && !item->result) {
        return -1;
    }

    pthread_mutex_lock(&t->lock);
    if (t->stop) {
        pthread_mutex_unlock(&t->lock);
        return -1;
    }
    enum smid_tx_priority prio = item->priority;
    if (t->count[prio] == SMID_TX_QUEUE_CAP) {
        if (prio == SMID_TX_PRIO_HEARTBEAT) {
            t->dropped[prio]++;
            pthread_mutex_unlock(&t->lock);
            return 0;
        }
        pthread_mutex_unlock(&t->lock);
        return -1;
    }

    struct smid_tx_item *dst = &t->queues[prio][t->head[prio]];
    *dst = *item;
    dst->serial = ++t->serial_next;
    if (dst->len <= SMID_TX_INLINE_CAP) {
        memcpy(dst->inline_data, item->data, dst->len);
        dst->data = dst->inline_data;
    }
    t->head[prio] = (t->head[prio] + 1u) % SMID_TX_QUEUE_CAP;
    t->count[prio]++;
    pthread_cond_signal(&t->cond);
    pthread_mutex_unlock(&t->lock);
    return 0;
}

int smid_transport_submit_sync(struct smid_transport *t, struct smid_tx_item item) {
    struct smid_tx_result result;
    smid_tx_result_init(&result);
    item.result = &result;
    int rc = smid_transport_submit(t, &item);
    if (!rc) {
        rc = smid_tx_result_wait(&result);
    }
    smid_tx_result_destroy(&result);
    return rc;
}

bool smid_transport_recent_success(struct smid_transport *t, uint64_t window_us) {
    uint64_t last = atomic_load_explicit(&t->last_success_us, memory_order_acquire);
    return last && smid_mono_us() - last < window_us;
}
