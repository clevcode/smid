/* Standalone exercise of the production encoder. Does not link USB or EVDI. */
#include "encoder.h"
#include "protocol.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <turbojpeg.h>

enum { FRAME_COUNT = 6, STRIDE = SMID_WIDTH * 4 };

/* Large patches expose swapped channels, wrong range, stale input and bad crops.
 * The greys include near-black/white values to expose clipped levels. */
static void fill_pattern(uint8_t *pixels, int frame) {
    static const uint8_t colours[8][3] = {
        {255, 255, 255}, {255, 255, 0}, {0, 255, 255}, {0, 255, 0},
        {255, 0, 255}, {255, 0, 0}, {0, 0, 255}, {0, 0, 0},
    };
    static const uint8_t grey[8] = {0, 16, 32, 64, 128, 192, 235, 255};
    for (int y = 0; y < SMID_STREAM_H; y++) {
        for (int x = 0; x < SMID_WIDTH; x++) {
            unsigned patch = (x / 240 + y / 128 + frame * 3) % 8;
            uint8_t *p = pixels + (size_t)y * STRIDE + x * 4;
            for (int c = 0; c < 3; c++)
                p[c] = frame % 2 ? grey[patch] : colours[patch][2 - c];
            p[3] = 255;
        }
    }
}

static FILE *output_file(const char *dir, const char *name) {
    char path[4096];
    int n = snprintf(path, sizeof(path), "%s/%s", dir, name);
    if (n < 0 || (size_t)n >= sizeof(path)) {
        fprintf(stderr, "output path is too long\n");
        return NULL;
    }
    FILE *f = fopen(path, "wb");
    if (!f)
        perror(path);
    return f;
}

static int save_jpeg(const char *dir, const char *name,
                     const uint8_t *jpeg, unsigned long len) {
    FILE *f = output_file(dir, name);
    if (!f)
        return -1;
    int rc = fwrite(jpeg, 1, len, f) == len ? 0 : -1;
    if (fclose(f))
        rc = -1;
    if (rc)
        fprintf(stderr, "could not write %s\n", name);
    return rc;
}

static int save_reference(const char *dir, const uint8_t *pixels, int frame) {
    char name[64];
    snprintf(name, sizeof(name), "reference-%d.ppm", frame);
    FILE *f = output_file(dir, name);
    if (!f)
        return -1;
    int rc = fprintf(f, "P6\n%d %d\n255\n", SMID_WIDTH, SMID_STREAM_H) < 0 ? -1 : 0;
    for (int y = 0; !rc && y < SMID_STREAM_H; y++) {
        uint8_t row[SMID_WIDTH * 3];
        for (int x = 0; x < SMID_WIDTH; x++) {
            const uint8_t *p = pixels + (size_t)y * STRIDE + x * 4;
            for (int c = 0; c < 3; c++)
                row[x * 3 + c] = p[2 - c];
        }
        if (fwrite(row, 1, sizeof(row), f) != sizeof(row))
            rc = -1;
    }
    if (fclose(f))
        rc = -1;
    return rc;
}

/* Check baseline SOF0 explicitly; the decoder alone also accepts progressive. */
static bool baseline_jpeg(const uint8_t *jpeg, size_t len) {
    if (len < 4 || jpeg[0] != 0xff || jpeg[1] != 0xd8 ||
        jpeg[len - 2] != 0xff || jpeg[len - 1] != 0xd9)
        return false;
    size_t pos = 2;
    while (pos + 4 <= len) {
        if (jpeg[pos++] != 0xff)
            return false;
        uint8_t marker = jpeg[pos++];
        size_t size = (size_t)jpeg[pos] * 256 + jpeg[pos + 1];
        if (size < 2 || size > len - pos)
            return false;
        if (marker == 0xc0)
            return size >= 8 && jpeg[pos + 2] == 8;
        if (marker == 0xda || marker == 0xd9 ||
            (marker >= 0xc1 && marker <= 0xcf && marker != 0xc4 && marker != 0xcc))
            return false;
        pos += size;
    }
    return false;
}

static int check_jpeg(tjhandle decoder, const uint8_t *jpeg, unsigned long len,
                      uint8_t *decoded, const uint8_t *source, int crop_y) {
    int width = 0, height = 0, subsampling = -1, colours = -1;
    if (!baseline_jpeg(jpeg, len)) {
        fprintf(stderr, "FAIL: missing complete 8-bit baseline JPEG\n");
        return -1;
    }
    if (tjDecompressHeader3(decoder, jpeg, len, &width, &height, &subsampling, &colours)) {
        fprintf(stderr, "FAIL: JPEG header: %s\n", tjGetErrorStr2(decoder));
        return -1;
    }
    if (width != SMID_WIDTH || height != SMID_TILE_H ||
        subsampling != TJSAMP_420 || colours != TJCS_YCbCr) {
        fprintf(stderr, "FAIL: unexpected JPEG %dx%d subsampling=%d colourspace=%d\n",
                width, height, subsampling, colours);
        return -1;
    }
    if (tjDecompress2(decoder, jpeg, len, decoded, width, width * 3, height,
                      TJPF_RGB, TJFLAG_ACCURATEDCT | TJFLAG_STOPONWARNING)) {
        fprintf(stderr, "FAIL: JPEG decode: %s\n", tjGetErrorStr2(decoder));
        return -1;
    }

    uint64_t sum[3] = {0};
    unsigned count = 0, max_error = 0;
    for (int y = 8; y < height - 8; y++) {
        int source_y = y + crop_y;
        /* Exclude patch edges where JPEG chroma subsampling is lossy. */
        if (source_y % 128 < 8 || source_y % 128 >= 120)
            continue;
        for (int x = 8; x < width - 8; x++) {
            if (x % 240 < 8 || x % 240 >= 232)
                continue;
            const uint8_t *expected = source + (size_t)source_y * STRIDE + x * 4;
            const uint8_t *actual = decoded + ((size_t)y * width + x) * 3;
            for (int c = 0; c < 3; c++) {
                unsigned error = (unsigned)abs((int)expected[2 - c] - actual[c]);
                sum[c] += error;
                if (error > max_error)
                    max_error = error;
            }
            count++;
        }
    }
    double r = (double)sum[0] / count;
    double g = (double)sum[1] / count;
    double b = (double)sum[2] / count;
    printf("  JPEG %dx%d baseline 4:2:0, %lu bytes; patch MAE RGB %.3f/%.3f/%.3f, max %u\n",
           width, height, len, r, g, b, max_error);
    if (r > 4 || g > 4 || b > 4 || max_error > 12) {
        fprintf(stderr, "FAIL: decoded colours/crop differ from the source\n");
        return -1;
    }
    return 0;
}

static int run_path(const char *dir, enum smid_encoder_backend backend, bool region,
                    uint8_t *frames[2], uint8_t *jpeg, unsigned long capacity,
                    tjhandle decoder, uint8_t *decoded) {
    const char *path = region ? "region" : "copy";
    struct smid_encoder enc[2] = {{0}};
    int rc = -1;
    printf("Testing %s path (%s)\n", path, smid_encoder_backend_name(backend));
    for (int band = 0; band < 2; band++) {
        if (smid_encoder_init(&enc[band], backend)) {
            fprintf(stderr, "FAIL: %s band %d initialization\n", path, band);
            goto out;
        }
        if (enc[band].backend != backend) {
            fprintf(stderr, "FAIL: encoder fell back to CPU; this is NOT a VA-API pass\n");
            goto out;
        }
    }
    for (int frame = 0; frame < FRAME_COUNT; frame++) {
        /* Exercise changed pixels at a cached pointer, then a different pointer. */
        uint8_t *source = frames[(frame / 2) % 2];
        fill_pattern(source, frame);
        if (!region && frame < 2 && save_reference(dir, source, frame))
            goto out;
        int quality = frame % 2 ? 75 : 90;
        for (int band = 0; band < 2; band++) {
            int crop_y = band ? SMID_LOWER_TILE_Y0 : 0;
            unsigned long len = 0;
            printf("%s frame=%d buffer=%d band=%d crop_y=%d quality=%d\n",
                   path, frame, (frame / 2) % 2, band, crop_y, quality);
            int encoded = region ?
                smid_encoder_encode_bgrx_region(&enc[band], source,
                    SMID_WIDTH, SMID_STREAM_H, STRIDE, 0, crop_y,
                    SMID_WIDTH, SMID_TILE_H, quality, jpeg, capacity, &len) :
                smid_encoder_encode_bgrx(&enc[band], source + (size_t)crop_y * STRIDE,
                    quality, jpeg, capacity, &len);
            if (encoded) {
                fprintf(stderr, "FAIL: %s frame=%d band=%d encode\n", path, frame, band);
                goto out;
            }
            /* Preserve even a failed image for inspection. */
            char name[96];
            snprintf(name, sizeof(name), "%s-frame%d-band%d.jpg", path, frame, band);
            if (save_jpeg(dir, name, jpeg, len) ||
                check_jpeg(decoder, jpeg, len, decoded, source, crop_y))
                goto out;
        }
    }
    printf("PASS: %s path, %d JPEGs\n", path, FRAME_COUNT * 2);
    rc = 0;
out:
    for (int band = 0; band < 2; band++)
        smid_encoder_destroy(&enc[band]);
    return rc;
}

int main(int argc, char **argv) {
    bool cpu = argc == 3 && !strcmp(argv[1], "--cpu-self-test");
    if ((!cpu && argc != 2) || (argc > 1 && !strcmp(argv[1], "--help"))) {
        fprintf(stderr, "Usage: %s [--cpu-self-test] NEW_OUTPUT_DIRECTORY\n", argv[0]);
        return 2;
    }
    const char *dir = argv[cpu ? 2 : 1];
    if (mkdir(dir, 0700)) {
        perror(dir);
        fprintf(stderr, "Use a new output directory; existing results are preserved.\n");
        return 2;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("smid encoder probe v1; synthetic input only; no USB or EVDI\n");
    printf("Mode: %s\n", cpu ? "CPU SELF-TEST (no VA-API validation)" : "strict direct-vaapi");
    const char *driver = getenv("LIBVA_DRIVER_NAME");
    printf("LIBVA_DRIVER_NAME=%s\n", driver ? driver : "(unset)");
    printf("Production node search: renderD128, renderD129, card0 (first openable)\n");
    printf("Source: %dx%d BGRX, stride %d, 4096-byte alignment; bands at y=0 and y=%d\n",
           SMID_WIDTH, SMID_STREAM_H, STRIDE, SMID_LOWER_TILE_Y0);

    uint8_t *frames[2] = {NULL, NULL};
    unsigned long capacity = smid_encoder_jpeg_capacity();
    uint8_t *jpeg = capacity == (unsigned long)-1 ? NULL : malloc(capacity);
    uint8_t *decoded = malloc((size_t)SMID_WIDTH * SMID_TILE_H * 3);
    tjhandle decoder = tjInitDecompress();
    int rc = 1;
    for (int i = 0; i < 2; i++) {
        int error = posix_memalign((void **)&frames[i], 4096, (size_t)STRIDE * SMID_STREAM_H);
        if (error) {
            fprintf(stderr, "allocation: %s\n", strerror(error));
            goto out;
        }
    }
    if (!jpeg || !decoded || !decoder) {
        fprintf(stderr, "allocation/decoder initialization failed\n");
        goto out;
    }
    enum smid_encoder_backend backend = cpu ? SMID_ENCODER_CPU : SMID_ENCODER_DIRECT_VAAPI;
    /* Try the runtime USER_PTR path even when the diagnostic copy path fails. */
    int copy_rc = run_path(dir, backend, false, frames, jpeg, capacity, decoder, decoded);
    int region_rc = run_path(dir, backend, true, frames, jpeg, capacity, decoder, decoded);
    if (!copy_rc && !region_rc) {
        puts(cpu ? "SELF-TEST PASS: CPU harness only; VA-API NOT tested" :
             "PROBE PASS: copy and USER_PTR region conversion/encoding/colour checks passed");
        puts("This is not a USB compatibility test or a performance benchmark.");
        rc = 0;
    } else {
        printf("PROBE FAIL: copy=%s region=%s; please send the complete log and images\n",
               copy_rc ? "FAIL" : "PASS", region_rc ? "FAIL" : "PASS");
    }
out:
    if (decoder)
        tjDestroy(decoder);
    free(decoded);
    free(jpeg);
    free(frames[0]);
    free(frames[1]);
    return rc;
}
