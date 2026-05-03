#include "usb.h"

#include "common.h"
#include "log.h"
#include "protocol.h"
#include "time.h"

#include <errno.h>
#include <libusb.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

enum {
    SMID_USB_RESET_COOLDOWN_S = 3,
    SMID_ACK_WRAP_LOW_SEQ = 1024,
    SMID_ACK_WRAP_HIGH_SEQ = 0xfffffc00u,
};

static const char SMID_USB_RESET_STAMP[] = "/tmp/smid-090c-0760-reset.stamp";

static void reset_cooldown_wait(void) {
    struct stat st;
    if (stat(SMID_USB_RESET_STAMP, &st)) {
        return;
    }
    time_t now = time(NULL);
    if (now == (time_t)-1 || now < st.st_mtime) {
        return;
    }
    time_t age = now - st.st_mtime;
    if (age < SMID_USB_RESET_COOLDOWN_S) {
        smid_sleep_until_us(smid_mono_us() +
                (uint64_t)(SMID_USB_RESET_COOLDOWN_S - age) * 1000000u);
    }
}

static void reset_cooldown_mark(void) {
    FILE *f = fopen(SMID_USB_RESET_STAMP, "w");
    if (f) {
        fputs("reset\n", f);
        fclose(f);
    }
}

static libusb_device_handle *open_wait(libusb_context *ctx, uint32_t timeout_ms) {
    uint64_t end = smid_mono_us() + (uint64_t)timeout_ms * 1000u;
    do {
        libusb_device_handle *h = libusb_open_device_with_vid_pid(ctx, SMID_USB_VID, SMID_USB_PID);
        if (h) {
            return h;
        }
        smid_sleep_until_us(smid_mono_us() + 100000u);
    } while (smid_mono_us() < end);
    return NULL;
}

static void close_handle_only(struct smid_usb *u) {
    if (!u->handle) {
        return;
    }
    if (u->iface_claimed) {
        libusb_release_interface(u->handle, SMID_USB_IFACE);
        u->iface_claimed = false;
    }
    libusb_close(u->handle);
    u->handle = NULL;
}

static int configure_handle(struct smid_usb *u) {
    libusb_set_auto_detach_kernel_driver(u->handle, 1);
    int rc = libusb_set_configuration(u->handle, 1);
    if (rc && rc != LIBUSB_ERROR_BUSY) {
        smid_logf("set_configuration: %s\n", libusb_error_name(rc));
        return rc;
    }
    rc = libusb_claim_interface(u->handle, SMID_USB_IFACE);
    if (rc) {
        smid_logf("claim_interface: %s\n", libusb_error_name(rc));
        return rc;
    }
    u->iface_claimed = true;
    rc = libusb_set_interface_alt_setting(u->handle, SMID_USB_IFACE, SMID_USB_ALTSETTING);
    if (rc) {
        smid_logf("alt_setting: %s\n", libusb_error_name(rc));
        return rc;
    }
    return 0;
}

static int clear_endpoint_halt(struct smid_usb *u, unsigned char endpoint) {
    int rc = libusb_clear_halt(u->handle, endpoint);
    if (!rc) {
        return 0;
    }
    smid_logf("warn: clear_halt ep=0x%02x: %s\n", endpoint, libusb_error_name(rc));
    if (rc == LIBUSB_ERROR_NO_DEVICE || rc == LIBUSB_ERROR_IO) {
        return rc;
    }
    return 0;
}

static int clear_endpoint_halts(struct smid_usb *u) {
    int rc = clear_endpoint_halt(u, SMID_EP_BULK_OUT);
    if (rc) {
        return rc;
    }
    rc = clear_endpoint_halt(u, SMID_EP_INTR_OUT);
    if (rc) {
        return rc;
    }
    return clear_endpoint_halt(u, SMID_EP_INTR_IN);
}

static int open_configured_handle(struct smid_usb *u, uint32_t timeout_ms,
                                  bool clear_halts) {
    u->handle = open_wait(u->ctx, timeout_ms);
    if (!u->handle) {
        return LIBUSB_ERROR_NO_DEVICE;
    }
    int rc = configure_handle(u);
    if (rc) {
        close_handle_only(u);
        return rc;
    }
    if (clear_halts) {
        rc = clear_endpoint_halts(u);
        if (rc) {
            close_handle_only(u);
            return rc;
        }
    }
    return 0;
}

int smid_usb_open(struct smid_usb *u) {
    memset(u, 0, sizeof(*u));
    pthread_mutex_init(&u->rx_lock, NULL);
    pthread_cond_init(&u->rx_cond, NULL);
    u->initialized = true;

    int rc = libusb_init_context(&u->ctx, NULL, 0);
    if (rc) {
        smid_logf("libusb_init: %s\n", libusb_error_name(rc));
        goto fail;
    }
    u->handle = open_wait(u->ctx, 1000);
    if (!u->handle) {
        smid_logf("SMI USB display %04x:%04x not found or permission denied\n",
                SMID_USB_VID, SMID_USB_PID);
        goto fail;
    }

    libusb_set_auto_detach_kernel_driver(u->handle, 1);
    reset_cooldown_wait();
    rc = libusb_reset_device(u->handle);
    reset_cooldown_mark();
    if (rc) {
        smid_logf("reset_device: %s\n", libusb_error_name(rc));
    }
    close_handle_only(u);
    smid_sleep_until_us(smid_mono_us() + 500000u);

    rc = open_configured_handle(u, 5000, true);
    if (rc == LIBUSB_ERROR_NO_DEVICE) {
        smid_logf("SMI USB display did not reappear after reset\n");
        goto fail;
    }
    if (rc) {
        goto fail;
    }
    smid_sleep_until_us(smid_mono_us() + 500000u);
    return 0;

fail:
    smid_usb_close(u);
    return -1;
}

static void rx_publish(struct smid_usb *u, const uint8_t *b, int got) {
    if (got < 24 || memcmp(b, SMID_MAGIC, 12)) {
        return;
    }
    uint32_t v = smid_get32(b + 16);
    uint32_t seq = smid_get32(b + 20);
    uint8_t code = (uint8_t)v;
    pthread_mutex_lock(&u->rx_lock);
    u->rx_code_count[code]++;
    u->rx_code_status[code] = b[17];
    if ((v & 0x00ffffffu) == 0x000001e9u) {
        if (v & 0x00010000u) {
            atomic_store_explicit(&u->ack_b, seq, memory_order_release);
            atomic_fetch_add_explicit(&u->ack_b_generation, 1, memory_order_release);
            atomic_store_explicit(&u->ack_b_seen, true, memory_order_release);
        } else {
            atomic_store_explicit(&u->ack_a, seq, memory_order_release);
            atomic_fetch_add_explicit(&u->ack_a_generation, 1, memory_order_release);
            atomic_store_explicit(&u->ack_a_seen, true, memory_order_release);
        }
    }
    u->rx_count++;
    pthread_cond_broadcast(&u->rx_cond);
    pthread_mutex_unlock(&u->rx_lock);
}

static void *rx_thread_main(void *arg) {
    struct smid_usb *u = arg;
    uint8_t b[1024];
    while (!atomic_load_explicit(&u->stop_rx, memory_order_acquire)) {
        int got = 0;
        int rc = libusb_interrupt_transfer(u->handle, SMID_EP_INTR_IN, b, sizeof(b), &got, 250);
        if (rc == LIBUSB_ERROR_TIMEOUT) {
            continue;
        }
        if (rc) {
            if (!atomic_load_explicit(&u->stop_rx, memory_order_acquire)) {
                atomic_store_explicit(&u->rx_error, rc, memory_order_release);
                smid_logf("usb rx stopped: %s\n", libusb_error_name(rc));
            }
            break;
        }
        rx_publish(u, b, got);
    }
    return NULL;
}

int smid_usb_start_io(struct smid_usb *u) {
    atomic_store_explicit(&u->stop_rx, false, memory_order_release);
    atomic_store_explicit(&u->rx_error, 0, memory_order_release);
    if (smid_transport_start(&u->tx, u->handle)) {
        smid_logf("usb transport start failed\n");
        return -1;
    }
    if (pthread_create(&u->rx_thread, NULL, rx_thread_main, u)) {
        smid_transport_stop(&u->tx);
        smid_logf("usb rx thread start failed\n");
        return -1;
    }
    u->rx_started = true;
    return 0;
}

void smid_usb_stop_io(struct smid_usb *u) {
    if (!u->initialized) {
        return;
    }
    atomic_store_explicit(&u->stop_rx, true, memory_order_release);
    pthread_mutex_lock(&u->rx_lock);
    pthread_cond_broadcast(&u->rx_cond);
    pthread_mutex_unlock(&u->rx_lock);
    if (u->rx_started) {
        pthread_join(u->rx_thread, NULL);
        u->rx_started = false;
    }
    smid_transport_stop(&u->tx);
}

int smid_usb_reconnect(struct smid_usb *u) {
    if (!u->initialized || !u->ctx) {
        return LIBUSB_ERROR_NO_DEVICE;
    }
    smid_usb_stop_io(u);
    close_handle_only(u);
    smid_usb_ack_clear(u);
    atomic_store_explicit(&u->rx_error, 0, memory_order_release);

    int rc = open_configured_handle(u, 5000, true);
    if (rc) {
        smid_logf("usb endpoint reconnect failed: %s\n", libusb_error_name(rc));
        return rc;
    }
    smid_logf("usb endpoint reconnect complete\n");
    return smid_usb_start_io(u);
}

void smid_usb_close(struct smid_usb *u) {
    if (!u->initialized) {
        return;
    }
    smid_usb_stop_io(u);
    close_handle_only(u);
    if (u->ctx) {
        libusb_exit(u->ctx);
        u->ctx = NULL;
    }
    reset_cooldown_mark();
    pthread_cond_destroy(&u->rx_cond);
    pthread_mutex_destroy(&u->rx_lock);
    memset(u, 0, sizeof(*u));
}

void smid_usb_rx_snapshot(struct smid_usb *u, uint8_t code, uint64_t *rx_count,
                          uint64_t *code_count) {
    pthread_mutex_lock(&u->rx_lock);
    *rx_count = u->rx_count;
    *code_count = code ? u->rx_code_count[code] : 0;
    pthread_mutex_unlock(&u->rx_lock);
}

bool smid_usb_code_seen_after(struct smid_usb *u, uint8_t code, uint64_t code_start,
                              uint8_t *status) {
    pthread_mutex_lock(&u->rx_lock);
    bool seen = code && u->rx_code_count[code] > code_start;
    if (seen && status) {
        *status = u->rx_code_status[code];
    }
    pthread_mutex_unlock(&u->rx_lock);
    return seen;
}

bool smid_usb_wait_code(struct smid_usb *u, uint64_t rx_start, uint64_t code_start,
                        uint8_t code, uint32_t timeout_ms, uint8_t *status) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }

    pthread_mutex_lock(&u->rx_lock);
    while (!atomic_load_explicit(&u->stop_rx, memory_order_acquire)) {
        if ((code && u->rx_code_count[code] > code_start) || (!code && u->rx_count > rx_start)) {
            if (status) {
                *status = code ? u->rx_code_status[code] : 0;
            }
            pthread_mutex_unlock(&u->rx_lock);
            return true;
        }
        if (pthread_cond_timedwait(&u->rx_cond, &u->rx_lock, &ts) == ETIMEDOUT) {
            break;
        }
    }
    pthread_mutex_unlock(&u->rx_lock);
    return false;
}

static bool ack_pre_wrap_stale(uint32_t seq, uint32_t ack) {
    /* Let post-wrap frames through until the device reports a post-wrap ACK. */
    return seq < SMID_ACK_WRAP_LOW_SEQ && ack >= SMID_ACK_WRAP_HIGH_SEQ;
}

bool smid_usb_ack_a_within_window(struct smid_usb *u, uint32_t seq, unsigned window,
                                  uint32_t *ack_out, bool *seen_out,
                                  unsigned *lag_out) {
    bool seen = atomic_load_explicit(&u->ack_a_seen, memory_order_acquire);
    uint32_t ack = atomic_load_explicit(&u->ack_a, memory_order_acquire);
    unsigned lag = seen ? (unsigned)(seq - ack) : 0;
    bool ready = !window || !seen || ack_pre_wrap_stale(seq, ack) || lag <= window;
    if (ack_out) {
        *ack_out = ack;
    }
    if (seen_out) {
        *seen_out = seen;
    }
    if (lag_out) {
        *lag_out = lag;
    }
    return ready;
}

void smid_usb_ack_snapshot(struct smid_usb *u, uint32_t *ack_a, uint32_t *ack_b,
                           bool *ack_a_seen, bool *ack_b_seen) {
    if (ack_a) {
        *ack_a = atomic_load_explicit(&u->ack_a, memory_order_acquire);
    }
    if (ack_b) {
        *ack_b = atomic_load_explicit(&u->ack_b, memory_order_acquire);
    }
    if (ack_a_seen) {
        *ack_a_seen = atomic_load_explicit(&u->ack_a_seen, memory_order_acquire);
    }
    if (ack_b_seen) {
        *ack_b_seen = atomic_load_explicit(&u->ack_b_seen, memory_order_acquire);
    }
}

void smid_usb_ack_clear(struct smid_usb *u) {
    atomic_store_explicit(&u->ack_a, 0, memory_order_release);
    atomic_store_explicit(&u->ack_b, 0, memory_order_release);
    atomic_store_explicit(&u->ack_a_seen, false, memory_order_release);
    atomic_store_explicit(&u->ack_b_seen, false, memory_order_release);
    atomic_fetch_add_explicit(&u->ack_a_generation, 1, memory_order_release);
    atomic_fetch_add_explicit(&u->ack_b_generation, 1, memory_order_release);
}

uint64_t smid_usb_ack_a_generation(struct smid_usb *u) {
    return atomic_load_explicit(&u->ack_a_generation, memory_order_acquire);
}

int smid_usb_take_rx_error(struct smid_usb *u) {
    return atomic_exchange_explicit(&u->rx_error, 0, memory_order_acq_rel);
}

static int send_cmd_once(struct smid_usb *u, const struct smid_rawcmd *c) {
    uint64_t rx_before = 0;
    uint64_t code_before = 0;
    smid_usb_rx_snapshot(u, c->wait_code, &rx_before, &code_before);

    struct smid_tx_item item = {
        .priority = SMID_TX_PRIO_CONTROL,
        .endpoint = c->endpoint,
        .xfer = c->xfer == 1 ? SMID_USB_XFER_INTR : SMID_USB_XFER_BULK,
        .timeout_ms = 2000,
        .data = c->data,
        .len = c->len,
        .name = c->name,
    };
    int rc = smid_transport_submit_sync(&u->tx, item);
    if (rc) {
        return rc;
    }

    uint8_t status = 0xff;
    if (c->wait_code && !smid_usb_wait_code(u, rx_before, code_before, c->wait_code, 1200, &status)) {
        smid_logf("warn: no response for %s code=0x%02x\n", c->name, c->wait_code);
        if (c->wait_code == 0x30) {
            return LIBUSB_ERROR_TIMEOUT;
        }
    }
    if (c->wait_code == 0x30 && status != 0) {
        smid_logf("warn: set-mode status=0x%02x for %s\n", status, c->name);
        return LIBUSB_ERROR_BUSY;
    }
    return 0;
}

static int send_cmd_with_retry(struct smid_usb *u, const struct smid_rawcmd *c) {
    int tries = c->wait_code == 0x30 ? 4 : 1;
    int rc = 0;
    for (int i = 0; i < tries; i++) {
        rc = send_cmd_once(u, c);
        if (!rc) {
            return 0;
        }
        smid_sleep_until_us(smid_mono_us() + 20000u);
    }
    return rc;
}

int smid_usb_send_cmds(struct smid_usb *u, const struct smid_rawcmd *cmds,
                       size_t n, const char *phase) {
    for (size_t i = 0; i < n; i++) {
        int rc = send_cmd_with_retry(u, &cmds[i]);
        if (rc) {
            smid_logf("%s: command %zu %s failed: %s\n",
                    phase, i, cmds[i].name, libusb_error_name(rc));
            return -1;
        }
    }
    return 0;
}
