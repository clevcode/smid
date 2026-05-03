#ifndef SMID_PROTOCOL_H
#define SMID_PROTOCOL_H

#include "common.h"

#include <stddef.h>
#include <stdint.h>

#define SMID_MAGIC "smifalconsta"
#define SMID_WIDTH 1920
#define SMID_HEIGHT 1080
#define SMID_TILE_H 544
#define SMID_STREAM_H 1080
#define SMID_LOWER_TILE_Y0 (SMID_STREAM_H - SMID_TILE_H)

enum {
    SMID_RAWCMD_CAP = 96,
    SMID_RAWCMD_DATA_CAP = 96,
};

struct smid_rawcmd {
    const char *name;
    uint8_t endpoint;
    uint8_t xfer;
    uint32_t len;
    uint8_t wait_code;
    uint8_t data[SMID_RAWCMD_DATA_CAP];
};

void smid_put32(uint8_t *p, uint32_t v);
uint32_t smid_get32(const uint8_t *p);
void smid_make_semantic_init(struct smid_rawcmd c[SMID_RAWCMD_CAP], size_t *n);

#endif
