#ifndef SMID_HEARTBEAT_H
#define SMID_HEARTBEAT_H

#include "transport.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

struct smid_heartbeat {
    pthread_t thread;
    struct smid_transport *tx;
    bool started;
    atomic_bool stop;
    uint32_t interval_ms;
    uint32_t quiet_ms;
};

int smid_heartbeat_start(struct smid_heartbeat *h, struct smid_transport *tx);
void smid_heartbeat_stop(struct smid_heartbeat *h);

#endif
