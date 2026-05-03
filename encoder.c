#include "encoder.h"

#include "log.h"
#include "protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <turbojpeg.h>
#include <unistd.h>
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_enc_jpeg.h>
#include <va/va_vpp.h>

struct smid_direct_vaapi {
    int drm_fd;
    int width;
    int height;
    VADisplay dpy;
    VAConfigID vpp_config;
    VAContextID vpp_context;
    VAConfigID jpeg_config;
    VAContextID jpeg_context;
    VASurfaceID bgrx_surface;
    VASurfaceID userptr_bgrx_surface;
    VASurfaceID nv12_surface;
    VAImage bgrx_image;
    void *bgrx_image_data;
    bool bgrx_image_derived;
    VABufferID vpp_param_buf;
    VABufferID coded_buf;
    VABufferID pic_param_buf;
    VABufferID qmatrix_buf;
    VABufferID huffman_buf;
    VABufferID packed_header_param_buf;
    VABufferID packed_header_data_buf;
    VABufferID slice_param_buf;
    VARectangle full_rect;
    uintptr_t userptr_buffers[1];
    uintptr_t userptr_key;
    int userptr_width;
    int userptr_height;
    int userptr_stride;
    uint32_t coded_cap;
    int current_quality;
    bool userptr_failed;
    bool initialized;
};

const char *smid_encoder_backend_name(enum smid_encoder_backend backend) {
    switch (backend) {
    case SMID_ENCODER_CPU:
        return "cpu";
    case SMID_ENCODER_DIRECT_VAAPI:
        return "direct-vaapi";
    }
    return "unknown";
}

int smid_encoder_parse_backend(const char *name, enum smid_encoder_backend *backend) {
    if (!strcmp(name, "cpu")) {
        *backend = SMID_ENCODER_CPU;
        return 0;
    }
    if (!strcmp(name, "direct-vaapi")) {
        *backend = SMID_ENCODER_DIRECT_VAAPI;
        return 0;
    }
    return -1;
}

unsigned long smid_encoder_jpeg_capacity(void) {
    return tjBufSize(SMID_WIDTH, SMID_TILE_H, TJSAMP_420);
}

unsigned long smid_encoder_jpeg_capacity_for_size(int width, int height) {
    return tjBufSize(width, height, TJSAMP_420);
}

static const uint8_t jpeg_luma_qtable[64] = {
    16, 11, 10, 16, 24, 40, 51, 61,
    12, 12, 14, 19, 26, 58, 60, 55,
    14, 13, 16, 24, 40, 57, 69, 56,
    14, 17, 22, 29, 51, 87, 80, 62,
    18, 22, 37, 56, 68,109,103, 77,
    24, 35, 55, 64, 81,104,113, 92,
    49, 64, 78, 87,103,121,120,101,
    72, 92, 95, 98,112,100,103, 99,
};

static const uint8_t jpeg_chroma_qtable[64] = {
    17, 18, 24, 47, 99, 99, 99, 99,
    18, 21, 26, 66, 99, 99, 99, 99,
    24, 26, 56, 99, 99, 99, 99, 99,
    47, 66, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
};

static const uint8_t jpeg_bits_dc_luma[16] =
    {0,1,5,1,1,1,1,1,1,0,0,0,0,0,0,0};
static const uint8_t jpeg_val_dc_luma[12] =
    {0,1,2,3,4,5,6,7,8,9,10,11};
static const uint8_t jpeg_bits_ac_luma[16] =
    {0,2,1,3,3,2,4,3,5,5,4,4,0,0,1,0x7d};
static const uint8_t jpeg_val_ac_luma[162] = {
    0x01,0x02,0x03,0x00,0x04,0x11,0x05,0x12,0x21,0x31,0x41,0x06,
    0x13,0x51,0x61,0x07,0x22,0x71,0x14,0x32,0x81,0x91,0xa1,0x08,
    0x23,0x42,0xb1,0xc1,0x15,0x52,0xd1,0xf0,0x24,0x33,0x62,0x72,
    0x82,0x09,0x0a,0x16,0x17,0x18,0x19,0x1a,0x25,0x26,0x27,0x28,
    0x29,0x2a,0x34,0x35,0x36,0x37,0x38,0x39,0x3a,0x43,0x44,0x45,
    0x46,0x47,0x48,0x49,0x4a,0x53,0x54,0x55,0x56,0x57,0x58,0x59,
    0x5a,0x63,0x64,0x65,0x66,0x67,0x68,0x69,0x6a,0x73,0x74,0x75,
    0x76,0x77,0x78,0x79,0x7a,0x83,0x84,0x85,0x86,0x87,0x88,0x89,
    0x8a,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9a,0xa2,0xa3,
    0xa4,0xa5,0xa6,0xa7,0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,0xb5,0xb6,
    0xb7,0xb8,0xb9,0xba,0xc2,0xc3,0xc4,0xc5,0xc6,0xc7,0xc8,0xc9,
    0xca,0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,0xd8,0xd9,0xda,0xe1,0xe2,
    0xe3,0xe4,0xe5,0xe6,0xe7,0xe8,0xe9,0xea,0xf1,0xf2,0xf3,0xf4,
    0xf5,0xf6,0xf7,0xf8,0xf9,0xfa,
};
static const uint8_t jpeg_bits_dc_chroma[16] =
    {0,3,1,1,1,1,1,1,1,1,1,0,0,0,0,0};
static const uint8_t jpeg_val_dc_chroma[12] =
    {0,1,2,3,4,5,6,7,8,9,10,11};
static const uint8_t jpeg_bits_ac_chroma[16] =
    {0,2,1,2,4,4,3,4,7,5,4,4,0,1,2,0x77};
static const uint8_t jpeg_val_ac_chroma[162] = {
    0x00,0x01,0x02,0x03,0x11,0x04,0x05,0x21,0x31,0x06,0x12,0x41,
    0x51,0x07,0x61,0x71,0x13,0x22,0x32,0x81,0x08,0x14,0x42,0x91,
    0xa1,0xb1,0xc1,0x09,0x23,0x33,0x52,0xf0,0x15,0x62,0x72,0xd1,
    0x0a,0x16,0x24,0x34,0xe1,0x25,0xf1,0x17,0x18,0x19,0x1a,0x26,
    0x27,0x28,0x29,0x2a,0x35,0x36,0x37,0x38,0x39,0x3a,0x43,0x44,
    0x45,0x46,0x47,0x48,0x49,0x4a,0x53,0x54,0x55,0x56,0x57,0x58,
    0x59,0x5a,0x63,0x64,0x65,0x66,0x67,0x68,0x69,0x6a,0x73,0x74,
    0x75,0x76,0x77,0x78,0x79,0x7a,0x82,0x83,0x84,0x85,0x86,0x87,
    0x88,0x89,0x8a,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9a,
    0xa2,0xa3,0xa4,0xa5,0xa6,0xa7,0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,
    0xb5,0xb6,0xb7,0xb8,0xb9,0xba,0xc2,0xc3,0xc4,0xc5,0xc6,0xc7,
    0xc8,0xc9,0xca,0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,0xd8,0xd9,0xda,
    0xe2,0xe3,0xe4,0xe5,0xe6,0xe7,0xe8,0xe9,0xea,0xf2,0xf3,0xf4,
    0xf5,0xf6,0xf7,0xf8,0xf9,0xfa,
};

static void rgb_to_bgrx_image(const uint8_t *rgb, int width, int height,
                              const VAImage *image, uint8_t *data) {
    uint8_t *dst_base = data + image->offsets[0];
    uint32_t dst_pitch = image->pitches[0];

    for (int y = 0; y < height; y++) {
        uint8_t *dst = dst_base + (size_t)y * dst_pitch;
        const uint8_t *src = rgb + (size_t)y * (size_t)width * 3u;
        for (int x = 0; x < width; x++) {
            const uint8_t *p = src + (size_t)x * 3u;
            dst[x * 4u + 0u] = p[2];
            dst[x * 4u + 1u] = p[1];
            dst[x * 4u + 2u] = p[0];
            dst[x * 4u + 3u] = 0xff;
        }
    }
}

static void bgrx_to_bgrx_image(const uint8_t *bgrx, int width, int height,
                               const VAImage *image, uint8_t *data) {
    uint8_t *dst_base = data + image->offsets[0];
    uint32_t dst_pitch = image->pitches[0];
    size_t row_bytes = (size_t)width * 4u;

    if (dst_pitch == row_bytes) {
        memcpy(dst_base, bgrx, row_bytes * (size_t)height);
        return;
    }
    for (int y = 0; y < height; y++) {
        memcpy(dst_base + (size_t)y * dst_pitch,
               bgrx + (size_t)y * row_bytes,
               row_bytes);
    }
}

static int va_status_ok(VAStatus st, const char *what);

static int direct_vaapi_map_bgrx_image(struct smid_direct_vaapi *enc, uint8_t **data) {
    if (!enc->bgrx_image_derived) {
        *data = enc->bgrx_image_data;
        return *data ? 0 : -1;
    }
    void *mapped = NULL;
    if (va_status_ok(vaMapBuffer(enc->dpy, enc->bgrx_image.buf, &mapped),
                     "map bgrx image")) {
        return -1;
    }
    *data = mapped;
    return 0;
}

static int direct_vaapi_unmap_bgrx_image(struct smid_direct_vaapi *enc) {
    if (!enc->bgrx_image_derived) {
        return 0;
    }
    return va_status_ok(vaUnmapBuffer(enc->dpy, enc->bgrx_image.buf),
                        "unmap bgrx image");
}

static int va_status_ok(VAStatus st, const char *what) {
    if (st == VA_STATUS_SUCCESS) {
        return 0;
    }
    smid_logf("direct-vaapi %s: %s\n", what, vaErrorStr(st));
    return -1;
}

static int direct_vaapi_open_render_node(void) {
    const char *paths[] = {"/dev/dri/renderD128", "/dev/dri/renderD129", "/dev/dri/card0"};
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        int fd = open(paths[i], O_RDWR | O_CLOEXEC);
        if (fd >= 0) {
            return fd;
        }
    }
    return -1;
}

static void direct_vaapi_reset(struct smid_direct_vaapi *enc) {
    memset(enc, 0, sizeof(*enc));
    enc->drm_fd = -1;
    enc->vpp_config = VA_INVALID_ID;
    enc->vpp_context = VA_INVALID_ID;
    enc->jpeg_config = VA_INVALID_ID;
    enc->jpeg_context = VA_INVALID_ID;
    enc->bgrx_surface = VA_INVALID_SURFACE;
    enc->userptr_bgrx_surface = VA_INVALID_SURFACE;
    enc->nv12_surface = VA_INVALID_SURFACE;
    enc->vpp_param_buf = VA_INVALID_ID;
    enc->coded_buf = VA_INVALID_ID;
    enc->pic_param_buf = VA_INVALID_ID;
    enc->qmatrix_buf = VA_INVALID_ID;
    enc->huffman_buf = VA_INVALID_ID;
    enc->packed_header_param_buf = VA_INVALID_ID;
    enc->packed_header_data_buf = VA_INVALID_ID;
    enc->slice_param_buf = VA_INVALID_ID;
    enc->bgrx_image.image_id = VA_INVALID_ID;
    enc->current_quality = -1;
}

static void direct_vaapi_destroy_userptr_surface(struct smid_direct_vaapi *enc) {
    if (enc->dpy && enc->userptr_bgrx_surface != VA_INVALID_SURFACE) {
        vaDestroySurfaces(enc->dpy, &enc->userptr_bgrx_surface, 1);
    }
    enc->userptr_bgrx_surface = VA_INVALID_SURFACE;
    enc->userptr_key = 0;
    enc->userptr_width = 0;
    enc->userptr_height = 0;
    enc->userptr_stride = 0;
}

static int clamp_jpeg_quality(int quality) {
    if (quality < 1) {
        return 1;
    } else if (quality > 100) {
        return 100;
    }
    return quality;
}

static uint8_t jpeg_quant_value(uint8_t base, int quality) {
    quality = clamp_jpeg_quality(quality);
    int scale = quality < 50 ? 5000 / quality : 200 - quality * 2;
    int value = (base * scale + 50) / 100;
    if (value < 1) {
        value = 1;
    } else if (value > 255) {
        value = 255;
    }
    return (uint8_t)value;
}

static void fill_jpeg_qmatrix(VAQMatrixBufferJPEG *q) {
    memset(q, 0, sizeof(*q));
    q->load_lum_quantiser_matrix = 1;
    q->load_chroma_quantiser_matrix = 1;
    for (int i = 0; i < 64; i++) {
        q->lum_quantiser_matrix[i] = jpeg_luma_qtable[i];
        q->chroma_quantiser_matrix[i] = jpeg_chroma_qtable[i];
    }
}

static void fill_jpeg_huffman(VAHuffmanTableBufferJPEGBaseline *h) {
    memset(h, 0, sizeof(*h));
    h->load_huffman_table[0] = 1;
    h->load_huffman_table[1] = 1;
    memcpy(h->huffman_table[0].num_dc_codes, jpeg_bits_dc_luma, sizeof(jpeg_bits_dc_luma));
    memcpy(h->huffman_table[0].dc_values, jpeg_val_dc_luma, sizeof(jpeg_val_dc_luma));
    memcpy(h->huffman_table[0].num_ac_codes, jpeg_bits_ac_luma, sizeof(jpeg_bits_ac_luma));
    memcpy(h->huffman_table[0].ac_values, jpeg_val_ac_luma, sizeof(jpeg_val_ac_luma));
    memcpy(h->huffman_table[1].num_dc_codes, jpeg_bits_dc_chroma, sizeof(jpeg_bits_dc_chroma));
    memcpy(h->huffman_table[1].dc_values, jpeg_val_dc_chroma, sizeof(jpeg_val_dc_chroma));
    memcpy(h->huffman_table[1].num_ac_codes, jpeg_bits_ac_chroma, sizeof(jpeg_bits_ac_chroma));
    memcpy(h->huffman_table[1].ac_values, jpeg_val_ac_chroma, sizeof(jpeg_val_ac_chroma));
}

static void direct_vaapi_destroy_buffers(VADisplay dpy, VABufferID *buffers, size_t n) {
    if (!dpy) {
        return;
    }
    for (size_t i = 0; i < n; i++) {
        if (buffers[i] != VA_INVALID_ID) {
            vaDestroyBuffer(dpy, buffers[i]);
            buffers[i] = VA_INVALID_ID;
        }
    }
}

static int direct_vaapi_create_surface(VADisplay dpy, unsigned int rt_format, unsigned int fourcc,
                                       int width, int height, VASurfaceID *surface,
                                       const char *what) {
    VASurfaceAttrib attr;
    memset(&attr, 0, sizeof(attr));
    attr.type = VASurfaceAttribPixelFormat;
    attr.flags = VA_SURFACE_ATTRIB_SETTABLE;
    attr.value.type = VAGenericValueTypeInteger;
    attr.value.value.i = (int)fourcc;

    return va_status_ok(vaCreateSurfaces(dpy, rt_format, width, height, surface, 1,
                                         &attr, 1),
                        what);
}

static int direct_vaapi_ensure_userptr_bgrx_surface(struct smid_direct_vaapi *enc,
                                                    const uint8_t *bgrx,
                                                    int width, int height,
                                                    int stride) {
    if (!bgrx || width <= 0 || height <= 0 || stride < width * 4) {
        return -1;
    }
    uintptr_t key = (uintptr_t)bgrx;
    if (enc->userptr_bgrx_surface != VA_INVALID_SURFACE &&
        enc->userptr_key == key &&
        enc->userptr_width == width &&
        enc->userptr_height == height &&
        enc->userptr_stride == stride) {
        return 0;
    }
    if (enc->userptr_failed) {
        return -1;
    }

    direct_vaapi_destroy_userptr_surface(enc);

    enc->userptr_buffers[0] = key;
    VASurfaceAttribExternalBuffers ext;
    memset(&ext, 0, sizeof(ext));
    ext.pixel_format = VA_FOURCC_BGRX;
    ext.width = (uint32_t)width;
    ext.height = (uint32_t)height;
    ext.data_size = (uint32_t)((size_t)stride * (size_t)height);
    ext.num_planes = 1;
    ext.pitches[0] = (uint32_t)stride;
    ext.offsets[0] = 0;
    ext.buffers = enc->userptr_buffers;
    ext.num_buffers = 1;
    ext.flags = VA_SURFACE_EXTBUF_DESC_CACHED;

    VASurfaceAttrib attrs[4];
    memset(attrs, 0, sizeof(attrs));
    attrs[0].type = VASurfaceAttribMemoryType;
    attrs[0].flags = VA_SURFACE_ATTRIB_SETTABLE;
    attrs[0].value.type = VAGenericValueTypeInteger;
    attrs[0].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_USER_PTR;
    attrs[1].type = VASurfaceAttribExternalBufferDescriptor;
    attrs[1].flags = VA_SURFACE_ATTRIB_SETTABLE;
    attrs[1].value.type = VAGenericValueTypePointer;
    attrs[1].value.value.p = &ext;
    attrs[2].type = VASurfaceAttribPixelFormat;
    attrs[2].flags = VA_SURFACE_ATTRIB_SETTABLE;
    attrs[2].value.type = VAGenericValueTypeInteger;
    attrs[2].value.value.i = VA_FOURCC_BGRX;
    attrs[3].type = VASurfaceAttribUsageHint;
    attrs[3].flags = VA_SURFACE_ATTRIB_SETTABLE;
    attrs[3].value.type = VAGenericValueTypeInteger;
    attrs[3].value.value.i = VA_SURFACE_ATTRIB_USAGE_HINT_VPP_READ;

    VAStatus st = vaCreateSurfaces(enc->dpy, VA_RT_FORMAT_RGB32, width, height,
                                   &enc->userptr_bgrx_surface, 1, attrs, 4);
    if (st != VA_STATUS_SUCCESS) {
        smid_logf("direct-vaapi userptr bgrx surface unavailable: %s\n", vaErrorStr(st));
        enc->userptr_bgrx_surface = VA_INVALID_SURFACE;
        enc->userptr_failed = true;
        return -1;
    }

    enc->userptr_key = key;
    enc->userptr_width = width;
    enc->userptr_height = height;
    enc->userptr_stride = stride;
    return 0;
}

static void jpeg_header_put_u8(uint8_t *buf, size_t cap, size_t *pos, uint8_t value) {
    if (*pos < cap) {
        buf[*pos] = value;
    }
    (*pos)++;
}

static void jpeg_header_put_u16(uint8_t *buf, size_t cap, size_t *pos, uint16_t value) {
    jpeg_header_put_u8(buf, cap, pos, (uint8_t)(value >> 8));
    jpeg_header_put_u8(buf, cap, pos, (uint8_t)value);
}

static void jpeg_header_put_marker(uint8_t *buf, size_t cap, size_t *pos, uint8_t marker) {
    jpeg_header_put_u8(buf, cap, pos, 0xff);
    jpeg_header_put_u8(buf, cap, pos, marker);
}

static int build_jpeg_packed_header(int width, int height, int quality,
                                    uint8_t *buf, size_t cap, size_t *len_out) {
    size_t pos = 0;

    jpeg_header_put_marker(buf, cap, &pos, 0xd8); /* SOI */

    jpeg_header_put_marker(buf, cap, &pos, 0xe0); /* APP0/JFIF */
    jpeg_header_put_u16(buf, cap, &pos, 16);
    jpeg_header_put_u8(buf, cap, &pos, 'J');
    jpeg_header_put_u8(buf, cap, &pos, 'F');
    jpeg_header_put_u8(buf, cap, &pos, 'I');
    jpeg_header_put_u8(buf, cap, &pos, 'F');
    jpeg_header_put_u8(buf, cap, &pos, 0);
    jpeg_header_put_u8(buf, cap, &pos, 1);
    jpeg_header_put_u8(buf, cap, &pos, 1);
    jpeg_header_put_u8(buf, cap, &pos, 0);
    jpeg_header_put_u16(buf, cap, &pos, 1);
    jpeg_header_put_u16(buf, cap, &pos, 1);
    jpeg_header_put_u8(buf, cap, &pos, 0);
    jpeg_header_put_u8(buf, cap, &pos, 0);

    jpeg_header_put_marker(buf, cap, &pos, 0xdb); /* DQT luma */
    jpeg_header_put_u16(buf, cap, &pos, 67);
    jpeg_header_put_u8(buf, cap, &pos, 0);
    for (int i = 0; i < 64; i++) {
        jpeg_header_put_u8(buf, cap, &pos, jpeg_quant_value(jpeg_luma_qtable[i], quality));
    }
    jpeg_header_put_marker(buf, cap, &pos, 0xdb); /* DQT chroma */
    jpeg_header_put_u16(buf, cap, &pos, 67);
    jpeg_header_put_u8(buf, cap, &pos, 1);
    for (int i = 0; i < 64; i++) {
        jpeg_header_put_u8(buf, cap, &pos, jpeg_quant_value(jpeg_chroma_qtable[i], quality));
    }

    jpeg_header_put_marker(buf, cap, &pos, 0xc0); /* SOF0 */
    jpeg_header_put_u16(buf, cap, &pos, 17);
    jpeg_header_put_u8(buf, cap, &pos, 8);
    jpeg_header_put_u16(buf, cap, &pos, (uint16_t)height);
    jpeg_header_put_u16(buf, cap, &pos, (uint16_t)width);
    jpeg_header_put_u8(buf, cap, &pos, 3);
    jpeg_header_put_u8(buf, cap, &pos, 1);
    jpeg_header_put_u8(buf, cap, &pos, 0x22);
    jpeg_header_put_u8(buf, cap, &pos, 0);
    jpeg_header_put_u8(buf, cap, &pos, 2);
    jpeg_header_put_u8(buf, cap, &pos, 0x11);
    jpeg_header_put_u8(buf, cap, &pos, 1);
    jpeg_header_put_u8(buf, cap, &pos, 3);
    jpeg_header_put_u8(buf, cap, &pos, 0x11);
    jpeg_header_put_u8(buf, cap, &pos, 1);

    const struct {
        const uint8_t *bits;
        size_t bits_len;
        const uint8_t *values;
        size_t values_len;
        uint8_t table_class;
        uint8_t table_id;
    } huff[] = {
        {jpeg_bits_dc_luma, sizeof(jpeg_bits_dc_luma),
         jpeg_val_dc_luma, sizeof(jpeg_val_dc_luma), 0, 0},
        {jpeg_bits_ac_luma, sizeof(jpeg_bits_ac_luma),
         jpeg_val_ac_luma, sizeof(jpeg_val_ac_luma), 1, 0},
        {jpeg_bits_dc_chroma, sizeof(jpeg_bits_dc_chroma),
         jpeg_val_dc_chroma, sizeof(jpeg_val_dc_chroma), 0, 1},
        {jpeg_bits_ac_chroma, sizeof(jpeg_bits_ac_chroma),
         jpeg_val_ac_chroma, sizeof(jpeg_val_ac_chroma), 1, 1},
    };
    for (size_t t = 0; t < sizeof(huff) / sizeof(huff[0]); t++) {
        jpeg_header_put_marker(buf, cap, &pos, 0xc4); /* DHT */
        jpeg_header_put_u16(buf, cap, &pos,
                            (uint16_t)(3 + huff[t].bits_len + huff[t].values_len));
        jpeg_header_put_u8(buf, cap, &pos,
                           (uint8_t)((huff[t].table_class << 4) | huff[t].table_id));
        for (size_t i = 0; i < huff[t].bits_len; i++) {
            jpeg_header_put_u8(buf, cap, &pos, huff[t].bits[i]);
        }
        for (size_t i = 0; i < huff[t].values_len; i++) {
            jpeg_header_put_u8(buf, cap, &pos, huff[t].values[i]);
        }
    }

    jpeg_header_put_marker(buf, cap, &pos, 0xda); /* SOS */
    jpeg_header_put_u16(buf, cap, &pos, 12);
    jpeg_header_put_u8(buf, cap, &pos, 3);
    jpeg_header_put_u8(buf, cap, &pos, 1);
    jpeg_header_put_u8(buf, cap, &pos, 0x00);
    jpeg_header_put_u8(buf, cap, &pos, 2);
    jpeg_header_put_u8(buf, cap, &pos, 0x11);
    jpeg_header_put_u8(buf, cap, &pos, 3);
    jpeg_header_put_u8(buf, cap, &pos, 0x11);
    jpeg_header_put_u8(buf, cap, &pos, 0);
    jpeg_header_put_u8(buf, cap, &pos, 63);
    jpeg_header_put_u8(buf, cap, &pos, 0);

    if (pos > cap) {
        smid_logf("direct-vaapi jpeg header overflow: %zu > %zu\n", pos, cap);
        return -1;
    }
    *len_out = pos;
    return 0;
}

static int direct_vaapi_init(struct smid_direct_vaapi *enc, int width, int height) {
    direct_vaapi_reset(enc);
    enc->width = width;
    enc->height = height;
    enc->drm_fd = direct_vaapi_open_render_node();
    if (enc->drm_fd < 0) {
        smid_logf("direct-vaapi open render node failed: %s\n", strerror(errno));
        return -1;
    }
    enc->dpy = vaGetDisplayDRM(enc->drm_fd);
    if (!enc->dpy) {
        smid_logf("direct-vaapi vaGetDisplayDRM failed\n");
        return -1;
    }
    int major = 0;
    int minor = 0;
    if (va_status_ok(vaInitialize(enc->dpy, &major, &minor), "initialize")) {
        return -1;
    }

    if (va_status_ok(vaCreateConfig(enc->dpy, VAProfileNone, VAEntrypointVideoProc,
                                    NULL, 0, &enc->vpp_config),
                     "create vpp config")) {
        return -1;
    }
    if (va_status_ok(vaCreateContext(enc->dpy, enc->vpp_config, 0, 0, 0,
                                     NULL, 0, &enc->vpp_context),
                     "create vpp context")) {
        return -1;
    }

    VAConfigAttrib jpeg_attrs[2] = {
        {.type = VAConfigAttribRTFormat},
        {.type = VAConfigAttribEncPackedHeaders},
    };
    if (va_status_ok(vaGetConfigAttributes(enc->dpy, VAProfileJPEGBaseline,
                                           VAEntrypointEncPicture, jpeg_attrs, 2),
                     "get jpeg config")) {
        return -1;
    }
    if (!(jpeg_attrs[0].value & VA_RT_FORMAT_YUV420)) {
        smid_logf("direct-vaapi JPEG encoder lacks YUV420 support\n");
        return -1;
    }
    if (jpeg_attrs[1].value == VA_ATTRIB_NOT_SUPPORTED ||
        !(jpeg_attrs[1].value & VA_ENC_PACKED_HEADER_RAW_DATA)) {
        smid_logf("direct-vaapi JPEG encoder lacks raw packed header support\n");
        return -1;
    }
    jpeg_attrs[0].value = VA_RT_FORMAT_YUV420;
    jpeg_attrs[1].value = VA_ENC_PACKED_HEADER_RAW_DATA;
    if (va_status_ok(vaCreateConfig(enc->dpy, VAProfileJPEGBaseline,
                                    VAEntrypointEncPicture, jpeg_attrs, 2,
                                    &enc->jpeg_config),
                     "create jpeg config")) {
        return -1;
    }
    if (direct_vaapi_create_surface(enc->dpy, VA_RT_FORMAT_RGB32, VA_FOURCC_BGRX,
                                    width, height, &enc->bgrx_surface,
                                    "create bgrx surface")) {
        return -1;
    }
    if (direct_vaapi_create_surface(enc->dpy, VA_RT_FORMAT_YUV420, VA_FOURCC_NV12,
                                    width, height, &enc->nv12_surface,
                                    "create nv12 surface")) {
        return -1;
    }
    enc->full_rect = (VARectangle){
        .x = 0,
        .y = 0,
        .width = (unsigned short)width,
        .height = (unsigned short)height,
    };
    VAProcPipelineParameterBuffer vpp;
    memset(&vpp, 0, sizeof(vpp));
    vpp.surface = enc->bgrx_surface;
    vpp.surface_region = &enc->full_rect;
    vpp.surface_color_standard = VAProcColorStandardSRGB;
    vpp.output_region = &enc->full_rect;
    vpp.output_background_color = 0xff000000;
    vpp.output_color_standard = VAProcColorStandardBT601;
    vpp.filter_flags = VA_FILTER_SCALING_DEFAULT;
    vpp.input_color_properties.color_range = VA_SOURCE_RANGE_FULL;
    vpp.output_color_properties.color_range = VA_SOURCE_RANGE_FULL;
    vpp.output_color_properties.chroma_sample_location =
        VA_CHROMA_SITING_VERTICAL_CENTER | VA_CHROMA_SITING_HORIZONTAL_CENTER;
    vpp.processing_mode = VAProcPerformanceMode;
    if (va_status_ok(vaCreateBuffer(enc->dpy, enc->vpp_context,
                                    VAProcPipelineParameterBufferType,
                                    sizeof(vpp), 1, &vpp, &enc->vpp_param_buf),
                     "create vpp params")) {
        return -1;
    }
    if (va_status_ok(vaCreateContext(enc->dpy, enc->jpeg_config, width, height,
                                     VA_PROGRESSIVE, &enc->nv12_surface, 1,
                                     &enc->jpeg_context),
                     "create jpeg context")) {
        return -1;
    }

    VAImageFormat bgrx = {
        .fourcc = VA_FOURCC_BGRX,
        .byte_order = VA_LSB_FIRST,
        .bits_per_pixel = 32,
        .depth = 32,
    };
    VAImage derived;
    memset(&derived, 0, sizeof(derived));
    derived.image_id = VA_INVALID_ID;
    if (vaDeriveImage(enc->dpy, enc->bgrx_surface, &derived) == VA_STATUS_SUCCESS &&
        derived.format.fourcc == VA_FOURCC_BGRX &&
        derived.format.bits_per_pixel == 32) {
        enc->bgrx_image = derived;
        enc->bgrx_image_derived = true;
    } else {
        if (derived.image_id != VA_INVALID_ID) {
            vaDestroyImage(enc->dpy, derived.image_id);
        }
        if (va_status_ok(vaCreateImage(enc->dpy, &bgrx, width, height, &enc->bgrx_image),
                         "create bgrx image")) {
            return -1;
        }
        if (va_status_ok(vaMapBuffer(enc->dpy, enc->bgrx_image.buf, &enc->bgrx_image_data),
                         "map bgrx image")) {
            return -1;
        }
    }

    enc->coded_cap = (uint32_t)smid_encoder_jpeg_capacity_for_size(width, height);
    if (va_status_ok(vaCreateBuffer(enc->dpy, enc->jpeg_context, VAEncCodedBufferType,
                                    enc->coded_cap, 1, NULL, &enc->coded_buf),
                     "create coded buffer")) {
        return -1;
    }

    VAQMatrixBufferJPEG q;
    fill_jpeg_qmatrix(&q);
    if (va_status_ok(vaCreateBuffer(enc->dpy, enc->jpeg_context, VAQMatrixBufferType,
                                    sizeof(q), 1, &q, &enc->qmatrix_buf),
                     "create qmatrix")) {
        return -1;
    }

    VAHuffmanTableBufferJPEGBaseline h;
    fill_jpeg_huffman(&h);
    if (va_status_ok(vaCreateBuffer(enc->dpy, enc->jpeg_context,
                                    VAHuffmanTableBufferType,
                                    sizeof(h), 1, &h, &enc->huffman_buf),
                     "create huffman")) {
        return -1;
    }

    VAEncSliceParameterBufferJPEG slice;
    memset(&slice, 0, sizeof(slice));
    slice.restart_interval = 0;
    slice.num_components = 3;
    slice.components[0].component_selector = 1;
    slice.components[0].dc_table_selector = 0;
    slice.components[0].ac_table_selector = 0;
    slice.components[1].component_selector = 2;
    slice.components[1].dc_table_selector = 1;
    slice.components[1].ac_table_selector = 1;
    slice.components[2].component_selector = 3;
    slice.components[2].dc_table_selector = 1;
    slice.components[2].ac_table_selector = 1;
    if (va_status_ok(vaCreateBuffer(enc->dpy, enc->jpeg_context,
                                    VAEncSliceParameterBufferType,
                                    sizeof(slice), 1, &slice, &enc->slice_param_buf),
                     "create slice params")) {
        return -1;
    }
    enc->initialized = true;
    return 0;
}

static void direct_vaapi_destroy(struct smid_direct_vaapi *enc) {
    if (!enc) {
        return;
    }
    if (enc->dpy && enc->bgrx_image_data) {
        vaUnmapBuffer(enc->dpy, enc->bgrx_image.buf);
    }
    if (enc->dpy && enc->bgrx_image.image_id != VA_INVALID_ID) {
        vaDestroyImage(enc->dpy, enc->bgrx_image.image_id);
    }
    if (enc->dpy) {
        VABufferID bufs[] = {
            enc->vpp_param_buf,
            enc->pic_param_buf,
            enc->qmatrix_buf,
            enc->huffman_buf,
            enc->packed_header_param_buf,
            enc->packed_header_data_buf,
            enc->slice_param_buf,
            enc->coded_buf,
        };
        direct_vaapi_destroy_buffers(enc->dpy, bufs, sizeof(bufs) / sizeof(bufs[0]));
        if (enc->jpeg_context != VA_INVALID_ID) {
            vaDestroyContext(enc->dpy, enc->jpeg_context);
        }
        if (enc->vpp_context != VA_INVALID_ID) {
            vaDestroyContext(enc->dpy, enc->vpp_context);
        }
        direct_vaapi_destroy_userptr_surface(enc);
        if (enc->nv12_surface != VA_INVALID_SURFACE) {
            vaDestroySurfaces(enc->dpy, &enc->nv12_surface, 1);
        }
        if (enc->bgrx_surface != VA_INVALID_SURFACE) {
            vaDestroySurfaces(enc->dpy, &enc->bgrx_surface, 1);
        }
        if (enc->jpeg_config != VA_INVALID_ID) {
            vaDestroyConfig(enc->dpy, enc->jpeg_config);
        }
        if (enc->vpp_config != VA_INVALID_ID) {
            vaDestroyConfig(enc->dpy, enc->vpp_config);
        }
        vaTerminate(enc->dpy);
    }
    if (enc->drm_fd >= 0) {
        close(enc->drm_fd);
    }
    direct_vaapi_reset(enc);
}

static int direct_vaapi_update_quality(struct smid_direct_vaapi *enc, int quality) {
    quality = clamp_jpeg_quality(quality);
    if (enc->current_quality == quality &&
        enc->pic_param_buf != VA_INVALID_ID &&
        enc->packed_header_param_buf != VA_INVALID_ID &&
        enc->packed_header_data_buf != VA_INVALID_ID) {
        return 0;
    }

    VABufferID old_bufs[] = {
        enc->pic_param_buf,
        enc->packed_header_param_buf,
        enc->packed_header_data_buf,
    };
    direct_vaapi_destroy_buffers(enc->dpy, old_bufs, sizeof(old_bufs) / sizeof(old_bufs[0]));
    enc->pic_param_buf = VA_INVALID_ID;
    enc->packed_header_param_buf = VA_INVALID_ID;
    enc->packed_header_data_buf = VA_INVALID_ID;
    enc->current_quality = -1;

    VAEncPictureParameterBufferJPEG pic;
    memset(&pic, 0, sizeof(pic));
    pic.reconstructed_picture = enc->nv12_surface;
    pic.picture_width = (uint16_t)enc->width;
    pic.picture_height = (uint16_t)enc->height;
    pic.coded_buf = enc->coded_buf;
    pic.pic_flags.bits.profile = 0;
    pic.pic_flags.bits.progressive = 0;
    pic.pic_flags.bits.huffman = 1;
    pic.pic_flags.bits.interleaved = 0;
    pic.sample_bit_depth = 8;
    pic.num_scan = 1;
    pic.num_components = 3;
    pic.component_id[0] = 1;
    pic.component_id[1] = 2;
    pic.component_id[2] = 3;
    pic.quantiser_table_selector[0] = 0;
    pic.quantiser_table_selector[1] = 1;
    pic.quantiser_table_selector[2] = 1;
    pic.quality = (uint8_t)quality;
    if (va_status_ok(vaCreateBuffer(enc->dpy, enc->jpeg_context,
                                    VAEncPictureParameterBufferType,
                                    sizeof(pic), 1, &pic, &enc->pic_param_buf),
                     "create picture params")) {
        return -1;
    }

    uint8_t jpeg_header[1024];
    size_t jpeg_header_len = 0;
    if (build_jpeg_packed_header(enc->width, enc->height, quality,
                                 jpeg_header, sizeof(jpeg_header), &jpeg_header_len)) {
        return -1;
    }
    VAEncPackedHeaderParameterBuffer packed_param = {
        .type = VAEncPackedHeaderRawData,
        .bit_length = (unsigned int)jpeg_header_len * 8u,
        .has_emulation_bytes = 0,
    };
    if (va_status_ok(vaCreateBuffer(enc->dpy, enc->jpeg_context,
                                    VAEncPackedHeaderParameterBufferType,
                                    sizeof(packed_param), 1, &packed_param,
                                    &enc->packed_header_param_buf),
                     "create packed header params")) {
        return -1;
    }
    if (va_status_ok(vaCreateBuffer(enc->dpy, enc->jpeg_context,
                                    VAEncPackedHeaderDataBufferType,
                                    (unsigned int)jpeg_header_len, 1, jpeg_header,
                                    &enc->packed_header_data_buf),
                     "create packed header data")) {
        return -1;
    }
    enc->current_quality = quality;
    return 0;
}

static int direct_vaapi_encode_nv12_jpeg(struct smid_direct_vaapi *enc,
                                         uint8_t *jpeg_out, unsigned long jpeg_cap,
                                         unsigned long *jpeg_len_out) {
    VABufferID render_buffers[] = {
        enc->pic_param_buf,
        enc->qmatrix_buf,
        enc->huffman_buf,
        enc->packed_header_param_buf,
        enc->packed_header_data_buf,
        enc->slice_param_buf,
    };

    if (va_status_ok(vaBeginPicture(enc->dpy, enc->jpeg_context, enc->nv12_surface),
                     "begin jpeg picture")) {
        return -1;
    }
    if (va_status_ok(vaRenderPicture(enc->dpy, enc->jpeg_context, render_buffers,
                                     (int)(sizeof(render_buffers) / sizeof(render_buffers[0]))),
                     "render jpeg picture")) {
        return -1;
    }
    if (va_status_ok(vaEndPicture(enc->dpy, enc->jpeg_context), "end jpeg picture")) {
        return -1;
    }
    if (va_status_ok(vaSyncSurface(enc->dpy, enc->nv12_surface), "sync jpeg surface")) {
        return -1;
    }

    VACodedBufferSegment *seg = NULL;
    if (va_status_ok(vaMapBuffer(enc->dpy, enc->coded_buf, (void **)&seg), "map coded buffer")) {
        return -1;
    }
    unsigned long total = 0;
    for (VACodedBufferSegment *s = seg; s; s = (VACodedBufferSegment *)s->next) {
        if (total + s->size > jpeg_cap) {
            vaUnmapBuffer(enc->dpy, enc->coded_buf);
            smid_logf("direct-vaapi jpeg output too large: %lu > %lu\n", total + s->size, jpeg_cap);
            return -1;
        }
        memcpy(jpeg_out + total, s->buf, s->size);
        total += s->size;
    }
    vaUnmapBuffer(enc->dpy, enc->coded_buf);
    *jpeg_len_out = total;
    return total ? 0 : -1;
}

static int direct_vaapi_vpp_region(struct smid_direct_vaapi *enc,
                                   VASurfaceID input_surface,
                                   const VARectangle *source_rect) {
    VAProcPipelineParameterBuffer vpp;
    memset(&vpp, 0, sizeof(vpp));
    vpp.surface = input_surface;
    vpp.surface_region = (VARectangle *)source_rect;
    vpp.surface_color_standard = VAProcColorStandardSRGB;
    vpp.output_region = &enc->full_rect;
    vpp.output_background_color = 0xff000000;
    vpp.output_color_standard = VAProcColorStandardBT601;
    vpp.filter_flags = VA_FILTER_SCALING_DEFAULT;
    vpp.input_color_properties.color_range = VA_SOURCE_RANGE_FULL;
    vpp.output_color_properties.color_range = VA_SOURCE_RANGE_FULL;
    vpp.output_color_properties.chroma_sample_location =
        VA_CHROMA_SITING_VERTICAL_CENTER | VA_CHROMA_SITING_HORIZONTAL_CENTER;
    vpp.processing_mode = VAProcPerformanceMode;

    VABufferID vpp_param = VA_INVALID_ID;
    if (va_status_ok(vaCreateBuffer(enc->dpy, enc->vpp_context,
                                    VAProcPipelineParameterBufferType,
                                    sizeof(vpp), 1, &vpp, &vpp_param),
                     "create dynamic vpp params")) {
        return -1;
    }
    int rc = 0;
    if (va_status_ok(vaBeginPicture(enc->dpy, enc->vpp_context, enc->nv12_surface),
                     "begin vpp picture") ||
        va_status_ok(vaRenderPicture(enc->dpy, enc->vpp_context, &vpp_param, 1),
                     "render vpp picture") ||
        va_status_ok(vaEndPicture(enc->dpy, enc->vpp_context), "end vpp picture")) {
        rc = -1;
    }
    vaDestroyBuffer(enc->dpy, vpp_param);
    return rc;
}

static int direct_vaapi_encode_uploaded_bgrx(struct smid_direct_vaapi *enc,
                                             int quality,
                                             uint8_t *jpeg_out, unsigned long jpeg_cap,
                                             unsigned long *jpeg_len_out) {
    if (direct_vaapi_update_quality(enc, quality)) {
        return -1;
    }
    if (!enc->bgrx_image_derived) {
        if (va_status_ok(vaPutImage(enc->dpy, enc->bgrx_surface, enc->bgrx_image.image_id,
                                    0, 0, enc->width, enc->height, 0, 0,
                                    enc->width, enc->height),
                         "put bgrx image")) {
            return -1;
        }
    }

    if (va_status_ok(vaBeginPicture(enc->dpy, enc->vpp_context, enc->nv12_surface),
                     "begin vpp picture")) {
        return -1;
    }
    if (va_status_ok(vaRenderPicture(enc->dpy, enc->vpp_context, &enc->vpp_param_buf, 1),
                     "render vpp picture")) {
        return -1;
    }
    if (va_status_ok(vaEndPicture(enc->dpy, enc->vpp_context), "end vpp picture")) {
        return -1;
    }

    return direct_vaapi_encode_nv12_jpeg(enc, jpeg_out, jpeg_cap, jpeg_len_out);
}

static int direct_vaapi_encode_userptr_bgrx_region(struct smid_direct_vaapi *enc,
                                                   const uint8_t *bgrx,
                                                   int src_width, int src_height,
                                                   int src_stride,
                                                   int x, int y, int width, int height,
                                                   int quality,
                                                   uint8_t *jpeg_out,
                                                   unsigned long jpeg_cap,
                                                   unsigned long *jpeg_len_out) {
    if (!enc->initialized || width != enc->width || height != enc->height ||
        x < 0 || y < 0 || x + width > src_width || y + height > src_height) {
        return -1;
    }
    if (direct_vaapi_update_quality(enc, quality)) {
        return -1;
    }
    if (direct_vaapi_ensure_userptr_bgrx_surface(enc, bgrx, src_width, src_height,
                                                 src_stride)) {
        return -1;
    }
    VARectangle source_rect = {
        .x = (short)x,
        .y = (short)y,
        .width = (unsigned short)width,
        .height = (unsigned short)height,
    };
    if (direct_vaapi_vpp_region(enc, enc->userptr_bgrx_surface, &source_rect)) {
        return -1;
    }
    return direct_vaapi_encode_nv12_jpeg(enc, jpeg_out, jpeg_cap, jpeg_len_out);
}

static int direct_vaapi_encode_rgb_into(struct smid_direct_vaapi *enc, const uint8_t *rgb,
                                        int quality,
                                        uint8_t *jpeg_out, unsigned long jpeg_cap,
                                        unsigned long *jpeg_len_out) {
    if (!enc->initialized) {
        return -1;
    }
    uint8_t *data = NULL;
    if (direct_vaapi_map_bgrx_image(enc, &data)) {
        return -1;
    }
    rgb_to_bgrx_image(rgb, enc->width, enc->height, &enc->bgrx_image, data);
    if (direct_vaapi_unmap_bgrx_image(enc)) {
        return -1;
    }
    return direct_vaapi_encode_uploaded_bgrx(enc, quality, jpeg_out, jpeg_cap, jpeg_len_out);
}

static int direct_vaapi_encode_bgrx_into(struct smid_direct_vaapi *enc, const uint8_t *bgrx,
                                         int quality,
                                         uint8_t *jpeg_out, unsigned long jpeg_cap,
                                         unsigned long *jpeg_len_out) {
    if (!enc->initialized) {
        return -1;
    }
    uint8_t *data = NULL;
    if (direct_vaapi_map_bgrx_image(enc, &data)) {
        return -1;
    }
    bgrx_to_bgrx_image(bgrx, enc->width, enc->height, &enc->bgrx_image, data);
    if (direct_vaapi_unmap_bgrx_image(enc)) {
        return -1;
    }
    return direct_vaapi_encode_uploaded_bgrx(enc, quality, jpeg_out, jpeg_cap, jpeg_len_out);
}

static int direct_vaapi_encode_bgrx_writer_into(struct smid_direct_vaapi *enc,
                                                smid_encoder_bgrx_writer_fn writer,
                                                void *writer_ctx,
                                                int quality,
                                                uint8_t *jpeg_out, unsigned long jpeg_cap,
                                                unsigned long *jpeg_len_out) {
    if (!enc->initialized || !writer) {
        return -1;
    }
    uint8_t *data = NULL;
    if (direct_vaapi_map_bgrx_image(enc, &data)) {
        return -1;
    }
    int rc = writer(writer_ctx, data, (int)enc->bgrx_image.pitches[0],
                    enc->width, enc->height);
    if (direct_vaapi_unmap_bgrx_image(enc)) {
        return -1;
    }
    if (rc) {
        return -1;
    }
    return direct_vaapi_encode_uploaded_bgrx(enc, quality, jpeg_out, jpeg_cap, jpeg_len_out);
}

static int encoder_init_cpu(struct smid_encoder *enc, int width, int height) {
    enc->backend = SMID_ENCODER_CPU;
    enc->width = width;
    enc->height = height;
    enc->jpeg_cap = smid_encoder_jpeg_capacity_for_size(width, height);
    enc->tj = tjInitCompress();
    if (!enc->tj) {
        smid_logf("cpu jpeg init failed\n");
        return -1;
    }
    return 0;
}

int smid_encoder_init_size(struct smid_encoder *enc, enum smid_encoder_backend backend,
                           int width, int height) {
    memset(enc, 0, sizeof(*enc));
    if (width <= 0 || height <= 0 || (width & 1) || (height & 1)) {
        smid_logf("encoder invalid size %dx%d\n", width, height);
        return -1;
    }
    if (backend == SMID_ENCODER_CPU) {
        return encoder_init_cpu(enc, width, height);
    }

    enc->backend = backend;
    enc->width = width;
    enc->height = height;
    enc->jpeg_cap = smid_encoder_jpeg_capacity_for_size(width, height);

    enc->direct = malloc(sizeof(*enc->direct));
    if (!enc->direct) {
        smid_logf("direct-vaapi encoder allocation failed; falling back to cpu jpeg\n");
        smid_encoder_destroy(enc);
        return encoder_init_cpu(enc, width, height);
    }
    if (direct_vaapi_init(enc->direct, width, height)) {
        smid_logf("direct-vaapi encoder unavailable for %dx%d; falling back to cpu jpeg\n",
                  width, height);
        smid_encoder_destroy(enc);
        return encoder_init_cpu(enc, width, height);
    }
    return 0;
}

int smid_encoder_encode_bgrx_with_writer(struct smid_encoder *enc,
                                         smid_encoder_bgrx_writer_fn writer,
                                         void *writer_ctx,
                                         int quality,
                                         uint8_t *jpeg_out, unsigned long jpeg_cap,
                                         unsigned long *jpeg_len_out) {
    if (!enc || !writer || !jpeg_out || !jpeg_len_out) {
        return -1;
    }
    if (enc->backend == SMID_ENCODER_DIRECT_VAAPI) {
        return direct_vaapi_encode_bgrx_writer_into(enc->direct, writer, writer_ctx,
                                                    quality, jpeg_out, jpeg_cap, jpeg_len_out);
    }

    size_t row_bytes = (size_t)enc->width * 4u;
    size_t bytes = row_bytes * (size_t)enc->height;
    uint8_t *bgrx = malloc(bytes);
    if (!bgrx) {
        return -1;
    }
    int rc = writer(writer_ctx, bgrx, (int)row_bytes, enc->width, enc->height);
    if (!rc) {
        rc = smid_encoder_encode_bgrx(enc, bgrx, quality, jpeg_out, jpeg_cap, jpeg_len_out);
    }
    free(bgrx);
    return rc;
}

int smid_encoder_encode_bgrx_region(struct smid_encoder *enc, const uint8_t *bgrx,
                                    int src_width, int src_height, int src_stride,
                                    int x, int y, int width, int height,
                                    int quality,
                                    uint8_t *jpeg_out, unsigned long jpeg_cap,
                                    unsigned long *jpeg_len_out) {
    if (!enc || !bgrx || !jpeg_out || !jpeg_len_out ||
        src_width <= 0 || src_height <= 0 || src_stride < src_width * 4 ||
        x < 0 || y < 0 || width <= 0 || height <= 0 ||
        x + width > src_width || y + height > src_height) {
        return -1;
    }
    quality = clamp_jpeg_quality(quality);
    if (enc->backend == SMID_ENCODER_DIRECT_VAAPI) {
        return direct_vaapi_encode_userptr_bgrx_region(enc->direct, bgrx,
                                                       src_width, src_height,
                                                       src_stride, x, y,
                                                       width, height, quality,
                                                       jpeg_out, jpeg_cap,
                                                       jpeg_len_out);
    }

    const uint8_t *region = bgrx + (size_t)y * (size_t)src_stride + (size_t)x * 4u;
    uint8_t *jpeg_ptr = jpeg_out;
    *jpeg_len_out = jpeg_cap;
    if (tjCompress2(enc->tj, region, width, src_stride, height, TJPF_BGRX,
                    &jpeg_ptr, jpeg_len_out, TJSAMP_420, quality,
                    TJFLAG_FASTDCT | TJFLAG_NOREALLOC)) {
        smid_logf("cpu bgrx region jpeg %dx%d failed: %s\n",
                  width, height, tjGetErrorStr2(enc->tj));
        return -1;
    }
    if (jpeg_ptr != jpeg_out) {
        smid_logf("cpu bgrx region jpeg reallocated unexpectedly\n");
        return -1;
    }
    return 0;
}

int smid_encoder_init(struct smid_encoder *enc, enum smid_encoder_backend backend) {
    return smid_encoder_init_size(enc, backend, SMID_WIDTH, SMID_TILE_H);
}

void smid_encoder_destroy(struct smid_encoder *enc) {
    if (!enc) {
        return;
    }
    if (enc->tj) {
        tjDestroy(enc->tj);
    }
    if (enc->direct) {
        direct_vaapi_destroy(enc->direct);
        free(enc->direct);
    }
    memset(enc, 0, sizeof(*enc));
}

int smid_encoder_encode_rgb(struct smid_encoder *enc, const uint8_t *rgb,
                            int quality,
                            uint8_t *jpeg_out, unsigned long jpeg_cap,
                            unsigned long *jpeg_len_out) {
    quality = clamp_jpeg_quality(quality);
    if (enc->backend == SMID_ENCODER_DIRECT_VAAPI) {
        return direct_vaapi_encode_rgb_into(enc->direct, rgb, quality,
                                            jpeg_out, jpeg_cap, jpeg_len_out);
    }

    uint8_t *jpeg_ptr = jpeg_out;
    *jpeg_len_out = jpeg_cap;
    if (tjCompress2(enc->tj, rgb, enc->width, 0, enc->height, TJPF_RGB,
                    &jpeg_ptr, jpeg_len_out, TJSAMP_420, quality,
                    TJFLAG_FASTDCT | TJFLAG_NOREALLOC)) {
        smid_logf("cpu jpeg failed: %s\n", tjGetErrorStr2(enc->tj));
        return -1;
    }
    if (jpeg_ptr != jpeg_out) {
        smid_logf("cpu jpeg reallocated unexpectedly\n");
        return -1;
    }
    return 0;
}

int smid_encoder_encode_rgb_size(struct smid_encoder *enc, const uint8_t *rgb,
                                 int width, int height, int quality,
                                 uint8_t *jpeg_out, unsigned long jpeg_cap,
                                 unsigned long *jpeg_len_out) {
    if (!enc || !rgb || !jpeg_out || !jpeg_len_out || width <= 0 || height <= 0) {
        return -1;
    }
    quality = clamp_jpeg_quality(quality);
    if (width == enc->width && height == enc->height) {
        return smid_encoder_encode_rgb(enc, rgb, quality, jpeg_out, jpeg_cap, jpeg_len_out);
    }
    if (enc->backend != SMID_ENCODER_CPU) {
        smid_logf("variable-size JPEG encode currently requires cpu backend\n");
        return -1;
    }

    uint8_t *jpeg_ptr = jpeg_out;
    *jpeg_len_out = jpeg_cap;
    if (tjCompress2(enc->tj, rgb, width, 0, height, TJPF_RGB,
                    &jpeg_ptr, jpeg_len_out, TJSAMP_420, quality,
                    TJFLAG_FASTDCT | TJFLAG_NOREALLOC)) {
        smid_logf("cpu jpeg %dx%d failed: %s\n", width, height, tjGetErrorStr2(enc->tj));
        return -1;
    }
    if (jpeg_ptr != jpeg_out) {
        smid_logf("cpu jpeg reallocated unexpectedly\n");
        return -1;
    }
    return 0;
}

int smid_encoder_encode_bgrx(struct smid_encoder *enc, const uint8_t *bgrx,
                             int quality,
                             uint8_t *jpeg_out, unsigned long jpeg_cap,
                             unsigned long *jpeg_len_out) {
    quality = clamp_jpeg_quality(quality);
    if (enc->backend == SMID_ENCODER_DIRECT_VAAPI) {
        return direct_vaapi_encode_bgrx_into(enc->direct, bgrx, quality,
                                             jpeg_out,
                                             jpeg_cap, jpeg_len_out);
    }

    uint8_t *jpeg_ptr = jpeg_out;
    *jpeg_len_out = jpeg_cap;
    if (tjCompress2(enc->tj, bgrx, enc->width, 0, enc->height, TJPF_BGRX,
                    &jpeg_ptr, jpeg_len_out, TJSAMP_420, quality,
                    TJFLAG_FASTDCT | TJFLAG_NOREALLOC)) {
        smid_logf("cpu bgrx jpeg failed: %s\n", tjGetErrorStr2(enc->tj));
        return -1;
    }
    if (jpeg_ptr != jpeg_out) {
        smid_logf("cpu bgrx jpeg reallocated unexpectedly\n");
        return -1;
    }
    return 0;
}

int smid_encoder_encode_bgrx_size(struct smid_encoder *enc, const uint8_t *bgrx,
                                  int width, int height, int quality,
                                  uint8_t *jpeg_out, unsigned long jpeg_cap,
                                  unsigned long *jpeg_len_out) {
    if (!enc || !bgrx || !jpeg_out || !jpeg_len_out || width <= 0 || height <= 0) {
        return -1;
    }
    quality = clamp_jpeg_quality(quality);
    if (width == enc->width && height == enc->height) {
        return smid_encoder_encode_bgrx(enc, bgrx, quality, jpeg_out, jpeg_cap, jpeg_len_out);
    }
    if (enc->backend != SMID_ENCODER_CPU) {
        smid_logf("variable-size BGRX JPEG encode currently requires cpu backend\n");
        return -1;
    }

    uint8_t *jpeg_ptr = jpeg_out;
    *jpeg_len_out = jpeg_cap;
    if (tjCompress2(enc->tj, bgrx, width, 0, height, TJPF_BGRX,
                    &jpeg_ptr, jpeg_len_out, TJSAMP_420, quality,
                    TJFLAG_FASTDCT | TJFLAG_NOREALLOC)) {
        smid_logf("cpu bgrx jpeg %dx%d failed: %s\n",
                  width, height, tjGetErrorStr2(enc->tj));
        return -1;
    }
    if (jpeg_ptr != jpeg_out) {
        smid_logf("cpu bgrx jpeg reallocated unexpectedly\n");
        return -1;
    }
    return 0;
}
