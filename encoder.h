#ifndef SMID_ENCODER_H
#define SMID_ENCODER_H

#include <stdint.h>

enum smid_encoder_backend {
    SMID_ENCODER_CPU,
    SMID_ENCODER_DIRECT_VAAPI,
};

struct smid_encoder {
    enum smid_encoder_backend backend;
    int width;
    int height;
    void *tj;
    unsigned long jpeg_cap;
    struct smid_direct_vaapi *direct;
};

typedef int (*smid_encoder_bgrx_writer_fn)(void *ctx, uint8_t *dst, int dst_pitch,
                                           int width, int height);

const char *smid_encoder_backend_name(enum smid_encoder_backend backend);
int smid_encoder_parse_backend(const char *name, enum smid_encoder_backend *backend);
unsigned long smid_encoder_jpeg_capacity(void);
unsigned long smid_encoder_jpeg_capacity_for_size(int width, int height);
int smid_encoder_init(struct smid_encoder *enc, enum smid_encoder_backend backend);
int smid_encoder_init_size(struct smid_encoder *enc, enum smid_encoder_backend backend,
                           int width, int height);
void smid_encoder_destroy(struct smid_encoder *enc);
int smid_encoder_encode_rgb(struct smid_encoder *enc, const uint8_t *rgb,
                            int quality,
                            uint8_t *jpeg_out, unsigned long jpeg_cap,
                            unsigned long *jpeg_len_out);
int smid_encoder_encode_rgb_size(struct smid_encoder *enc, const uint8_t *rgb,
                                 int width, int height, int quality,
                                 uint8_t *jpeg_out, unsigned long jpeg_cap,
                                 unsigned long *jpeg_len_out);
int smid_encoder_encode_bgrx(struct smid_encoder *enc, const uint8_t *bgrx,
                             int quality,
                             uint8_t *jpeg_out, unsigned long jpeg_cap,
                             unsigned long *jpeg_len_out);
int smid_encoder_encode_bgrx_size(struct smid_encoder *enc, const uint8_t *bgrx,
                                  int width, int height, int quality,
                                  uint8_t *jpeg_out, unsigned long jpeg_cap,
                                  unsigned long *jpeg_len_out);
int smid_encoder_encode_bgrx_with_writer(struct smid_encoder *enc,
                                         smid_encoder_bgrx_writer_fn writer,
                                         void *writer_ctx,
                                         int quality,
                                         uint8_t *jpeg_out, unsigned long jpeg_cap,
                                         unsigned long *jpeg_len_out);
int smid_encoder_encode_bgrx_region(struct smid_encoder *enc, const uint8_t *bgrx,
                                    int src_width, int src_height, int src_stride,
                                    int x, int y, int width, int height,
                                    int quality,
                                    uint8_t *jpeg_out, unsigned long jpeg_cap,
                                    unsigned long *jpeg_len_out);

#endif
