#ifndef SMID_CNM_H
#define SMID_CNM_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

enum {
    SMID_CNM_SIZE = 0x5000,
};

struct smid_cnm {
    bool initialized;
};

int smid_cnm_init(struct smid_cnm *cnm);
void smid_cnm_destroy(struct smid_cnm *cnm);
int smid_cnm_build_header_base(struct smid_cnm *cnm, uint8_t *jpeg_slot,
                               uint32_t encoded_len, uint8_t *cnm_out,
                               bool patch_base, uint32_t base);

#endif
