#include "common.h"
#include "cnm.h"
#include "encoder.h"
#include "evdi_source.h"
#include "frame_packet.h"
#include "heartbeat.h"
#include "log.h"
#include "protocol.h"
#include "time.h"
#include "transport.h"
#include "usb.h"

#include <libusb.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t stop_requested;

enum {
    SMID_VIDEO_RETAINED_PERIOD = 3,
};

struct smid_video_tile {
    int stream;
    int tile;
    struct smid_encoder encoder;
    struct smid_encoder companion_encoder;
    bool companion_encoder_ready;
    unsigned long jpeg_cap;
    struct smid_frame_slot frame[2];
    unsigned long jpeg_len;
    struct smid_tx_result tx_result[2];
    bool tx_result_ready[2];
};

struct smid_video_build_worker {
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    bool initialized;
    bool started;
    bool stop;
    bool has_job;
    bool done;
    int rc;
    struct smid_video_tile *tile;
    struct smid_evdi_source *evdi;
    struct smid_cnm cnm;
    bool cnm_ready;
    uint32_t seq;
    int quality;
    int slot;
    bool companion;
};

struct smid_video_inflight {
    bool active;
    int slot;
    int submitted[4];
    int submitted_n;
    uint8_t send_mask;
    uint32_t seq[2];
    uint64_t frame_start_us;
    uint64_t submit_us;
    uint64_t build_wall_us;
    uint64_t wire_bytes;
};

struct smid_video_retained_band {
    struct smid_frame_hash hash[SMID_VIDEO_RETAINED_PERIOD];
    bool valid[SMID_VIDEO_RETAINED_PERIOD];
    unsigned next;
    unsigned count;
};

struct smid_video_send_plan {
    uint8_t send_mask;
    uint8_t companion_mask;
    uint8_t hash_valid_mask;
    struct smid_frame_hash hash[4];
};

struct smid_video_timing_estimate {
    uint64_t build_wall_us;
    uint64_t tx_wait_us;
};

enum {
    SMID_JPEG_QUALITY_FLOOR_DEFAULT = 70,
    SMID_JPEG_QUALITY_UP_GOOD_WINDOWS = 2,
    SMID_JPEG_QUALITY_DOWN_PRESSURE_WINDOWS = 4,
    SMID_JPEG_QUALITY_SERVICE_DOWN_PRESSURE_WINDOWS = 2,
    SMID_JPEG_QUALITY_IDLE_RESET_US = 500000,
    SMID_JPEG_QUALITY_ACK_PACED_BLOCK_PCT = 25,
    SMID_JPEG_QUALITY_ACK_PACED_HOLD_WINDOWS = 3,
    SMID_JPEG_QUALITY_FRAME_PRESSURE_PCT = 85,
    SMID_JPEG_QUALITY_DELIVERY_PRESSURE_PCT = 97,
    SMID_JPEG_QUALITY_WIRE_PRESSURE_PCT = 105,
    SMID_JPEG_QUALITY_WIRE_SPARE_PCT = 95,
    SMID_JPEG_QUALITY_ENCODER_BUDGET_PCT = 65,
    SMID_JPEG_QUALITY_SERVICE_FRAME_PCT = 130,
    SMID_JPEG_QUALITY_SERVICE_TAIL_PCT = 240,
    SMID_JPEG_QUALITY_SERVICE_TX_PCT = 75,
    SMID_QUALITY_TARGET_USB_MIB_S_DEFAULT = 36,
    SMID_VIDEO_LATE_REBUILD_WAIT_US = 2000,
    SMID_VIDEO_JIT_DEFAULT_BUILD_WALL_US = 7000,
    SMID_VIDEO_JIT_DEFAULT_TX_WAIT_US = 18000,
    SMID_VIDEO_JIT_MARGIN_US = 1000,
    SMID_VIDEO_JIT_MIN_WAIT_US = 1000,
    SMID_VIDEO_JIT_POLL_US = 500,
    SMID_VIDEO_COMPANION_RECT_X = 32,
    SMID_VIDEO_COMPANION_RECT_Y_IN_BAND = 32,
    SMID_VIDEO_COMPANION_RECT_SIZE = 32,
};

struct smid_quality_controller {
    int floor;
    int ceiling;
    int current;
    int target_fps;
    bool fixed;
    int encoder_pressure_windows;
    int service_pressure_windows;
    int spare_windows;
    int ack_paced_hold_windows;
    uint64_t target_wire_bytes_per_s;
};

static void on_signal(int sig) {
    (void)sig;
    stop_requested = 1;
}

static void quality_controller_init(struct smid_quality_controller *q, int ceiling, int fps,
                                    int target_mib_s, bool fixed) {
    if (ceiling < 1) {
        ceiling = 1;
    } else if (ceiling > 100) {
        ceiling = 100;
    }
    q->ceiling = ceiling;
    q->floor = fixed ? ceiling : SMID_JPEG_QUALITY_FLOOR_DEFAULT;
    if (q->floor > q->ceiling) {
        q->floor = q->ceiling;
    }
    q->current = q->ceiling;
    q->target_fps = fps > 0 ? fps : 60;
    q->fixed = fixed;
    q->encoder_pressure_windows = 0;
    q->service_pressure_windows = 0;
    q->spare_windows = 0;
    q->ack_paced_hold_windows = 0;
    if (target_mib_s < 1) {
        target_mib_s = SMID_QUALITY_TARGET_USB_MIB_S_DEFAULT;
    }
    q->target_wire_bytes_per_s =
            (uint64_t)target_mib_s * 1024u * 1024u;
}

static int quality_controller_current(const struct smid_quality_controller *q) {
    return q->current;
}

static void quality_controller_update(struct smid_quality_controller *q,
                                      uint64_t elapsed_us, uint64_t frames,
                                      const uint64_t stream_frames[2],
                                      const uint64_t stream_dirty[2],
                                      int streams, uint64_t dirty_events,
                                      uint64_t wire_bytes,
                                      uint64_t ack_waits, uint64_t ack_blocks,
                                      uint64_t build_wall_us,
                                      uint64_t tx_wait_us,
                                      uint64_t frame_us,
                                      uint64_t frame_max_us) {
    if (q->fixed) {
        return;
    }
    if (q->floor >= q->ceiling) {
        return;
    }
    if (!elapsed_us) {
        return;
    }

    uint64_t target_wire_bytes = q->target_wire_bytes_per_s * elapsed_us / 1000000u;
    uint64_t configured_target_frames = (uint64_t)q->target_fps * elapsed_us / 1000000u;
    if (!configured_target_frames) {
        configured_target_frames = 1;
    }
    bool wire_pressure = wire_bytes > target_wire_bytes *
            SMID_JPEG_QUALITY_WIRE_PRESSURE_PCT / 100u;
    bool wire_below_target = wire_bytes < target_wire_bytes;
    bool wire_spare = wire_bytes < target_wire_bytes *
            SMID_JPEG_QUALITY_WIRE_SPARE_PCT / 100u;
    uint64_t delivered_frames = frames;
    uint64_t min_active_frames = UINT64_MAX;
    uint64_t observed_target_frames = 0;
    bool active_demand = false;
    for (int i = 0; i < streams && i < 2; i++) {
        if (stream_dirty[i] > observed_target_frames) {
            observed_target_frames = stream_dirty[i];
        }
        if (stream_dirty[i] * 100u >= configured_target_frames *
                SMID_JPEG_QUALITY_FRAME_PRESSURE_PCT) {
            active_demand = true;
            if (stream_frames[i] < min_active_frames) {
                min_active_frames = stream_frames[i];
            }
        }
    }
    if (active_demand) {
        delivered_frames = min_active_frames;
    } else if (streams <= 1 && dirty_events * 100u >= configured_target_frames *
               SMID_JPEG_QUALITY_FRAME_PRESSURE_PCT) {
        active_demand = true;
        if (dirty_events > observed_target_frames) {
            observed_target_frames = dirty_events;
        }
    }
    if (observed_target_frames > configured_target_frames) {
        observed_target_frames = configured_target_frames;
    }
    if (!observed_target_frames) {
        observed_target_frames = configured_target_frames;
    }
    bool delivery_pressure = active_demand &&
            delivered_frames * 100u < observed_target_frames *
                    SMID_JPEG_QUALITY_DELIVERY_PRESSURE_PCT;
    bool ack_paced = ack_waits &&
            ack_blocks * 100u >= ack_waits * SMID_JPEG_QUALITY_ACK_PACED_BLOCK_PCT;
    if (ack_paced) {
        q->ack_paced_hold_windows = SMID_JPEG_QUALITY_ACK_PACED_HOLD_WINDOWS;
    } else if (q->ack_paced_hold_windows > 0) {
        q->ack_paced_hold_windows--;
    }
    bool recently_ack_paced = ack_paced || q->ack_paced_hold_windows > 0;
    bool ack_limited_with_wire_headroom = recently_ack_paced && wire_below_target;
    uint64_t frame_budget_us = elapsed_us / observed_target_frames;
    if (!frame_budget_us) {
        frame_budget_us = 1;
    }
    uint64_t avg_build_wall_us = frames ? build_wall_us / frames : 0;
    uint64_t avg_tx_wait_us = frames ? tx_wait_us / frames : 0;
    uint64_t avg_frame_us = frames ? frame_us / frames : 0;
    bool encoder_pressure = delivery_pressure && !ack_limited_with_wire_headroom &&
            avg_build_wall_us * 100u >= frame_budget_us *
                    SMID_JPEG_QUALITY_ENCODER_BUDGET_PCT &&
            avg_build_wall_us >= avg_tx_wait_us;
    bool service_pressure = delivery_pressure && frames && !encoder_pressure &&
            (avg_frame_us * 100u >= frame_budget_us *
                    SMID_JPEG_QUALITY_SERVICE_FRAME_PCT ||
             frame_max_us * 100u >= frame_budget_us *
                    SMID_JPEG_QUALITY_SERVICE_TAIL_PCT ||
             avg_tx_wait_us * 100u >= frame_budget_us *
                    SMID_JPEG_QUALITY_SERVICE_TX_PCT);

    if (wire_pressure) {
        q->encoder_pressure_windows = 0;
        q->service_pressure_windows = 0;
        q->spare_windows = 0;
        if (q->current > q->floor) {
            q->current -= 2;
            if (q->current < q->floor) {
                q->current = q->floor;
            }
        }
    } else if (encoder_pressure) {
        q->spare_windows = 0;
        q->service_pressure_windows = 0;
        q->encoder_pressure_windows++;
        if (q->encoder_pressure_windows >= SMID_JPEG_QUALITY_DOWN_PRESSURE_WINDOWS &&
            q->current > q->floor) {
            q->current -= 2;
            if (q->current < q->floor) {
                q->current = q->floor;
            }
            q->encoder_pressure_windows = 0;
        }
    } else if (service_pressure) {
        q->spare_windows = 0;
        q->encoder_pressure_windows = 0;
        q->service_pressure_windows++;
        if (q->service_pressure_windows >= SMID_JPEG_QUALITY_SERVICE_DOWN_PRESSURE_WINDOWS &&
            q->current > q->floor) {
            q->current -= 2;
            if (q->current < q->floor) {
                q->current = q->floor;
            }
            q->service_pressure_windows = 0;
        }
    } else if (wire_spare && !delivery_pressure && q->current < q->ceiling) {
        q->encoder_pressure_windows = 0;
        q->service_pressure_windows = 0;
        q->spare_windows++;
        if (q->spare_windows >= SMID_JPEG_QUALITY_UP_GOOD_WINDOWS) {
            q->current += 2;
            if (q->current > q->ceiling) {
                q->current = q->ceiling;
            }
            q->spare_windows = 0;
        }
    } else {
        q->encoder_pressure_windows = 0;
        q->service_pressure_windows = 0;
        q->spare_windows = 0;
    }
}

static void quality_controller_note_idle(struct smid_quality_controller *q) {
    if (q->fixed) {
        return;
    }
    q->current = q->ceiling;
    q->encoder_pressure_windows = 0;
    q->service_pressure_windows = 0;
    q->spare_windows = 0;
    q->ack_paced_hold_windows = 0;
}

static bool video_frame_hash_equal(struct smid_frame_hash a, struct smid_frame_hash b) {
    return a.a == b.a && a.b == b.b;
}

static void video_retained_reset_all(struct smid_video_retained_band retained[4]) {
    memset(retained, 0, sizeof(struct smid_video_retained_band) * 4u);
}

static void video_retained_reset_stream(struct smid_video_retained_band retained[4],
                                        int stream) {
    unsigned base = (unsigned)stream * 2u;
    memset(&retained[base], 0, sizeof(struct smid_video_retained_band) * 2u);
}

static bool video_retained_band_base_matches(const struct smid_video_retained_band *band,
                                             struct smid_frame_hash current) {
    return band->count >= SMID_VIDEO_RETAINED_PERIOD && band->valid[band->next] &&
           video_frame_hash_equal(band->hash[band->next], current);
}

static void video_retained_band_note_packet(struct smid_video_retained_band *band,
                                            struct smid_frame_hash current) {
    band->hash[band->next] = current;
    band->valid[band->next] = true;
    band->next = (band->next + 1u) % SMID_VIDEO_RETAINED_PERIOD;
    if (band->count < SMID_VIDEO_RETAINED_PERIOD) {
        band->count++;
    }
}

static void video_retained_note_plan(struct smid_video_retained_band retained[4],
                                     const struct smid_video_send_plan *plan) {
    for (int i = 0; i < 4; i++) {
        uint8_t bit = (uint8_t)(1u << i);
        if ((plan->send_mask & bit) && (plan->hash_valid_mask & bit)) {
            video_retained_band_note_packet(&retained[i], plan->hash[i]);
        }
    }
}

static int video_make_send_plan(struct smid_evdi_source *evdi, uint8_t dirty,
                                uint8_t live_send_mask,
                                struct smid_video_retained_band retained[4],
                                struct smid_video_send_plan *plan) {
    memset(plan, 0, sizeof(*plan));
    dirty &= live_send_mask;
    if (!dirty) {
        return 0;
    }

    for (int stream = 0; stream < 2; stream++) {
        unsigned base = (unsigned)stream * 2u;
        uint8_t lower_bit = (uint8_t)(1u << base);
        uint8_t top_bit = (uint8_t)(1u << (base + 1u));
        uint8_t stream_mask = (uint8_t)(lower_bit | top_bit) & live_send_mask;
        uint8_t stream_dirty = dirty & stream_mask;
        if (!stream_dirty) {
            continue;
        }

        struct smid_frame_hash hashes[2];
        if (smid_evdi_source_current_band_hashes(evdi, stream, hashes)) {
            plan->send_mask |= stream_mask;
            continue;
        }
        plan->hash[base] = hashes[0];
        plan->hash[base + 1u] = hashes[1];
        plan->hash_valid_mask |= stream_mask;

        plan->send_mask |= stream_dirty;
        uint8_t companion_candidates = (uint8_t)(stream_mask & ~stream_dirty);
        for (int tile = 0; tile < 2; tile++) {
            uint8_t bit = (uint8_t)(1u << (base + (unsigned)tile));
            if (!(companion_candidates & bit)) {
                continue;
            }
            if (video_retained_band_base_matches(&retained[base + (unsigned)tile],
                                                 hashes[tile])) {
                plan->companion_mask |= bit;
            }
            plan->send_mask |= bit;
        }
    }
    plan->send_mask &= live_send_mask;
    plan->companion_mask &= plan->send_mask;
    return 0;
}

static int video_tile_init(struct smid_video_tile *t, int stream, int tile,
                           enum smid_encoder_backend backend) {
    memset(t, 0, sizeof(*t));
    t->stream = stream;
    t->tile = tile;
    t->jpeg_cap = smid_encoder_jpeg_capacity();
    if (smid_encoder_init(&t->encoder, backend)) {
        return -1;
    }
    if (smid_encoder_init_size(&t->companion_encoder, SMID_ENCODER_CPU,
                               SMID_VIDEO_COMPANION_RECT_SIZE,
                               SMID_VIDEO_COMPANION_RECT_SIZE)) {
        smid_encoder_destroy(&t->encoder);
        return -1;
    }
    t->companion_encoder_ready = true;
    for (int i = 0; i < 2; i++) {
        if (smid_frame_slot_init(&t->frame[i], (uint32_t)t->jpeg_cap)) {
            for (int j = 0; j < i; j++) {
                if (t->tx_result_ready[j]) {
                    smid_tx_result_destroy(&t->tx_result[j]);
                    t->tx_result_ready[j] = false;
                }
                smid_frame_slot_destroy(&t->frame[j]);
            }
            smid_encoder_destroy(&t->companion_encoder);
            t->companion_encoder_ready = false;
            smid_encoder_destroy(&t->encoder);
            return -1;
        }
        smid_tx_result_init(&t->tx_result[i]);
        t->tx_result_ready[i] = true;
    }
    return 0;
}

static void video_tile_destroy(struct smid_video_tile *t) {
    for (int i = 0; i < 2; i++) {
        if (t->tx_result_ready[i]) {
            smid_tx_result_destroy(&t->tx_result[i]);
        }
        smid_frame_slot_destroy(&t->frame[i]);
    }
    if (t->companion_encoder_ready) {
        smid_encoder_destroy(&t->companion_encoder);
    }
    smid_encoder_destroy(&t->encoder);
    memset(t, 0, sizeof(*t));
}

struct smid_evdi_region_encoder {
    struct smid_encoder *encoder;
    struct smid_frame_rect rect;
    int quality;
    uint8_t *jpeg_out;
    unsigned long jpeg_cap;
    unsigned long *jpeg_len;
};

static int encode_evdi_bgrx_region(void *ctx, const uint8_t *bgrx,
                                   int width, int height, int stride) {
    struct smid_evdi_region_encoder *e = ctx;
    return smid_encoder_encode_bgrx_region(e->encoder, bgrx, width, height, stride,
                                           e->rect.x, e->rect.y, e->rect.w, e->rect.h,
                                           e->quality, e->jpeg_out, e->jpeg_cap,
                                           e->jpeg_len);
}

static int video_tile_build_evdi(struct smid_video_tile *t, int slot,
                                 struct smid_evdi_source *evdi,
                                 struct smid_cnm *cnm, uint32_t seq, int quality) {
    if (slot < 0 || slot > 1) {
        return -1;
    }
    uint8_t *jpeg_out = smid_frame_slot_jpeg_data(&t->frame[slot]);
    if (!jpeg_out) {
        return -1;
    }
    t->jpeg_len = t->jpeg_cap;
    struct smid_frame_rect rect = smid_frame_rect_make(t->tile, 0,
            t->tile ? 0 : SMID_LOWER_TILE_Y0, SMID_WIDTH, SMID_TILE_H);
    struct smid_evdi_region_encoder region = {
        .encoder = &t->encoder,
        .rect = rect,
        .quality = quality,
        .jpeg_out = jpeg_out,
        .jpeg_cap = t->jpeg_cap,
        .jpeg_len = &t->jpeg_len,
    };
    int rc = smid_evdi_source_with_bgrx_frame(evdi, t->stream,
                                              encode_evdi_bgrx_region, &region);
    if (rc) {
        if (t->encoder.backend == SMID_ENCODER_DIRECT_VAAPI) {
            smid_logf("direct-vaapi userptr EVDI encode failed stream=%d tile=%d\n",
                      t->stream, t->tile);
        } else {
            smid_logf("cpu bgrx EVDI encode failed stream=%d tile=%d\n",
                      t->stream, t->tile);
        }
        return -1;
    }
    return smid_build_frame_packet(cnm, &t->frame[slot], t->stream, t->tile, seq,
                                   jpeg_out, (uint32_t)t->jpeg_len);
}

static int video_tile_build_evdi_companion(struct smid_video_tile *t, int slot,
                                           struct smid_evdi_source *evdi,
                                           struct smid_cnm *cnm, uint32_t seq,
                                           int quality) {
    if (slot < 0 || slot > 1) {
        return -1;
    }
    uint8_t *jpeg_out = smid_frame_slot_jpeg_data(&t->frame[slot]);
    if (!jpeg_out) {
        return -1;
    }
    int band_y0 = t->tile ? 0 : SMID_LOWER_TILE_Y0;
    struct smid_frame_rect rect = smid_frame_rect_make(t->tile,
            SMID_VIDEO_COMPANION_RECT_X,
            (uint16_t)(band_y0 + SMID_VIDEO_COMPANION_RECT_Y_IN_BAND),
            SMID_VIDEO_COMPANION_RECT_SIZE,
            SMID_VIDEO_COMPANION_RECT_SIZE);
    t->jpeg_len = t->jpeg_cap;
    struct smid_evdi_region_encoder region = {
        .encoder = &t->companion_encoder,
        .rect = rect,
        .quality = quality,
        .jpeg_out = jpeg_out,
        .jpeg_cap = t->jpeg_cap,
        .jpeg_len = &t->jpeg_len,
    };
    int rc = smid_evdi_source_with_bgrx_frame(evdi, t->stream,
                                              encode_evdi_bgrx_region, &region);
    if (rc) {
        smid_logf("companion EVDI encode failed stream=%d tile=%d\n",
                  t->stream, t->tile);
        return -1;
    }
    return smid_build_frame_packet_rect(cnm, &t->frame[slot], t->stream, t->tile,
                                        seq, rect, jpeg_out, (uint32_t)t->jpeg_len);
}

static void *video_build_worker_main(void *arg) {
    struct smid_video_build_worker *w = arg;
    pthread_mutex_lock(&w->lock);
    for (;;) {
        while (!w->stop && !w->has_job) {
            pthread_cond_wait(&w->cond, &w->lock);
        }
        if (w->stop) {
            break;
        }
        struct smid_video_tile *tile = w->tile;
        struct smid_evdi_source *evdi = w->evdi;
        uint32_t seq = w->seq;
        int quality = w->quality;
        int slot = w->slot;
        bool companion = w->companion;
        w->has_job = false;
        pthread_mutex_unlock(&w->lock);

        int rc = companion ?
                video_tile_build_evdi_companion(tile, slot, evdi, &w->cnm, seq, quality) :
                video_tile_build_evdi(tile, slot, evdi, &w->cnm, seq, quality);

        pthread_mutex_lock(&w->lock);
        w->rc = rc;
        w->done = true;
        pthread_cond_broadcast(&w->cond);
    }
    pthread_mutex_unlock(&w->lock);
    return NULL;
}

static int video_build_worker_init(struct smid_video_build_worker *w,
                                   struct smid_video_tile *tile) {
    memset(w, 0, sizeof(*w));
    w->tile = tile;
    if (pthread_mutex_init(&w->lock, NULL)) {
        return -1;
    }
    if (pthread_cond_init(&w->cond, NULL)) {
        pthread_mutex_destroy(&w->lock);
        return -1;
    }
    if (smid_cnm_init(&w->cnm)) {
        pthread_cond_destroy(&w->cond);
        pthread_mutex_destroy(&w->lock);
        return -1;
    }
    w->cnm_ready = true;
    w->initialized = true;
    if (pthread_create(&w->thread, NULL, video_build_worker_main, w)) {
        smid_cnm_destroy(&w->cnm);
        pthread_cond_destroy(&w->cond);
        pthread_mutex_destroy(&w->lock);
        memset(w, 0, sizeof(*w));
        return -1;
    }
    w->started = true;
    return 0;
}

static void video_build_worker_destroy(struct smid_video_build_worker *w) {
    if (w->started) {
        pthread_mutex_lock(&w->lock);
        w->stop = true;
        pthread_cond_broadcast(&w->cond);
        pthread_mutex_unlock(&w->lock);
        pthread_join(w->thread, NULL);
        w->started = false;
    }
    if (w->cnm_ready) {
        smid_cnm_destroy(&w->cnm);
        w->cnm_ready = false;
    }
    if (w->initialized) {
        pthread_cond_destroy(&w->cond);
        pthread_mutex_destroy(&w->lock);
    }
    memset(w, 0, sizeof(*w));
}

static void video_build_worker_submit(struct smid_video_build_worker *w,
                                      struct smid_evdi_source *evdi,
                                      uint32_t seq, int quality, int slot,
                                      bool companion) {
    pthread_mutex_lock(&w->lock);
    w->evdi = evdi;
    w->seq = seq;
    w->quality = quality;
    w->slot = slot;
    w->companion = companion;
    w->done = false;
    w->rc = 0;
    w->has_job = true;
    pthread_cond_signal(&w->cond);
    pthread_mutex_unlock(&w->lock);
}

static int video_build_worker_wait(struct smid_video_build_worker *w) {
    pthread_mutex_lock(&w->lock);
    while (!w->done) {
        pthread_cond_wait(&w->cond, &w->lock);
    }
    int rc = w->rc;
    pthread_mutex_unlock(&w->lock);
    return rc;
}

static int video_build_send_mask(struct smid_video_build_worker workers[4],
                                 struct smid_evdi_source *evdi,
                                 uint8_t send_mask, uint8_t companion_mask,
                                 const uint32_t seq[2],
                                 int quality, int slot,
                                 uint64_t *build_wall_us) {
    uint64_t wall_start_us = smid_mono_us();
    int rc = 0;

    for (int i = 0; i < 4; i++) {
        uint8_t bit = (uint8_t)(1u << i);
        if (send_mask & bit) {
            video_build_worker_submit(&workers[i], evdi, seq[workers[i].tile->stream],
                                      quality, slot,
                                      (companion_mask & bit) != 0);
        }
    }
    for (int i = 0; i < 4; i++) {
        uint8_t bit = (uint8_t)(1u << i);
        if (!(send_mask & bit)) {
            continue;
        }
        if (video_build_worker_wait(&workers[i])) {
            rc = -1;
            break;
        }
    }

    if (build_wall_us) {
        *build_wall_us = smid_mono_us() - wall_start_us;
    }
    return rc;
}

static uint64_t video_timing_ewma(uint64_t old_value, uint64_t sample) {
    if (!sample) {
        return old_value;
    }
    if (!old_value) {
        return sample;
    }
    return (old_value * 7u + sample + 4u) / 8u;
}

static bool video_inflight_done(struct smid_video_tile tiles[4],
                                const struct smid_video_inflight *inflight) {
    if (!inflight->active) {
        return true;
    }
    for (int j = 0; j < inflight->submitted_n; j++) {
        int tile_index = inflight->submitted[j];
        if (!smid_tx_result_poll(&tiles[tile_index].tx_result[inflight->slot],
                                 NULL, NULL)) {
            return false;
        }
    }
    return true;
}

static uint64_t video_jit_wait_before_build(struct smid_video_tile tiles[4],
                                            const struct smid_video_inflight *inflight,
                                            const struct smid_video_timing_estimate *timing) {
    if (!inflight->active || !timing->build_wall_us || !timing->tx_wait_us ||
        video_inflight_done(tiles, inflight)) {
        return 0;
    }
    uint64_t now_us = smid_mono_us();
    uint64_t build_lead_us = timing->build_wall_us + SMID_VIDEO_JIT_MARGIN_US;
    uint64_t expected_ready_us = inflight->submit_us + timing->tx_wait_us;
    if (expected_ready_us <= now_us + build_lead_us + SMID_VIDEO_JIT_MIN_WAIT_US) {
        return 0;
    }

    uint64_t target_start_us = expected_ready_us - build_lead_us;
    uint64_t start_us = now_us;
    while (!stop_requested && smid_mono_us() < target_start_us &&
           !video_inflight_done(tiles, inflight)) {
        now_us = smid_mono_us();
        uint64_t sleep_until_us = target_start_us;
        if (sleep_until_us > now_us + SMID_VIDEO_JIT_POLL_US) {
            sleep_until_us = now_us + SMID_VIDEO_JIT_POLL_US;
        }
        smid_sleep_until_us(sleep_until_us);
    }
    return smid_mono_us() - start_us;
}

static int video_inflight_wait(struct smid_video_tile tiles[4],
                               struct smid_video_inflight *inflight,
                               uint64_t *stat_frames,
                               uint64_t stat_stream_frames[2],
                               uint64_t *stat_build_wall_us,
                               uint64_t *stat_wire_bytes,
                               uint64_t *stat_tx_wait_us,
                               uint64_t *stat_frame_us,
                               uint64_t *stat_frame_max_us,
                               uint32_t last_stream_seq[2],
                               uint64_t *last_quality_frame_us,
                               struct smid_video_timing_estimate *timing) {
    if (!inflight->active) {
        return 0;
    }
    int rc = 0;
    uint64_t complete_us = 0;
    for (int j = 0; j < inflight->submitted_n; j++) {
        int tile_index = inflight->submitted[j];
        uint64_t done_us = 0;
        int wait_rc = smid_tx_result_wait_done_us(
                &tiles[tile_index].tx_result[inflight->slot], &done_us);
        if (done_us > complete_us) {
            complete_us = done_us;
        }
        if (wait_rc && !rc) {
            rc = wait_rc;
        }
    }
    uint64_t now_us = smid_mono_us();
    if (!complete_us) {
        complete_us = now_us;
    }
    *stat_build_wall_us += inflight->build_wall_us;
    *stat_wire_bytes += inflight->wire_bytes;
    uint64_t tx_wait_us = complete_us - inflight->submit_us;
    *stat_tx_wait_us += tx_wait_us;
    uint64_t frame_elapsed_us = complete_us - inflight->frame_start_us;
    *stat_frame_us += frame_elapsed_us;
    if (frame_elapsed_us > *stat_frame_max_us) {
        *stat_frame_max_us = frame_elapsed_us;
    }
    *last_quality_frame_us = complete_us;
    (*stat_frames)++;
    if (inflight->send_mask & 0x3u) {
        stat_stream_frames[0]++;
        last_stream_seq[0] = inflight->seq[0];
    }
    if (inflight->send_mask & 0xcu) {
        stat_stream_frames[1]++;
        last_stream_seq[1] = inflight->seq[1];
    }
    if (timing) {
        timing->tx_wait_us = video_timing_ewma(timing->tx_wait_us, tx_wait_us);
        timing->build_wall_us = video_timing_ewma(timing->build_wall_us,
                                                  inflight->build_wall_us);
    }
    inflight->active = false;
    return rc;
}

static bool usb_video_error_recoverable(int rc) {
    switch (rc) {
    case LIBUSB_ERROR_IO:
    case LIBUSB_ERROR_NO_DEVICE:
    case LIBUSB_ERROR_PIPE:
    case LIBUSB_ERROR_OVERFLOW:
    case LIBUSB_ERROR_TIMEOUT:
        return true;
    default:
        return false;
    }
}

static int video_recover_usb_session(int rc,
                                     struct smid_evdi_source *evdi,
                                     struct smid_usb *usb,
                                     struct smid_heartbeat *heartbeat,
                                     const struct smid_rawcmd *init,
                                     size_t init_n,
                                     uint32_t seq[2],
                                     uint32_t last_stream_seq[2],
                                     uint8_t *dirty,
                                     uint8_t live_send_mask,
                                     struct smid_video_inflight *inflight,
                                     struct smid_video_retained_band retained[4],
                                     bool *ack_gate_bypassed,
                                     uint32_t *ack_gate_bypass_ack,
                                     struct smid_video_timing_estimate *timing) {
    if (!usb_video_error_recoverable(rc) || stop_requested) {
        return rc;
    }

    smid_logf("usb session recover: rc=%s\n", libusb_error_name(rc));

    unsigned attempt = 0;
    while (!stop_requested) {
        attempt++;
        smid_evdi_source_set_usb(evdi, NULL);
        smid_heartbeat_stop(heartbeat);

        int light_rc = smid_usb_reconnect(usb);
        if (!light_rc) {
            light_rc = smid_usb_send_cmds(usb, init, init_n, "semantic-reinit");
        }
        if (!light_rc) {
            light_rc = smid_heartbeat_start(heartbeat, &usb->tx);
        }
        if (!light_rc) {
            smid_logf("usb session recovered via endpoint reconnect attempt=%u\n",
                    attempt);
            goto recovered;
        }

        smid_logf("usb endpoint reconnect attempt=%u failed rc=%s; trying full reset\n",
                attempt, libusb_error_name(light_rc));
        smid_heartbeat_stop(heartbeat);
        smid_usb_close(usb);

        int full_rc = smid_usb_open(usb);
        if (!full_rc) {
            full_rc = smid_usb_start_io(usb);
        }
        if (!full_rc) {
            full_rc = smid_usb_send_cmds(usb, init, init_n, "semantic-reinit");
        }
        if (!full_rc) {
            full_rc = smid_heartbeat_start(heartbeat, &usb->tx);
        }
        if (!full_rc) {
            smid_logf("usb session recovered via full reset attempt=%u\n", attempt);
            goto recovered;
        }

        smid_logf("usb full reset recover attempt=%u failed rc=%s; retrying\n",
                attempt, libusb_error_name(full_rc));
        smid_heartbeat_stop(heartbeat);
        smid_usb_close(usb);
        smid_sleep_until_us(smid_mono_us() + 250000u);
    }

    return -1;

recovered:
    smid_evdi_source_set_usb(evdi, usb);
    (void)smid_evdi_source_poke_cursor_state(evdi);

    seq[0] = 2;
    seq[1] = 2;
    last_stream_seq[0] = 0;
    last_stream_seq[1] = 0;
    *dirty = live_send_mask;
    inflight->active = false;
    *ack_gate_bypassed = false;
    *ack_gate_bypass_ack = 0;
    timing->build_wall_us = SMID_VIDEO_JIT_DEFAULT_BUILD_WALL_US;
    timing->tx_wait_us = SMID_VIDEO_JIT_DEFAULT_TX_WAIT_US;
    smid_usb_ack_clear(usb);
    video_retained_reset_all(retained);
    smid_logf("usb session recovered; forcing full refresh\n");
    return 0;
}

static void video_skip_zero_frame_indexes(uint32_t seq[2],
                                          uint32_t last_stream_seq[2],
                                          uint8_t *dirty,
                                          uint8_t live_send_mask,
                                          struct smid_video_retained_band retained[4],
                                          bool *ack_gate_bypassed,
                                          uint32_t *ack_gate_bypass_ack) {
    for (int stream = 0; stream < 2; stream++) {
        if (seq[stream] != 0) {
            continue;
        }
        uint8_t stream_mask = (uint8_t)(0x3u << ((unsigned)stream * 2u)) & live_send_mask;
        if (!stream_mask) {
            continue;
        }
        seq[stream] = 1;
        last_stream_seq[stream] = 0;
        *dirty |= stream_mask;
        video_retained_reset_stream(retained, stream);
        if (stream == 0) {
            *ack_gate_bypassed = false;
            *ack_gate_bypass_ack = 0;
        }
        smid_logf("sequence wrap: stream=%d skipped frame index zero; next seq=%u\n",
                  stream, seq[stream]);
    }
}

static bool video_sequence_wrapped(const uint32_t seq[2], uint8_t live_send_mask) {
    for (int stream = 0; stream < 2; stream++) {
        uint8_t stream_mask = (uint8_t)(0x3u << ((unsigned)stream * 2u)) & live_send_mask;
        if (stream_mask && seq[stream] == 0) {
            return true;
        }
    }
    return false;
}

static void usage(const char *argv0) {
    fprintf(stderr, "usage: %s [--seconds N] [--fps N] [--ack-window N] [--jpeg-quality N] [--jpeg-fixed-quality] [--jpeg-target-mib-s N] [--encoder cpu|direct-vaapi] [--evdi-streams N]\n", argv0);
}

int main(int argc, char **argv) {
    int evdi_streams = 2;
    enum smid_encoder_backend encoder = SMID_ENCODER_DIRECT_VAAPI;
    int seconds = 0;
    int fps = 60;
    int ack_window = 1;
    int jpeg_quality = 98;
    bool jpeg_fixed_quality = false;
    int jpeg_target_mib_s = SMID_QUALITY_TARGET_USB_MIB_S_DEFAULT;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--evdi-streams") && i + 1 < argc) {
            evdi_streams = atoi(argv[++i]);
            if (evdi_streams < 1) {
                evdi_streams = 1;
            } else if (evdi_streams > 2) {
                evdi_streams = 2;
            }
        } else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) {
            seconds = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--fps") && i + 1 < argc) {
            fps = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--ack-window") && i + 1 < argc) {
            ack_window = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--jpeg-quality") && i + 1 < argc) {
            jpeg_quality = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--jpeg-fixed-quality")) {
            jpeg_fixed_quality = true;
        } else if (!strcmp(argv[i], "--jpeg-target-mib-s") && i + 1 < argc) {
            jpeg_target_mib_s = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--encoder") && i + 1 < argc) {
            if (smid_encoder_parse_backend(argv[++i], &encoder)) {
                usage(argv[0]);
                return 2;
            }
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    if (fps < 1) {
        fps = 1;
    } else if (fps > 60) {
        fps = 60;
    }
    if (ack_window < 0) {
        ack_window = 0;
    }
    if (jpeg_quality < 1) {
        jpeg_quality = 1;
    } else if (jpeg_quality > 100) {
        jpeg_quality = 100;
    }
    if (jpeg_target_mib_s < 1) {
        usage(argv[0]);
        return 1;
    }

    if (smid_log_start()) {
        fprintf(stderr, "failed to start logger\n");
        return 1;
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    smid_logf("smid: starting\n");

    {
        struct smid_usb usb;
        if (smid_usb_open(&usb)) {
            smid_log_stop();
            return 1;
        }
        if (smid_usb_start_io(&usb)) {
            smid_usb_close(&usb);
            smid_log_stop();
            return 1;
        }
        struct smid_heartbeat heartbeat;
        if (smid_heartbeat_start(&heartbeat, &usb.tx)) {
            smid_usb_close(&usb);
            smid_log_stop();
            return 1;
        }
        struct smid_rawcmd init[SMID_RAWCMD_CAP];
        size_t init_n = 0;
        smid_make_semantic_init(init, &init_n);
        int rc = smid_usb_send_cmds(&usb, init, init_n, "init");
        if (!rc) {
            struct smid_evdi_source *evdi = NULL;
            struct smid_video_tile tiles[4];
            struct smid_video_build_worker workers[4];
            memset(tiles, 0, sizeof(tiles));
            memset(workers, 0, sizeof(workers));

            if (!rc && smid_evdi_source_start(&evdi, evdi_streams, true, &usb)) {
                rc = -1;
            }
            for (int i = 0; i < 4 && !rc; i++) {
                rc = video_tile_init(&tiles[i], i / 2, i % 2, encoder);
            }
            for (int i = 0; i < 4 && !rc; i++) {
                rc = video_build_worker_init(&workers[i], &tiles[i]);
            }

            uint32_t seq[2] = { 2, 2 };
            uint32_t last_stream_seq[2] = { 0, 0 };
            uint64_t start_us = smid_mono_us();
            uint64_t next_stats_us = start_us + 1000000u;
            uint64_t stats_start_us = start_us;
            uint64_t end_us = seconds > 0 ? start_us + (uint64_t)seconds * 1000000u : 0;
            uint8_t dirty = evdi_streams > 1 ? 0xfu : 0x3u;
            uint8_t live_send_mask = evdi_streams > 1 ? 0xfu : 0x3u;
            uint64_t stat_frames = 0;
            uint64_t stat_stream_frames[2] = {0, 0};
            uint64_t stat_wire_bytes = 0;
            uint64_t stat_build_wall_us = 0;
            uint64_t stat_tx_wait_us = 0;
            uint64_t stat_frame_us = 0;
            uint64_t stat_frame_max_us = 0;
            uint64_t stat_ack_checks = 0;
            uint64_t stat_ack_misses = 0;
            struct smid_quality_controller quality_ctl;
            quality_controller_init(&quality_ctl, jpeg_quality, fps, jpeg_target_mib_s,
                                    jpeg_fixed_quality);
            uint64_t last_quality_frame_us = start_us;
            struct smid_video_inflight inflight;
            memset(&inflight, 0, sizeof(inflight));
            struct smid_video_retained_band retained[4];
            video_retained_reset_all(retained);
            bool ack_gate_bypassed = false;
            uint32_t ack_gate_bypass_ack = 0;
            struct smid_video_timing_estimate timing = {
                .build_wall_us = SMID_VIDEO_JIT_DEFAULT_BUILD_WALL_US,
                .tx_wait_us = SMID_VIDEO_JIT_DEFAULT_TX_WAIT_US,
            };
            int pipe_slot = 0;

            video_skip_zero_frame_indexes(seq, last_stream_seq, &dirty,
                                          live_send_mask, retained,
                                          &ack_gate_bypassed,
                                          &ack_gate_bypass_ack);
            if (!rc) {
                smid_logf("streaming evdi video %s at %dfps encoder=%s q-ceiling=%d q-floor=%d q-mode=%s q-target=%dMiB/s dirty-check=hash-bands cursor-events=1 ack-window=%d\n",
                        seconds > 0 ? "for requested duration" : "until interrupted", fps,
                        smid_encoder_backend_name(tiles[0].encoder.backend), jpeg_quality,
                        quality_ctl.floor, jpeg_fixed_quality ? "fixed" : "adaptive",
                        jpeg_target_mib_s, ack_window);
            }
            while (!rc && !stop_requested && (!end_us || smid_mono_us() < end_us)) {
                dirty |= smid_evdi_source_consume_dirty(evdi);
                uint64_t now_us = smid_mono_us();
                if (!dirty && now_us - last_quality_frame_us >= SMID_JPEG_QUALITY_IDLE_RESET_US) {
                    quality_controller_note_idle(&quality_ctl);
                }
                if (now_us >= next_stats_us) {
                    struct smid_evdi_stats st;
                    smid_evdi_source_take_stats(evdi, &st);
                    uint64_t stat_elapsed_us = now_us - stats_start_us;
                    quality_controller_update(&quality_ctl, stat_elapsed_us, stat_frames,
                                              stat_stream_frames, st.stream_dirty,
                                              evdi_streams, st.dirty, stat_wire_bytes,
                                              stat_ack_checks, stat_ack_misses,
                                              stat_build_wall_us, stat_tx_wait_us,
                                              stat_frame_us, stat_frame_max_us);
                    stat_frames = 0;
                    stat_stream_frames[0] = 0;
                    stat_stream_frames[1] = 0;
                    stat_wire_bytes = 0;
                    stat_build_wall_us = 0;
                    stat_tx_wait_us = 0;
                    stat_frame_us = 0;
                    stat_frame_max_us = 0;
                    stat_ack_checks = 0;
                    stat_ack_misses = 0;
                    stats_start_us = now_us;
                    do {
                        next_stats_us += 1000000u;
                    } while (next_stats_us <= now_us);
                }
                int rx_rc = smid_usb_take_rx_error(&usb);
                if (rx_rc) {
                    rc = video_recover_usb_session(rx_rc, evdi, &usb, &heartbeat,
                            init, init_n, seq, last_stream_seq, &dirty,
                            live_send_mask, &inflight, retained,
                            &ack_gate_bypassed, &ack_gate_bypass_ack,
                            &timing);
                    if (!rc) {
                        continue;
                    }
                    break;
                }
                if (!dirty) {
                    if (inflight.active) {
                        rc = video_inflight_wait(tiles, &inflight, &stat_frames,
                                stat_stream_frames, &stat_build_wall_us,
                                &stat_wire_bytes, &stat_tx_wait_us,
                                &stat_frame_us, &stat_frame_max_us, last_stream_seq,
                                &last_quality_frame_us, &timing);
                        if (rc) {
                            rc = video_recover_usb_session(rc, evdi, &usb, &heartbeat,
                                    init, init_n, seq, last_stream_seq, &dirty,
                                    live_send_mask, &inflight, retained,
                                    &ack_gate_bypassed, &ack_gate_bypass_ack,
                                    &timing);
                        }
                        continue;
                    }
                    uint64_t gen = smid_evdi_source_damage_snapshot(evdi);
                    uint64_t wait_us = 1000000u;
                    if (end_us && now_us + wait_us > end_us) {
                        wait_us = end_us > now_us ? end_us - now_us : 0;
                    }
                    smid_evdi_source_wait_damage(evdi, gen, wait_us);
                    continue;
                }

                if (video_jit_wait_before_build(tiles, &inflight, &timing)) {
                    dirty |= smid_evdi_source_consume_dirty(evdi);
                }

                uint64_t frame_start_us = smid_mono_us();
                int frame_quality = quality_controller_current(&quality_ctl);
                struct smid_video_send_plan plan;
                rc = video_make_send_plan(evdi, dirty, live_send_mask, retained, &plan);
                uint8_t send_mask = plan.send_mask;
                int build_slot = pipe_slot;
                int submitted[4];
                int submitted_n = 0;
                uint64_t wire_total_bytes = 0;
                uint64_t late_wait_us = 0;
                bool ack_uncertain = false;

                uint64_t build_wall_us = 0;
                if (!rc) {
                    rc = video_build_send_mask(workers, evdi, send_mask, plan.companion_mask,
                                               seq, frame_quality, build_slot,
                                               &build_wall_us);
                }
                if (!rc && inflight.active) {
                    uint64_t wait_start_us = smid_mono_us();
                    rc = video_inflight_wait(tiles, &inflight, &stat_frames,
                            stat_stream_frames, &stat_build_wall_us,
                            &stat_wire_bytes, &stat_tx_wait_us,
                            &stat_frame_us, &stat_frame_max_us, last_stream_seq,
                            &last_quality_frame_us, &timing);
                    late_wait_us += smid_mono_us() - wait_start_us;
                    if (rc) {
                        rc = video_recover_usb_session(rc, evdi, &usb, &heartbeat,
                                init, init_n, seq, last_stream_seq, &dirty,
                                live_send_mask, &inflight, retained,
                                &ack_gate_bypassed, &ack_gate_bypass_ack,
                                &timing);
                        if (!rc) {
                            continue;
                        }
                    }
                }
                if (!rc && (send_mask & 0x3u) && last_stream_seq[0]) {
                    uint32_t ack_a = 0;
                    bool ack_a_seen = false;
                    unsigned lag = 0;
                    stat_ack_checks++;
                    bool ack_ready = smid_usb_ack_a_within_window(&usb, last_stream_seq[0],
                            (unsigned)ack_window, &ack_a, &ack_a_seen, &lag);
                    (void)lag;
                    if (!ack_ready) {
                        stat_ack_misses++;
                        if (!ack_gate_bypassed || ack_gate_bypass_ack != ack_a) {
                            ack_gate_bypassed = true;
                            ack_gate_bypass_ack = ack_a;
                        }
                        last_stream_seq[0] = 0;
                        ack_uncertain = true;
                        video_retained_reset_all(retained);
                    } else if (ack_gate_bypassed) {
                        if (ack_a_seen && ack_a != ack_gate_bypass_ack) {
                            ack_gate_bypassed = false;
                            if (ack_uncertain) {
                                last_stream_seq[0] = 0;
                                video_retained_reset_all(retained);
                            }
                        } else {
                            last_stream_seq[0] = 0;
                            ack_uncertain = true;
                            video_retained_reset_all(retained);
                        }
                    }
                }
                if (!rc && (ack_uncertain ||
                            late_wait_us >= SMID_VIDEO_LATE_REBUILD_WAIT_US)) {
                    uint8_t late_dirty = smid_evdi_source_consume_dirty(evdi) & live_send_mask;
                    if (late_dirty || ack_uncertain) {
                        dirty |= late_dirty;
                        rc = video_make_send_plan(evdi, dirty, live_send_mask,
                                                  retained, &plan);
                        send_mask = plan.send_mask;
                        frame_start_us = smid_mono_us();
                        if (!rc) {
                            rc = video_build_send_mask(workers, evdi, send_mask,
                                                       plan.companion_mask, seq,
                                                       frame_quality, build_slot,
                                                       &build_wall_us);
                        }
                    }
                }
                for (int i = 0; i < 4 && !rc; i++) {
                    uint8_t bit = (uint8_t)(1u << i);
                    if (!(send_mask & bit)) {
                        continue;
                    }
                    smid_tx_result_reset(&tiles[i].tx_result[build_slot]);
                    struct smid_tx_item item = {
                        .priority = SMID_TX_PRIO_VIDEO,
                        .endpoint = SMID_EP_BULK_OUT,
                        .xfer = SMID_USB_XFER_BULK,
                        .timeout_ms = 2000,
                        .data = tiles[i].frame[build_slot].packet,
                        .len = tiles[i].frame[build_slot].wire_len,
                        .result = &tiles[i].tx_result[build_slot],
                        .name = "evdi-frame",
                    };
                    rc = smid_transport_submit(&usb.tx, &item);
                    if (!rc) {
                        submitted[submitted_n++] = i;
                        wire_total_bytes += tiles[i].frame[build_slot].wire_len;
                    }
                }
                if (submitted_n > 0) {
                    if (!rc && !ack_uncertain) {
                        video_retained_note_plan(retained, &plan);
                    } else if (!rc) {
                        video_retained_reset_all(retained);
                    }
                    inflight.active = true;
                    inflight.slot = build_slot;
                    memcpy(inflight.submitted, submitted,
                           (size_t)submitted_n * sizeof(submitted[0]));
                    inflight.submitted_n = submitted_n;
                    inflight.send_mask = send_mask;
                    memcpy(inflight.seq, seq, sizeof(inflight.seq));
                    inflight.frame_start_us = frame_start_us;
                    inflight.submit_us = smid_mono_us();
                    inflight.build_wall_us = build_wall_us;
                    inflight.wire_bytes = wire_total_bytes;
                    pipe_slot ^= 1;
                }
                if (rc) {
                    rc = video_recover_usb_session(rc, evdi, &usb, &heartbeat,
                            init, init_n, seq, last_stream_seq, &dirty,
                            live_send_mask, &inflight, retained,
                            &ack_gate_bypassed, &ack_gate_bypass_ack,
                            &timing);
                    if (!rc) {
                        continue;
                    }
                    break;
                }
                dirty = 0;
                for (int stream = 0; stream < 2; stream++) {
                    uint8_t stream_mask = (uint8_t)(0x3u << ((unsigned)stream * 2u)) &
                            live_send_mask;
                    if (send_mask & stream_mask) {
                        seq[stream]++;
                    }
                }
                if (video_sequence_wrapped(seq, live_send_mask)) {
                    uint32_t ack_a = 0;
                    uint32_t ack_b = 0;
                    bool ack_a_seen = false;
                    bool ack_b_seen = false;
                    smid_usb_ack_snapshot(&usb, &ack_a, &ack_b, &ack_a_seen, &ack_b_seen);
                    smid_logf("sequence wrap: seq0=%u seq1=%u ackA=%u%s ackB=%u%s; reconnecting USB session\n",
                              seq[0], seq[1],
                              ack_a, ack_a_seen ? "" : "?",
                              ack_b, ack_b_seen ? "" : "?");
                    rc = video_recover_usb_session(LIBUSB_ERROR_PIPE, evdi, &usb,
                            &heartbeat, init, init_n, seq, last_stream_seq, &dirty,
                            live_send_mask, &inflight, retained,
                            &ack_gate_bypassed, &ack_gate_bypass_ack,
                            &timing);
                    if (!rc) {
                        continue;
                    }
                    break;
                }
            }
            if (inflight.active) {
                int wait_rc = video_inflight_wait(tiles, &inflight, &stat_frames,
                        stat_stream_frames, &stat_build_wall_us,
                        &stat_wire_bytes, &stat_tx_wait_us,
                        &stat_frame_us, &stat_frame_max_us, last_stream_seq,
                        &last_quality_frame_us, &timing);
                if (wait_rc && !rc) {
                    rc = wait_rc;
                }
            }
            for (int i = 0; i < 4; i++) {
                video_build_worker_destroy(&workers[i]);
                video_tile_destroy(&tiles[i]);
            }
            smid_evdi_source_stop(evdi);
        }
        smid_heartbeat_stop(&heartbeat);
        smid_usb_close(&usb);
        if (rc) {
            smid_log_stop();
            return 1;
        }
    }

    smid_logf("smid: exiting\n");
    smid_log_stop();
    return 0;
}
