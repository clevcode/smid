#include "frame_packet.h"

#include "log.h"
#include "protocol.h"

#include <stdlib.h>
#include <string.h>

static uint32_t align_up_u32(uint32_t v, uint32_t a) {
    return (v + a - 1u) & ~(a - 1u);
}

static void put16_unaligned(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)(v >> 8);
}

uint32_t smid_frame_slot_capacity(uint32_t jpeg_cap) {
    return SMID_FRAME_HEADER_SIZE + align_up_u32(jpeg_cap, 512) + SMID_CNM_SIZE + 1u;
}

int smid_frame_slot_init(struct smid_frame_slot *slot, uint32_t jpeg_cap) {
    memset(slot, 0, sizeof(*slot));
    slot->packet_cap = smid_frame_slot_capacity(jpeg_cap);
    slot->packet = calloc(1, slot->packet_cap);
    return slot->packet ? 0 : -1;
}

void smid_frame_slot_destroy(struct smid_frame_slot *slot) {
    free(slot->packet);
    memset(slot, 0, sizeof(*slot));
}

uint8_t *smid_frame_slot_jpeg_data(struct smid_frame_slot *slot) {
    if (!slot || !slot->packet || slot->packet_cap <= SMID_FRAME_HEADER_SIZE) {
        return NULL;
    }
    return slot->packet + SMID_FRAME_HEADER_SIZE;
}

struct smid_frame_rect smid_frame_rect_make(int tile, uint16_t x, uint16_t y,
                                            uint16_t w, uint16_t h) {
    struct smid_frame_rect rect = {
        .x = x,
        .y = y,
        .w = w,
        .h = h,
        .stride_bytes = SMID_WIDTH * 4,
        .band_marker = tile ? SMID_FRAME_BAND_TOP : SMID_FRAME_BAND_LOWER,
    };
    return rect;
}

void smid_frame_header_build(uint8_t header[SMID_FRAME_HEADER_SIZE], int stream,
                             uint32_t seq, struct smid_frame_rect rect,
                             uint32_t wire_len, uint32_t encoded_len) {
    memset(header, 0, SMID_FRAME_HEADER_SIZE);
    memcpy(header + SMID_FRAME_OFF_MAGIC, SMID_MAGIC, 12);
    smid_put32(header + SMID_FRAME_OFF_WIRE_LEN, wire_len);
    smid_put32(header + SMID_FRAME_OFF_TARGET,
               stream ? 0xb0000002u : 0xa0000002u);
    put16_unaligned(header + SMID_FRAME_OFF_COMMAND, SMID_FRAME_COMMAND_IMAGE);
    put16_unaligned(header + SMID_FRAME_OFF_SEQ, (uint16_t)seq);
    put16_unaligned(header + SMID_FRAME_OFF_SEQ_HI, (uint16_t)(seq >> 16));
    header[SMID_FRAME_OFF_OPCODE] = SMID_FRAME_OPCODE_JPEG;
    put16_unaligned(header + SMID_FRAME_OFF_X, rect.x);
    put16_unaligned(header + SMID_FRAME_OFF_Y, rect.y);
    put16_unaligned(header + SMID_FRAME_OFF_W, rect.w);
    put16_unaligned(header + SMID_FRAME_OFF_H, rect.h);
    put16_unaligned(header + SMID_FRAME_OFF_STRIDE, rect.stride_bytes);
    smid_put32(header + SMID_FRAME_OFF_ENCODED_LEN, encoded_len);
    header[SMID_FRAME_OFF_BAND] = rect.band_marker;
}

uint32_t smid_frame_seq_take(uint32_t *seq, uint32_t step) {
    uint32_t current = *seq;
    *seq += step;
    return current;
}

int smid_build_frame_packet_rect(struct smid_cnm *cnm, struct smid_frame_slot *slot,
                                 int stream, int tile, uint32_t seq,
                                 struct smid_frame_rect rect,
                                 const uint8_t *jpeg, uint32_t jpeg_len) {
    return smid_build_frame_packet_rect_base(cnm, slot, stream, tile, seq, rect,
                                             jpeg, jpeg_len, true, 0);
}

int smid_build_frame_packet_rect_base(struct smid_cnm *cnm, struct smid_frame_slot *slot,
                                      int stream, int tile, uint32_t seq,
                                      struct smid_frame_rect rect,
                                      const uint8_t *jpeg, uint32_t jpeg_len,
                                      bool patch_cnm_base, uint32_t cnm_base) {
    if (!cnm || !slot || !slot->packet || !jpeg || !jpeg_len) {
        return -1;
    }
    uint32_t encoded_len = align_up_u32(jpeg_len, 512);
    uint32_t wire_len = SMID_FRAME_HEADER_SIZE + encoded_len + SMID_CNM_SIZE;
    if ((wire_len & 0x1ffu) == 0) {
        wire_len++;
    }
    if (wire_len > slot->packet_cap) {
        smid_logf("frame packet too large stream=%d tile=%d jpeg=%u wire=%u cap=%u\n",
                  stream, tile, jpeg_len, wire_len, slot->packet_cap);
        return -1;
    }

    uint8_t *pkt = slot->packet;
    smid_frame_header_build(pkt, stream, seq, rect, wire_len, encoded_len);
    if (jpeg != pkt + SMID_FRAME_HEADER_SIZE) {
        memcpy(pkt + SMID_FRAME_HEADER_SIZE, jpeg, jpeg_len);
    }
    memset(pkt + SMID_FRAME_HEADER_SIZE + jpeg_len, 0, encoded_len - jpeg_len);
    if (smid_cnm_build_header_base(cnm, pkt + SMID_FRAME_HEADER_SIZE, encoded_len,
                                   pkt + SMID_FRAME_HEADER_SIZE + encoded_len,
                                   patch_cnm_base, cnm_base)) {
        return -1;
    }
    slot->wire_len = wire_len;
    slot->encoded_len = encoded_len;
    return 0;
}

int smid_build_frame_packet(struct smid_cnm *cnm, struct smid_frame_slot *slot,
                            int stream, int tile, uint32_t seq,
                            const uint8_t *jpeg, uint32_t jpeg_len) {
    struct smid_frame_rect rect = smid_frame_rect_make(tile, 0,
            tile ? 0 : SMID_LOWER_TILE_Y0, SMID_WIDTH, SMID_TILE_H);
    return smid_build_frame_packet_rect(cnm, slot, stream, tile, seq, rect, jpeg, jpeg_len);
}
