#ifndef SMID_FRAME_PACKET_H
#define SMID_FRAME_PACKET_H

#include "cnm.h"

#include <stdbool.h>
#include <stdint.h>

enum {
    SMID_FRAME_HEADER_SIZE = 48,
    SMID_FRAME_OFF_MAGIC = 0,
    SMID_FRAME_OFF_WIRE_LEN = 12,
    SMID_FRAME_OFF_TARGET = 16,
    SMID_FRAME_OFF_COMMAND = 20,
    SMID_FRAME_OFF_SEQ = 22,
    SMID_FRAME_OFF_SEQ_HI = 24,
    SMID_FRAME_OFF_OPCODE = 26,
    SMID_FRAME_OFF_X = 27,
    SMID_FRAME_OFF_Y = 29,
    SMID_FRAME_OFF_W = 31,
    SMID_FRAME_OFF_H = 33,
    SMID_FRAME_OFF_STRIDE = 36,
    SMID_FRAME_OFF_ENCODED_LEN = 38,
    SMID_FRAME_OFF_BAND = 42,
    SMID_FRAME_COMMAND_IMAGE = 0x0104,
    SMID_FRAME_OPCODE_JPEG = 0xe0,
    SMID_FRAME_BAND_TOP = 0x21,
    SMID_FRAME_BAND_LOWER = 0x22,
};

struct smid_frame_slot {
    uint8_t *packet;
    uint32_t packet_cap;
    uint32_t wire_len;
    uint32_t encoded_len;
};

struct smid_frame_rect {
    uint16_t x;
    uint16_t y;
    uint16_t w;
    uint16_t h;
    uint16_t stride_bytes;
    uint8_t band_marker;
};

struct smid_frame_rect smid_frame_rect_make(int tile, uint16_t x, uint16_t y,
                                            uint16_t w, uint16_t h);
void smid_frame_header_build(uint8_t header[SMID_FRAME_HEADER_SIZE], int stream,
                             uint32_t seq, struct smid_frame_rect rect,
                             uint32_t wire_len, uint32_t encoded_len);
uint32_t smid_frame_seq_take(uint32_t *seq, uint32_t step);
uint32_t smid_frame_slot_capacity(uint32_t jpeg_cap);
int smid_frame_slot_init(struct smid_frame_slot *slot, uint32_t jpeg_cap);
void smid_frame_slot_destroy(struct smid_frame_slot *slot);
uint8_t *smid_frame_slot_jpeg_data(struct smid_frame_slot *slot);
int smid_build_frame_packet(struct smid_cnm *cnm, struct smid_frame_slot *slot,
                            int stream, int tile, uint32_t seq,
                            const uint8_t *jpeg, uint32_t jpeg_len);
int smid_build_frame_packet_rect(struct smid_cnm *cnm, struct smid_frame_slot *slot,
                                 int stream, int tile, uint32_t seq,
                                 struct smid_frame_rect rect,
                                 const uint8_t *jpeg, uint32_t jpeg_len);
int smid_build_frame_packet_rect_base(struct smid_cnm *cnm, struct smid_frame_slot *slot,
                                      int stream, int tile, uint32_t seq,
                                      struct smid_frame_rect rect,
                                      const uint8_t *jpeg, uint32_t jpeg_len,
                                      bool patch_cnm_base, uint32_t cnm_base);

#endif
