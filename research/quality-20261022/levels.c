/* Offline test of production encoder levels. No USB or EVDI code is linked. */
#include "encoder.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint8_t levels[] = {
    0, 1, 2, 4, 8, 15, 16, 17, 32, 64, 128, 192, 224, 230,
    234, 235, 236, 238, 240, 242, 244, 246, 248, 250, 252, 253, 254, 255
};

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    enum smid_encoder_backend backend;
    if (smid_encoder_parse_backend(argv[1], &backend)) return 2;
    uint8_t *pixels = NULL;
    if (posix_memalign((void **)&pixels, 4096, 1920u * 1080u * 4u)) return 1;
    memset(pixels, 255, 1920u * 1080u * 4u);
    for (int y = 0; y < 1080; y++) {
        for (unsigned n = 0; n < sizeof(levels); n++) {
            for (int x = 0; x < 64; x++) {
                /* Flat patches above, one-pixel grey borders on white below. */
                int value = y % 536 < 128 || x == 31 ? levels[n] : 255;
                uint8_t *p = pixels + ((size_t)y * 1920 + n * 64 + x) * 4;
                p[0] = p[1] = p[2] = (uint8_t)value;
            }
        }
    }
    struct smid_encoder enc;
    if (smid_encoder_init(&enc, backend)) return 1;
    if (enc.backend != backend) {
        fprintf(stderr, "Requested backend fell back; not a pass\n");
        return 1;
    }
    unsigned long cap = smid_encoder_jpeg_capacity();
    uint8_t *jpeg = malloc(cap);
    if (!jpeg) return 1;
    const int qualities[] = {70, 98, 100};
    for (unsigned q = 0; q < 3; q++) {
        for (int band = 0; band < 2; band++) {
            unsigned long len = 0;
            if (smid_encoder_encode_bgrx_region(&enc, pixels, 1920, 1080, 7680,
                    0, band * 536, 1920, 544, qualities[q], jpeg, cap, &len)) return 1;
            char name[4096];
            snprintf(name, sizeof(name), "%s/%s-q%d-band%d.jpg", argv[2], argv[1], qualities[q], band);
            FILE *f = fopen(name, "wb");
            if (!f) return 1;
            if (fwrite(jpeg, 1, len, f) != len || fclose(f)) return 1;
            printf("%s %lu bytes\n", name, len);
        }
    }
    smid_encoder_destroy(&enc);
    free(jpeg);
    free(pixels);
    return 0;
}
