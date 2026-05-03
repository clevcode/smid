#ifndef SMID_USB_H
#define SMID_USB_H

#include "protocol.h"
#include "transport.h"

#include <libusb.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

struct smid_usb {
    bool initialized;
    libusb_context *ctx;
    libusb_device_handle *handle;
    bool iface_claimed;
    struct smid_transport tx;
    pthread_t rx_thread;
    pthread_mutex_t rx_lock;
    pthread_cond_t rx_cond;
    bool rx_started;
    atomic_bool stop_rx;
    uint64_t rx_count;
    uint64_t rx_code_count[256];
    uint8_t rx_code_status[256];
    atomic_uint_fast32_t ack_a;
    atomic_uint_fast32_t ack_b;
    atomic_uint_fast64_t ack_a_generation;
    atomic_uint_fast64_t ack_b_generation;
    atomic_bool ack_a_seen;
    atomic_bool ack_b_seen;
    atomic_int rx_error;
};

int smid_usb_open(struct smid_usb *u);
void smid_usb_close(struct smid_usb *u);
int smid_usb_reconnect(struct smid_usb *u);
int smid_usb_start_io(struct smid_usb *u);
void smid_usb_stop_io(struct smid_usb *u);
void smid_usb_rx_snapshot(struct smid_usb *u, uint8_t code, uint64_t *rx_count,
                          uint64_t *code_count);
bool smid_usb_code_seen_after(struct smid_usb *u, uint8_t code, uint64_t code_start,
                              uint8_t *status);
bool smid_usb_wait_code(struct smid_usb *u, uint64_t rx_start, uint64_t code_start,
                        uint8_t code, uint32_t timeout_ms, uint8_t *status);
bool smid_usb_ack_a_within_window(struct smid_usb *u, uint32_t seq, unsigned window,
                                  uint32_t *ack_out, bool *seen_out,
                                  unsigned *lag_out);
void smid_usb_ack_snapshot(struct smid_usb *u, uint32_t *ack_a, uint32_t *ack_b,
                           bool *ack_a_seen, bool *ack_b_seen);
void smid_usb_ack_clear(struct smid_usb *u);
uint64_t smid_usb_ack_a_generation(struct smid_usb *u);
int smid_usb_take_rx_error(struct smid_usb *u);
int smid_usb_send_cmds(struct smid_usb *u, const struct smid_rawcmd *cmds,
                       size_t n, const char *phase);

#endif
