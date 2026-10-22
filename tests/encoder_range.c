/* Check encoded sample levels, not a JPEG viewer's full-range RGB rendering. */
#include "encoder.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <turbojpeg.h>

enum { WIDTH = 1920, HEIGHT = 544, STRIDE = WIDTH * 4 };
static const unsigned char colors[][3] = {
    {0,0,0}, {16,16,16}, {128,128,128}, {235,235,235},
    {240,240,240}, {244,244,244}, {248,248,248}, {252,252,252},
    {254,254,254}, {255,255,255}, {255,0,0}, {0,255,0},
    {0,0,255}, {0,255,255}, {255,0,255}, {255,255,0}
};
static void fill(unsigned char *dst, int stride, int width, int height, int rgb) {
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            const unsigned char *c = colors[x / 32 % 16];
            if (y >= 32 && x % 32 != 16) c = colors[9];
            unsigned char *p = dst + (size_t)y * stride + x * (rgb ? 3 : 4);
            p[0] = c[rgb ? 0 : 2]; p[1] = c[1]; p[2] = c[rgb ? 2 : 0];
            if (!rgb) p[3] = 255;
        }
    }
}
static int writer(void *ctx, uint8_t *dst, int pitch, int width, int height) {
    (void)ctx;
    fill(dst, pitch, width, height, 0);
    return 0;
}
static unsigned images;
static void check(tjhandle dec, unsigned char *jpeg, unsigned long len,
                   int width, int height, int quality, const char *path) {
    int w, h, sub, cs;
    assert(!tjDecompressHeader3(dec, jpeg, len, &w, &h, &sub, &cs));
    assert(w == width && h == height && sub == TJSAMP_420 && cs == TJCS_YCbCr);
    unsigned long capacity = tjBufSizeYUV2(w, 1, h, sub);
    unsigned char *yuv = malloc(capacity);
    assert(yuv);
    assert(!tjDecompressToYUV2(dec, jpeg, len, yuv, w, 1, h,
                              TJFLAG_ACCURATEDCT | TJFLAG_STOPONWARNING));
    int ys = tjPlaneWidth(0, w, sub), cs_stride = tjPlaneWidth(1, w, sub);
    unsigned long ysize = tjPlaneSizeYUV(0, w, 0, h, sub);
    unsigned long csize = tjPlaneSizeYUV(1, w, 0, h, sub);
    int tolerance = quality < 90 ? 4 : 2;
    for (int n = 0; n < width / 32; n++) {
        double r = colors[n % 16][0], g = colors[n % 16][1], b = colors[n % 16][2];
        int expected[] = {
            (int)(16 + (0.299*r + 0.587*g + 0.114*b) * 219/255 + 0.5),
            (int)(128 + (-0.168736*r - 0.331264*g + 0.5*b) * 224/255 + 0.5),
            (int)(128 + (0.5*r - 0.418688*g - 0.081312*b) * 224/255 + 0.5)
        };
        int x = n * 32 + 16, y = 16;
        int actual[] = {yuv[y * ys + x], yuv[ysize + y/2 * cs_stride + x/2],
                        yuv[ysize + csize + y/2 * cs_stride + x/2]};
        for (int c = 0; c < 3; c++) {
            if (abs(actual[c] - expected[c]) > tolerance) {
                fprintf(stderr, "%s Q%d patch%d component%d: got%d expected%d\n",
                        path, quality, n, c, actual[c], expected[c]);
                FILE *f = fopen("build/tests/encoder-range-failure.jpg", "wb");
                if (f) { (void)fwrite(jpeg, 1, len, f); fclose(f); }
                abort();
            }
        }
        if (quality >= 98 && n >= 3 && n <= 8 && height >= 64) {
            int border = yuv[48 * ys + x];
            assert(abs(border - expected[0]) <= 2);
            /* A near-white border must still differ from a white background. */
            if (n <= 7) assert(border < yuv[48 * ys + x + 8]);
        }
    }
    /* Explicit endpoints catch accidental full-range or double range scaling. */
    assert(abs(yuv[16 * ys + 16] - 16) <= 1);
    if (width >= 320) assert(abs(yuv[16 * ys + 9 * 32 + 16] - 235) <= 1);
    images++;
    free(yuv);
}
int main(int argc, char **argv) {
    enum smid_encoder_backend backend = SMID_ENCODER_CPU;
    if (argc == 2 && !strcmp(argv[1], "--vaapi")) backend = SMID_ENCODER_DIRECT_VAAPI;
    else assert(argc == 1);
    struct smid_encoder enc;
    assert(!smid_encoder_init_size(&enc, backend, WIDTH, HEIGHT));
    assert(enc.backend == backend); /* Never count CPU fallback as GPU success. */
    tjhandle dec = tjInitDecompress();
    assert(dec);
    unsigned char *bgrx, *region;
    assert(!posix_memalign((void **)&bgrx, 4096, WIDTH * HEIGHT * 4));
    assert(!posix_memalign((void **)&region, 4096, STRIDE * (HEIGHT + 536)));
    unsigned char rgb[WIDTH * HEIGHT * 3];
    fill(bgrx, WIDTH * 4, WIDTH, HEIGHT, 0);
    fill(rgb, WIDTH * 3, WIDTH, HEIGHT, 1);
    memset(region, 99, STRIDE * (HEIGHT + 536));
    fill(region + 536 * STRIDE, STRIDE, WIDTH, HEIGHT, 0);
    unsigned long cap = smid_encoder_jpeg_capacity();
    unsigned char *jpeg = malloc(cap);
    assert(jpeg);
    const int qualities[] = {98, 70, 100, 98};
    for (unsigned q = 0; q < 4; q++) {
        unsigned long len;
        int quality = qualities[q];
        assert(!smid_encoder_encode_bgrx(&enc, bgrx, quality, jpeg, cap, &len));
        check(dec, jpeg, len, WIDTH, HEIGHT, quality, "bgrx");
        assert(!smid_encoder_encode_rgb(&enc, rgb, quality, jpeg, cap, &len));
        check(dec, jpeg, len, WIDTH, HEIGHT, quality, "rgb");
        assert(!smid_encoder_encode_bgrx_with_writer(&enc, writer, NULL, quality, jpeg, cap, &len));
        check(dec, jpeg, len, WIDTH, HEIGHT, quality, "writer");
        assert(!smid_encoder_encode_bgrx_region(&enc, region, WIDTH, HEIGHT+536, STRIDE,
                 0, 536, WIDTH, HEIGHT, quality, jpeg, cap, &len));
        check(dec, jpeg, len, WIDTH, HEIGHT, quality, "region");
    }
    smid_encoder_destroy(&enc);
    /* The runtime creates a dedicated CPU encoder for every 32x32 companion. */
    assert(!smid_encoder_init_size(&enc, SMID_ENCODER_CPU, 32, 32));
    for (int n = 0; n < 16; n++) {
        memset(bgrx, 0, 32 * 32 * 4);
        for (int p = 0; p < 32 * 32; p++) {
            bgrx[p*4] = bgrx[p*4+1] = bgrx[p*4+2] = (uint8_t)(240+n);
        }
        unsigned long len;
        assert(!smid_encoder_encode_bgrx_region(&enc, bgrx, 32, 32, 128,
                 0, 0, 32, 32, 98, jpeg, cap, &len));
        unsigned char yuv[32*32*3/2];
        assert(!tjDecompressToYUV2(dec, jpeg, len, yuv, 32, 1, 32, TJFLAG_ACCURATEDCT));
        assert(abs(yuv[16*32+16] - (int)(16 + (240+n)*219.0/255 + 0.5)) <= 1);
        assert(yuv[32*32+8*16+8] == 128);
        assert(yuv[32*32*5/4+8*16+8] == 128);
        images++;
    }
    /* Odd dimensions exercise padded YUV planes and buffer growth/reuse. */
    fill(rgb, 65*3, 65, 35, 1);
    unsigned long len;
    assert(!smid_encoder_encode_rgb_size(&enc, rgb, 65, 35, 98, jpeg, cap, &len));
    check(dec, jpeg, len, 65, 35, 98, "rgb-size");
    fill(bgrx, 65*4, 65, 35, 0);
    assert(!smid_encoder_encode_bgrx_size(&enc, bgrx, 65, 35, 98, jpeg, cap, &len));
    check(dec, jpeg, len, 65, 35, 98, "bgrx-size");
    smid_encoder_destroy(&enc);
    tjDestroy(dec);
    free(bgrx); free(region); free(jpeg);
    printf("PASS: %s limited-range levels, colours, borders, crops and CPU companions (%u JPEGs)\n",
           smid_encoder_backend_name(backend), images);
    return 0;
}
