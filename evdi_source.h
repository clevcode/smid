#ifndef SMID_EVDI_SOURCE_H
#define SMID_EVDI_SOURCE_H

#include <stdbool.h>
#include <stdint.h>

struct smid_usb;

struct smid_frame_hash {
    uint64_t a;
    uint64_t b;
};

struct smid_evdi_stats {
    uint64_t dirty;
    uint64_t stream_dirty[2];
};

struct smid_evdi_source;

typedef int (*smid_evdi_bgrx_frame_fn)(void *ctx, const uint8_t *bgrx,
                                       int width, int height, int stride);

int smid_evdi_source_start(struct smid_evdi_source **out, int streams, bool cursor_events,
                           struct smid_usb *usb);
void smid_evdi_source_stop(struct smid_evdi_source *src);
void smid_evdi_source_set_usb(struct smid_evdi_source *src, struct smid_usb *usb);
void smid_evdi_source_take_stats(struct smid_evdi_source *src, struct smid_evdi_stats *stats);
uint8_t smid_evdi_source_consume_dirty(struct smid_evdi_source *src);
uint8_t smid_evdi_source_consume_blank(struct smid_evdi_source *src);
int smid_evdi_source_current_band_hashes(struct smid_evdi_source *src, int stream,
                                         struct smid_frame_hash hashes[2]);
uint64_t smid_evdi_source_damage_snapshot(struct smid_evdi_source *src);
void smid_evdi_source_wait_damage(struct smid_evdi_source *src, uint64_t generation,
                                  uint64_t timeout_us);
void smid_evdi_source_copy_rgb_tile(struct smid_evdi_source *src, int stream, int tile,
                                    uint8_t *rgb);
void smid_evdi_source_copy_bgrx_tile_pitched(struct smid_evdi_source *src,
                                             int stream, int tile,
                                             uint8_t *bgrx, int dst_pitch);
int smid_evdi_source_with_bgrx_frame(struct smid_evdi_source *src, int stream,
                                     smid_evdi_bgrx_frame_fn fn, void *ctx);
int smid_evdi_source_poke_cursor_state(struct smid_evdi_source *src);

#endif
