#include "evdi_source.h"

#include "log.h"
#include "protocol.h"
#include "time.h"
#include "transport.h"
#include "usb.h"

#include <errno.h>
#include <evdi_lib.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

#if (defined(__x86_64__) || defined(__i386__)) && defined(__GNUC__)
#include <immintrin.h>
#define SMID_HAVE_X86_TARGET_INTRINSICS 1
#endif

enum {
    SMID_EVDI_MAX_STREAMS = 2,
    SMID_EVDI_BUFFER_ID = 1,
    SMID_EVDI_MAX_RECTS = 64,
    SMID_CURSOR_IMAGE_LEN = 0x4060u,
    SMID_CURSOR_IMAGE_BYTES = 0x4000u,
    SMID_CURSOR_IMAGE_OFF = 0x60u,
    SMID_CURSOR_SIDE = 64u,
    SMID_CURSOR_STRIDE = 0x100u,
    SMID_CURSOR_CACHE_MAX = 0x190u,
    SMID_CURSOR_CACHE_REWIND = 0x64u,
};

typedef struct smid_frame_hash (*smid_frame_hash_fn)(const uint8_t *data, size_t len);

struct smid_frame_hash_impl {
    smid_frame_hash_fn fn;
};

struct smid_cursor_cache_entry {
    bool valid;
    uint32_t shape;
    uint32_t hash;
    uint32_t width;
    uint32_t height;
    uint32_t hot_x;
    uint32_t hot_y;
    uint32_t pixel_format;
    uint32_t stride;
    uint8_t image[SMID_CURSOR_IMAGE_BYTES];
};

struct smid_evdi_stream {
    struct smid_evdi_source *source;
    int index;
    evdi_handle handle;
    pthread_t thread;
    pthread_rwlock_t buffer_lock;
    bool buffer_lock_initialized;
    bool thread_started;
    atomic_bool stop;
    int wake_pipe[2];
    struct evdi_event_context events;
    struct evdi_rect rects[SMID_EVDI_MAX_RECTS];
    atomic_bool dpms_on;
    atomic_bool mode_seen;
    atomic_bool requested;
    int width;
    int height;
    int stride;
    size_t buffer_size;
    uint8_t *buffer;
    struct smid_frame_hash last_band_hash[2];
    bool have_band_hash[2];
    uint8_t cursor_image_pkt[SMID_CURSOR_IMAGE_LEN];
    uint8_t cursor_select_pkt[48];
    uint8_t cursor_state_pkt[44];
    struct smid_tx_result cursor_upload_result;
    uint32_t cursor_shape_slot;
    uint32_t cursor_upload_shape;
    uint64_t cursor_upload_code_before;
    uint64_t cursor_upload_start_us;
    int32_t cursor_x;
    int32_t cursor_y;
    bool cursor_visible;
    bool cursor_upload_result_ready;
    atomic_bool cursor_upload_inflight;
    bool cursor_upload_tx_done;
    bool pending_cursor_set;
    bool pending_cursor_enabled;
    uint32_t pending_cursor_width;
    uint32_t pending_cursor_height;
    int32_t pending_cursor_hot_x;
    int32_t pending_cursor_hot_y;
    uint32_t pending_cursor_pixel_format;
    uint32_t pending_cursor_stride;
    uint8_t pending_cursor_image[SMID_CURSOR_IMAGE_BYTES];
    bool pending_cursor_move;
    int32_t pending_cursor_x;
    int32_t pending_cursor_y;
    atomic_uchar dirty_tiles;
    atomic_uchar blank_tiles;
    atomic_uint_fast64_t stat_dirty;
};

struct smid_evdi_source {
    int streams;
    bool cursor_events;
    smid_frame_hash_fn frame_hash_fn;
    struct smid_usb *usb;
    pthread_mutex_t usb_lock;
    pthread_mutex_t damage_lock;
    pthread_cond_t damage_cond;
    pthread_mutex_t cursor_lock;
    pthread_mutex_t cursor_event_lock;
    pthread_cond_t cursor_event_cond;
    pthread_t cursor_thread;
    bool cursor_thread_started;
    atomic_bool cursor_stop;
    atomic_bool device_power_on;
    uint64_t damage_generation;
    int cursor_selected_stream;
    uint32_t cursor_selected_shape[SMID_EVDI_MAX_STREAMS];
    uint32_t cursor_next_shape;
    struct smid_cursor_cache_entry cursor_cache[SMID_CURSOR_CACHE_MAX + 1u];
    struct smid_evdi_stream stream[SMID_EVDI_MAX_STREAMS];
};

static bool evdi_request_next(struct smid_evdi_stream *s);
static void stream_stop(struct smid_evdi_stream *s);

static void signal_damage_locked(struct smid_evdi_source *src) {
    src->damage_generation++;
    pthread_cond_broadcast(&src->damage_cond);
}

static void signal_damage(struct smid_evdi_source *src) {
    pthread_mutex_lock(&src->damage_lock);
    signal_damage_locked(src);
    pthread_mutex_unlock(&src->damage_lock);
}

static struct smid_usb *source_usb_locked(struct smid_evdi_source *src) {
    pthread_mutex_lock(&src->usb_lock);
    struct smid_usb *usb = src->usb;
    pthread_mutex_unlock(&src->usb_lock);
    return usb;
}

static void make_device_power_packet(uint8_t packet[44], bool on) {
    memset(packet, 0, 44);
    memcpy(packet, SMID_MAGIC, 12);
    smid_put32(packet + 12, 44);
    smid_put32(packet + 16, 6);
    packet[20] = 0x47;
    packet[21] = on ? 1u : 0u;
}

static int submit_device_power(struct smid_evdi_source *src, bool on) {
    struct smid_usb *usb = source_usb_locked(src);
    if (!usb) {
        return 0;
    }
    uint8_t packet[44];
    make_device_power_packet(packet, on);
    struct smid_tx_item item = {
        .priority = SMID_TX_PRIO_CONTROL,
        .endpoint = SMID_EP_BULK_OUT,
        .xfer = SMID_USB_XFER_BULK,
        .timeout_ms = 1000,
        .data = packet,
        .len = sizeof(packet),
        .name = on ? "display-power-on" : "display-power-off",
    };
    return smid_transport_submit(&usb->tx, &item);
}

static void update_device_power(struct smid_evdi_source *src) {
    bool any_on = false;
    for (int i = 0; i < src->streams; i++) {
        any_on = any_on ||
                 atomic_load_explicit(&src->stream[i].dpms_on, memory_order_acquire);
    }
    bool old = atomic_exchange_explicit(&src->device_power_on, any_on,
                                        memory_order_acq_rel);
    if (old != any_on && submit_device_power(src, any_on)) {
        smid_logf("evdi: display power %s enqueue failed\n", any_on ? "on" : "off");
    }
}

void smid_evdi_source_set_usb(struct smid_evdi_source *src, struct smid_usb *usb) {
    if (!src) {
        return;
    }
    pthread_mutex_lock(&src->usb_lock);
    src->usb = usb;
    pthread_mutex_unlock(&src->usb_lock);
    if (usb) {
        atomic_store_explicit(&src->device_power_on, true, memory_order_release);
        update_device_power(src);
    }
    pthread_mutex_lock(&src->cursor_event_lock);
    pthread_cond_broadcast(&src->cursor_event_cond);
    pthread_mutex_unlock(&src->cursor_event_lock);
}

static uint64_t atomic_take_u64(atomic_uint_fast64_t *v) {
    return atomic_exchange_explicit(v, 0, memory_order_relaxed);
}

static uint32_t fnv1a32_update(uint32_t h, const void *data, size_t len) {
    const uint8_t *p = data;
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

static uint64_t rotl64(uint64_t v, unsigned n) {
    return (v << n) | (v >> (64u - n));
}

static uint64_t mix64(uint64_t v) {
    v ^= v >> 33;
    v *= 0xff51afd7ed558ccdull;
    v ^= v >> 33;
    v *= 0xc4ceb9fe1a85ec53ull;
    v ^= v >> 33;
    return v;
}

static struct smid_frame_hash frame_hash_scalar(const uint8_t *data, size_t len) {
    uint64_t sum = 0x9e3779b97f4a7c15ull ^ (uint64_t)len;
    uint64_t sum2 = 0x632be59bd9b4e019ull + ((uint64_t)len << 1);
    uint64_t x = 0x94d049bb133111ebull ^ ((uint64_t)len << 32);
    size_t i = 0;
    for (; i + sizeof(uint64_t) <= len; i += sizeof(uint64_t)) {
        uint64_t v = 0;
        memcpy(&v, data + i, sizeof(v));
        sum += v;
        sum2 += sum;
        x ^= rotl64(v, 17);
    }
    uint64_t tail = 0;
    unsigned shift = 0;
    for (; i < len; i++, shift += 8) {
        tail |= (uint64_t)data[i] << shift;
    }
    sum += tail;
    sum2 += sum;
    x ^= rotl64(tail, 17);
    struct smid_frame_hash out = {
        .a = mix64(sum ^ rotl64(sum2, 23) ^ x),
        .b = mix64(sum2 ^ rotl64(x, 41) ^ ((uint64_t)len * 0x9e3779b185ebca87ull)),
    };
    return out;
}

#if SMID_HAVE_X86_TARGET_INTRINSICS
__attribute__((target("avx2"), always_inline))
static inline __m256i loadu256_once(const void *p) {
    __m256i v = _mm256_loadu_si256((const __m256i *)p);
    __asm__("" : "+x"(v));
    return v;
}

__attribute__((target("avx2"), always_inline))
static inline uint64_t hsum256_epi64(__m256i v) {
    __m128i lo = _mm256_castsi256_si128(v);
    __m128i hi = _mm256_extracti128_si256(v, 1);
    __m128i s = _mm_add_epi64(lo, hi);
    s = _mm_add_epi64(s, _mm_unpackhi_epi64(s, s));
    return (uint64_t)_mm_cvtsi128_si64(s);
}

__attribute__((target("avx2"), always_inline))
static inline uint64_t hxor256_epi64(__m256i v) {
    __m128i lo = _mm256_castsi256_si128(v);
    __m128i hi = _mm256_extracti128_si256(v, 1);
    __m128i x = _mm_xor_si128(lo, hi);
    x = _mm_xor_si128(x, _mm_unpackhi_epi64(x, x));
    return (uint64_t)_mm_cvtsi128_si64(x);
}

__attribute__((target("avx2")))
static struct smid_frame_hash frame_hash_avx2(const uint8_t *data, size_t len) {
    const __m256i c0 = _mm256_set_epi64x(0x94d049bb133111ebull, 0x632be59bd9b4e019ull,
                                         0xbf58476d1ce4e5b9ull, 0x9e3779b97f4a7c15ull);
    const __m256i c1 = _mm256_set_epi64x(0x8ebc6af09c88c6e3ull, 0x589965cc75374cc3ull,
                                         0x1d8e4e27c47d124full, 0xc2b2ae3d27d4eb4full);
    const __m256i c2 = _mm256_set_epi64x(0x165667b19e3779f9ull, 0xd6e8feb86659fd93ull,
                                         0xa5a3564e27f886c7ull, 0x85ebca77c2b2ae63ull);
    const __m256i c3 = _mm256_set_epi64x(0x27d4eb2f165667c5ull, 0x9e3779b185ebca87ull,
                                         0xc4ceb9fe1a85ec53ull, 0xff51afd7ed558ccdull);
    __m256i sum0 = _mm256_xor_si256(c0, _mm256_set1_epi64x((long long)len));
    __m256i sum1 = _mm256_xor_si256(c1, _mm256_set1_epi64x((long long)((uint64_t)len << 1)));
    __m256i sum2 = _mm256_xor_si256(c2, _mm256_set1_epi64x((long long)((uint64_t)len << 2)));
    __m256i sum3 = _mm256_xor_si256(c3, _mm256_set1_epi64x((long long)((uint64_t)len << 3)));
    __m256i xor0 = c3;
    __m256i xor1 = c2;
    __m256i xor2 = c1;
    __m256i xor3 = c0;
    xor0 = _mm256_xor_si256(xor0, _mm256_set1_epi64x((long long)((uint64_t)len << 32)));
    size_t i = 0;
    for (; i + 256u <= len; i += 256u) {
        __m256i v0 = loadu256_once(data + i + 0u);
        __m256i v1 = loadu256_once(data + i + 32u);
        __m256i v2 = loadu256_once(data + i + 64u);
        __m256i v3 = loadu256_once(data + i + 96u);
        __m256i v4 = loadu256_once(data + i + 128u);
        __m256i v5 = loadu256_once(data + i + 160u);
        __m256i v6 = loadu256_once(data + i + 192u);
        __m256i v7 = loadu256_once(data + i + 224u);
        sum0 = _mm256_add_epi64(sum0, v0);
        sum1 = _mm256_add_epi64(sum1, v1);
        sum2 = _mm256_add_epi64(sum2, v2);
        sum3 = _mm256_add_epi64(sum3, v3);
        sum0 = _mm256_add_epi64(sum0, v4);
        sum1 = _mm256_add_epi64(sum1, v5);
        sum2 = _mm256_add_epi64(sum2, v6);
        sum3 = _mm256_add_epi64(sum3, v7);
        xor0 = _mm256_xor_si256(xor0, v0);
        xor1 = _mm256_xor_si256(xor1, v1);
        xor2 = _mm256_xor_si256(xor2, v2);
        xor3 = _mm256_xor_si256(xor3, v3);
        xor0 = _mm256_xor_si256(xor0, v4);
        xor1 = _mm256_xor_si256(xor1, v5);
        xor2 = _mm256_xor_si256(xor2, v6);
        xor3 = _mm256_xor_si256(xor3, v7);
    }
    for (; i + 32u <= len; i += 32u) {
        __m256i v = loadu256_once(data + i);
        sum0 = _mm256_add_epi64(sum0, v);
        xor0 = _mm256_xor_si256(xor0, v);
    }

    __m256i sum01 = _mm256_add_epi64(sum0, sum1);
    __m256i sum23 = _mm256_add_epi64(sum2, sum3);
    __m256i xor01 = _mm256_xor_si256(xor0, xor1);
    __m256i xor23 = _mm256_xor_si256(xor2, xor3);
    uint64_t vec_sum = hsum256_epi64(_mm256_add_epi64(sum01, sum23));
    uint64_t vec_x = hxor256_epi64(_mm256_xor_si256(xor01, xor23));
    uint64_t a = mix64(vec_sum ^ rotl64(vec_x, 17) ^
                       ((uint64_t)len * 0x9e3779b185ebca87ull));
    uint64_t b = mix64(vec_x ^ rotl64(vec_sum, 29) ^
                       ((uint64_t)len * 0xc2b2ae3d27d4eb4full));

    uint64_t tail_sum = 0;
    uint64_t tail_sum2 = 0;
    uint64_t tail_x = 0;
    for (; i + sizeof(uint64_t) <= len; i += sizeof(uint64_t)) {
        uint64_t v = 0;
        memcpy(&v, data + i, sizeof(v));
        tail_sum += v;
        tail_sum2 += tail_sum;
        tail_x ^= rotl64(v, 17);
    }
    uint64_t tail = 0;
    unsigned shift = 0;
    for (; i < len; i++, shift += 8) {
        tail |= (uint64_t)data[i] << shift;
    }
    tail_sum += tail;
    tail_sum2 += tail_sum;
    tail_x ^= rotl64(tail, 17);

    struct smid_frame_hash out = {
        .a = mix64(a ^ tail_sum ^ rotl64(tail_sum2, 23) ^ tail_x),
        .b = mix64(b ^ tail_sum2 ^ rotl64(tail_x, 41)),
    };
    return out;
}
#endif

static struct smid_frame_hash_impl frame_hash_resolve(void) {
#if SMID_HAVE_X86_TARGET_INTRINSICS
    __builtin_cpu_init();
    if (__builtin_cpu_supports("avx2")) {
        return (struct smid_frame_hash_impl){
            .fn = frame_hash_avx2,
        };
    }
#endif
    return (struct smid_frame_hash_impl){
        .fn = frame_hash_scalar,
    };
}

static bool frame_hash_equal(struct smid_frame_hash a, struct smid_frame_hash b) {
    return a.a == b.a && a.b == b.b;
}

static uint32_t cursor_image_hash(const struct evdi_cursor_set *cursor_set,
                                  const uint8_t *image) {
    uint32_t h = 2166136261u;
    h = fnv1a32_update(h, &cursor_set->width, sizeof(cursor_set->width));
    h = fnv1a32_update(h, &cursor_set->height, sizeof(cursor_set->height));
    h = fnv1a32_update(h, &cursor_set->hot_x, sizeof(cursor_set->hot_x));
    h = fnv1a32_update(h, &cursor_set->hot_y, sizeof(cursor_set->hot_y));
    h = fnv1a32_update(h, &cursor_set->pixel_format, sizeof(cursor_set->pixel_format));
    h = fnv1a32_update(h, &cursor_set->stride, sizeof(cursor_set->stride));
    return fnv1a32_update(h, image, SMID_CURSOR_IMAGE_BYTES);
}

static bool cursor_cache_matches(const struct smid_cursor_cache_entry *e,
                                 const struct evdi_cursor_set *cursor_set,
                                 const uint8_t *image, uint32_t hash) {
    return e->valid &&
           e->hash == hash &&
           e->width == cursor_set->width &&
           e->height == cursor_set->height &&
           e->hot_x == (uint32_t)cursor_set->hot_x &&
           e->hot_y == (uint32_t)cursor_set->hot_y &&
           e->pixel_format == cursor_set->pixel_format &&
           e->stride == cursor_set->stride &&
           !memcmp(e->image, image, SMID_CURSOR_IMAGE_BYTES);
}

static bool cursor_cache_matches_source(const struct smid_cursor_cache_entry *e,
                                        const struct evdi_cursor_set *cursor_set) {
    if (!e->valid ||
        e->width != cursor_set->width ||
        e->height != cursor_set->height ||
        e->hot_x != (uint32_t)cursor_set->hot_x ||
        e->hot_y != (uint32_t)cursor_set->hot_y ||
        e->pixel_format != cursor_set->pixel_format ||
        e->stride != cursor_set->stride ||
        !cursor_set->buffer) {
        return false;
    }
    const uint8_t *src = (const uint8_t *)cursor_set->buffer;
    uint32_t src_stride = cursor_set->stride ? cursor_set->stride : cursor_set->width * 4u;
    uint32_t copy_row = cursor_set->width * 4u;
    if (copy_row > src_stride) {
        copy_row = src_stride;
    }
    if (copy_row > SMID_CURSOR_STRIDE) {
        copy_row = SMID_CURSOR_STRIDE;
    }
    for (uint32_t y = 0; y < cursor_set->height; y++) {
        if (memcmp(e->image + (size_t)y * SMID_CURSOR_STRIDE,
                   src + (size_t)y * src_stride, copy_row)) {
            return false;
        }
    }
    return true;
}

static void make_1080p_edid(uint8_t edid[128], int index) {
    static const uint8_t tmpl[128] = {
        0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00,
        0x4f, 0x53, 0x01, 0x10, 0x01, 0x00, 0x00, 0x00,
        0x01, 0x24, 0x01, 0x03, 0x80, 0x34, 0x1d, 0x78,
        0x0a, 0xee, 0x91, 0xa3, 0x54, 0x4c, 0x99, 0x26,
        0x0f, 0x50, 0x54, 0x21, 0x08, 0x00, 0x81, 0x80,
        0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
        0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x02, 0x3a,
        0x80, 0x18, 0x71, 0x38, 0x2d, 0x40, 0x58, 0x2c,
        0x45, 0x00, 0x09, 0x25, 0x21, 0x00, 0x00, 0x1e,
        0x00, 0x00, 0x00, 0xfd, 0x00, 0x32, 0x4b, 0x1e,
        0x53, 0x11, 0x00, 0x0a, 0x20, 0x20, 0x20, 0x20,
        0x20, 0x20, 0x00, 0x00, 0x00, 0xfc, 0x00, 0x53,
        0x4d, 0x49, 0x20, 0x56, 0x69, 0x72, 0x74, 0x20,
        0x41, 0x0a, 0x20, 0x20, 0x00, 0x00, 0x00, 0xff,
        0x00, 0x53, 0x4d, 0x49, 0x45, 0x56, 0x44, 0x49,
        0x30, 0x30, 0x31, 0x0a, 0x20, 0x20, 0x00, 0x00,
    };
    memcpy(edid, tmpl, sizeof(tmpl));
    edid[12] = (uint8_t)(1 + index);
    edid[104] = (uint8_t)('A' + index);
    edid[122] = (uint8_t)('1' + index);
    edid[127] = 0;
    uint8_t sum = 0;
    for (int i = 0; i < 127; i++) {
        sum = (uint8_t)(sum + edid[i]);
    }
    edid[127] = (uint8_t)(0u - sum);
}

static const uint8_t *band_data_locked(struct smid_evdi_stream *s, int tile,
                                       size_t *bytes_out) {
    int y0 = tile ? 0 : SMID_LOWER_TILE_Y0;
    if (bytes_out) {
        *bytes_out = 0;
    }
    if (!s->buffer || s->stride <= 0 || y0 < 0 || y0 >= s->height) {
        return NULL;
    }
    int rows = SMID_TILE_H;
    if (y0 + rows > s->height) {
        rows = s->height - y0;
    }
    if (rows <= 0) {
        return NULL;
    }
    if (bytes_out) {
        *bytes_out = (size_t)rows * (size_t)s->stride;
    }
    return s->buffer + (size_t)y0 * (size_t)s->stride;
}

static uint8_t screen_dirty_mask_locked(struct smid_evdi_stream *s) {
    if (!s->buffer || s->width <= 0 || s->height <= 0) {
        return 0;
    }
    uint8_t dirty = 0;
    for (int tile = 0; tile < 2; tile++) {
        size_t band_bytes = 0;
        const uint8_t *band = band_data_locked(s, tile, &band_bytes);
        if (!band || !band_bytes) {
            continue;
        }
        struct smid_frame_hash h = s->source->frame_hash_fn(band, band_bytes);
        if (!s->have_band_hash[tile] ||
            !frame_hash_equal(h, s->last_band_hash[tile])) {
            dirty |= (uint8_t)(1u << tile);
        }
        s->last_band_hash[tile] = h;
        s->have_band_hash[tile] = true;
    }
    return dirty;
}

static int evdi_alloc_buffer(struct smid_evdi_stream *s, struct evdi_mode mode) {
    int stride = ((mode.width + 63) & ~63) * 4;
    size_t buffer_size = (size_t)stride * (size_t)mode.height;
    uint8_t *buffer = NULL;
    if (posix_memalign((void **)&buffer, 4096, buffer_size)) {
        buffer = NULL;
    }
    if (buffer) {
        memset(buffer, 0, buffer_size);
    }
    if (!buffer) {
        free(buffer);
        smid_logf("evdi%d: framebuffer allocation failed\n", s->index);
        return -1;
    }

    pthread_rwlock_wrlock(&s->buffer_lock);
    if (s->buffer) {
        evdi_unregister_buffer(s->handle, SMID_EVDI_BUFFER_ID);
        free(s->buffer);
    }
    s->width = mode.width;
    s->height = mode.height;
    s->stride = stride;
    s->buffer_size = buffer_size;
    s->buffer = buffer;
    s->last_band_hash[0] = (struct smid_frame_hash){0, 0};
    s->last_band_hash[1] = (struct smid_frame_hash){0, 0};
    s->have_band_hash[0] = false;
    s->have_band_hash[1] = false;
    struct evdi_buffer b = {
        .id = SMID_EVDI_BUFFER_ID,
        .buffer = s->buffer,
        .width = mode.width,
        .height = mode.height,
        .stride = s->stride,
        .rects = s->rects,
        .rect_count = SMID_EVDI_MAX_RECTS,
    };
    evdi_register_buffer(s->handle, b);
    atomic_store_explicit(&s->requested, false, memory_order_release);
    atomic_store_explicit(&s->dirty_tiles, 0x3, memory_order_release);
    atomic_store_explicit(&s->mode_seen, true, memory_order_release);
    pthread_rwlock_unlock(&s->buffer_lock);

    smid_logf("evdi%d: mode %dx%d@%d bpp=%d fmt=0x%x stride=%d\n",
            s->index, mode.width, mode.height, mode.refresh_rate,
            mode.bits_per_pixel, mode.pixel_format, s->stride);
    return 0;
}

static void evdi_mode_handler(struct evdi_mode mode, void *user_data) {
    (void)evdi_alloc_buffer((struct smid_evdi_stream *)user_data, mode);
}

static void cursor_init_stream(struct smid_evdi_stream *s) {
    uint8_t selector = s->index ? 0x10 : 0x00;

    memset(s->cursor_image_pkt, 0, sizeof(s->cursor_image_pkt));
    memcpy(s->cursor_image_pkt, SMID_MAGIC, 12);
    smid_put32(s->cursor_image_pkt + 12, SMID_CURSOR_IMAGE_LEN);
    smid_put32(s->cursor_image_pkt + 16, 6u | ((uint32_t)selector << 24));
    s->cursor_image_pkt[20] = 0x42;
    s->cursor_image_pkt[21] = 0x30;
    s->cursor_image_pkt[22] = 0x40;
    smid_put32(s->cursor_image_pkt + 48, 2);
    smid_put32(s->cursor_image_pkt + 52, SMID_CURSOR_SIDE);
    smid_put32(s->cursor_image_pkt + 56, SMID_CURSOR_SIDE);
    smid_put32(s->cursor_image_pkt + 60, SMID_CURSOR_STRIDE);
    smid_put32(s->cursor_image_pkt + 88, SMID_CURSOR_IMAGE_BYTES);

    memset(s->cursor_select_pkt, 0, sizeof(s->cursor_select_pkt));
    memcpy(s->cursor_select_pkt, SMID_MAGIC, 12);
    smid_put32(s->cursor_select_pkt + 12, sizeof(s->cursor_select_pkt));
    smid_put32(s->cursor_select_pkt + 16, 0x00400039u);
    s->cursor_select_pkt[21] = (uint8_t)(s->index & 1);
    s->cursor_select_pkt[26] = 1;

    memset(s->cursor_state_pkt, 0, sizeof(s->cursor_state_pkt));
    memcpy(s->cursor_state_pkt, SMID_MAGIC, 12);
    smid_put32(s->cursor_state_pkt + 12, sizeof(s->cursor_state_pkt));
    s->cursor_state_pkt[16] = 0x3a;
    smid_put32(s->cursor_state_pkt + 17, 0x0c);
    s->cursor_state_pkt[21] = 0;
    s->cursor_shape_slot = UINT32_MAX;
    s->cursor_upload_shape = UINT32_MAX;
    atomic_init(&s->cursor_upload_inflight, false);
    smid_tx_result_init(&s->cursor_upload_result);
    s->cursor_upload_result_ready = true;
}

static int cursor_submit_inline(struct smid_evdi_source *src, const uint8_t *data, uint32_t len,
                                enum smid_tx_priority priority, uint8_t endpoint,
                                enum smid_usb_xfer xfer, uint32_t timeout_ms,
                                const char *name) {
    if (len > SMID_TX_INLINE_CAP) {
        return -1;
    }
    pthread_mutex_lock(&src->usb_lock);
    struct smid_usb *usb = src->usb;
    if (!usb) {
        pthread_mutex_unlock(&src->usb_lock);
        return -1;
    }
    struct smid_tx_item item = {
        .priority = priority,
        .endpoint = endpoint,
        .xfer = xfer,
        .timeout_ms = timeout_ms,
        .data = data,
        .len = len,
        .result = NULL,
        .name = name,
    };
    int rc = smid_transport_submit(&usb->tx, &item);
    pthread_mutex_unlock(&src->usb_lock);
    return rc;
}

static int cursor_select_locked(struct smid_evdi_stream *s) {
    struct smid_evdi_source *src = s->source;
    int target = s->index;
    uint32_t shape = s->cursor_shape_slot;
    if (shape == UINT32_MAX) {
        return 0;
    }
    if (atomic_load_explicit(&s->cursor_upload_inflight, memory_order_acquire) &&
        s->cursor_upload_shape == shape) {
        return 0;
    }
    if (src->cursor_selected_stream == target &&
        src->cursor_selected_shape[target & 1] == shape) {
        return 0;
    }

    s->cursor_select_pkt[21] = (uint8_t)(target & 1);
    smid_put32(s->cursor_select_pkt + 22, shape);
    s->cursor_select_pkt[26] = 1;
    int rc = cursor_submit_inline(src, s->cursor_select_pkt, sizeof(s->cursor_select_pkt),
                                  SMID_TX_PRIO_CURSOR, SMID_EP_INTR_OUT, SMID_USB_XFER_INTR,
                                  250, "cursor-select");
    if (!rc) {
        src->cursor_selected_stream = target;
        src->cursor_selected_shape[target & 1] = shape;
    }
    return rc;
}

static int cursor_submit_state_locked(struct smid_evdi_stream *s, int32_t x, int32_t y,
                                      bool enabled, bool force) {
    struct smid_evdi_source *src = s->source;
    bool same_state = s->cursor_x == x && s->cursor_y == y && s->cursor_visible == enabled;
    int rc = 0;
    if (enabled) {
        rc = cursor_select_locked(s);
    }
    if (!rc && (force || !same_state)) {
        int target = s->index;
        s->cursor_x = x;
        s->cursor_y = y;
        s->cursor_visible = enabled;
        s->cursor_state_pkt[21] = (uint8_t)(target & 1);
        s->cursor_state_pkt[32] = enabled ? 1 : 0;
        smid_put32(s->cursor_state_pkt + 36, (uint32_t)x);
        smid_put32(s->cursor_state_pkt + 40, (uint32_t)y);
        rc = cursor_submit_inline(src, s->cursor_state_pkt, sizeof(s->cursor_state_pkt),
                                  SMID_TX_PRIO_CURSOR, SMID_EP_INTR_OUT, SMID_USB_XFER_INTR,
                                  250, "cursor-state");
    }
    return rc;
}

static int cursor_send_state(struct smid_evdi_stream *s, int32_t x, int32_t y, bool enabled) {
    struct smid_evdi_source *src = s->source;

    pthread_mutex_lock(&src->cursor_lock);
    int rc = cursor_submit_state_locked(s, x, y, enabled, false);
    pthread_mutex_unlock(&src->cursor_lock);
    return rc;
}

static int cursor_send_state_force(struct smid_evdi_stream *s, int32_t x, int32_t y, bool enabled) {
    struct smid_evdi_source *src = s->source;

    pthread_mutex_lock(&src->cursor_lock);
    int rc = cursor_submit_state_locked(s, x, y, enabled, true);
    pthread_mutex_unlock(&src->cursor_lock);
    return rc;
}

static void cursor_upload_fail_locked(struct smid_evdi_stream *s, const char *why) {
    if (s->cursor_upload_shape <= SMID_CURSOR_CACHE_MAX) {
        s->source->cursor_cache[s->cursor_upload_shape].valid = false;
    }
    smid_logf("warn: cursor image upload failed stream=%d shape=%u %s\n",
            s->index, s->cursor_upload_shape, why);
    atomic_store_explicit(&s->cursor_upload_inflight, false, memory_order_release);
    s->cursor_upload_tx_done = false;
    s->cursor_upload_shape = UINT32_MAX;
}

static void cursor_pump_locked(struct smid_evdi_stream *s) {
    if (!atomic_load_explicit(&s->cursor_upload_inflight, memory_order_acquire)) {
        return;
    }

    if (!s->cursor_upload_tx_done) {
        int rc = 0;
        int transferred = 0;
        if (!smid_tx_result_poll(&s->cursor_upload_result, &rc, &transferred)) {
            return;
        }
        if (rc || transferred != (int)SMID_CURSOR_IMAGE_LEN) {
            cursor_upload_fail_locked(s, "tx");
            return;
        }
        s->cursor_upload_tx_done = true;
    }

    uint8_t status = 0xff;
    pthread_mutex_lock(&s->source->usb_lock);
    struct smid_usb *usb = s->source->usb;
    bool seen = usb && smid_usb_code_seen_after(usb, 0x42, s->cursor_upload_code_before, &status);
    pthread_mutex_unlock(&s->source->usb_lock);
    if (!seen) {
        if (smid_mono_us() - s->cursor_upload_start_us > 1000000u) {
            cursor_upload_fail_locked(s, "response-timeout");
        }
        return;
    }
    if (status) {
        cursor_upload_fail_locked(s, "status");
        return;
    }

    uint32_t shape = s->cursor_upload_shape;
    atomic_store_explicit(&s->cursor_upload_inflight, false, memory_order_release);
    s->cursor_upload_tx_done = false;
    s->cursor_upload_shape = UINT32_MAX;
    if (s->cursor_shape_slot == shape) {
        int rc = cursor_select_locked(s);
        if (!rc && s->cursor_visible) {
            (void)cursor_submit_state_locked(s, s->cursor_x, s->cursor_y, true, true);
        }
    }
}

static void cursor_pump_stream(struct smid_evdi_stream *s) {
    struct smid_evdi_source *src = s->source;
    if (!source_usb_locked(src) ||
        !atomic_load_explicit(&s->cursor_upload_inflight, memory_order_acquire)) {
        return;
    }
    pthread_mutex_lock(&src->cursor_lock);
    cursor_pump_locked(s);
    pthread_mutex_unlock(&src->cursor_lock);
}

static int cursor_send_image_ex(struct smid_evdi_stream *s, struct evdi_cursor_set *cursor_set,
                                bool force_upload) {
    struct smid_evdi_source *src = s->source;
    if (!cursor_set->enabled || !cursor_set->buffer ||
        cursor_set->width == 0 || cursor_set->height == 0) {
        return cursor_send_state(s, 0, 0, false);
    }
    if (cursor_set->width > SMID_CURSOR_SIDE || cursor_set->height > SMID_CURSOR_SIDE) {
        smid_logf("cursor stream=%d unsupported size %ux%u\n",
                s->index, cursor_set->width, cursor_set->height);
        return -1;
    }

    pthread_mutex_lock(&src->cursor_lock);
    cursor_pump_locked(s);
    int target = s->index;
    if (atomic_load_explicit(&s->cursor_upload_inflight, memory_order_acquire)) {
        pthread_mutex_unlock(&src->cursor_lock);
        return 0;
    }
    if (!force_upload &&
        s->cursor_shape_slot <= SMID_CURSOR_CACHE_MAX &&
        cursor_cache_matches_source(&src->cursor_cache[s->cursor_shape_slot], cursor_set)) {
        int rc = cursor_select_locked(s);
        pthread_mutex_unlock(&src->cursor_lock);
        return rc;
    }

    uint8_t *p = s->cursor_image_pkt;
    p[19] = target ? 0x10 : 0x00;
    p[24] = 0;
    smid_put32(p + 64, (uint32_t)cursor_set->hot_x);
    smid_put32(p + 68, (uint32_t)cursor_set->hot_y);
    smid_put32(p + 72, cursor_set->width);
    smid_put32(p + 76, cursor_set->height);
    smid_put32(p + 80, cursor_set->pixel_format);
    smid_put32(p + 84, cursor_set->stride);
    memset(p + SMID_CURSOR_IMAGE_OFF, 0, SMID_CURSOR_IMAGE_BYTES);

    const uint8_t *src_pixels = (const uint8_t *)cursor_set->buffer;
    uint32_t src_stride = cursor_set->stride ? cursor_set->stride : cursor_set->width * 4u;
    uint32_t copy_row = cursor_set->width * 4u;
    if (copy_row > src_stride) {
        copy_row = src_stride;
    }
    if (copy_row > SMID_CURSOR_STRIDE) {
        copy_row = SMID_CURSOR_STRIDE;
    }
    for (uint32_t y = 0; y < cursor_set->height; y++) {
        memcpy(p + SMID_CURSOR_IMAGE_OFF + (size_t)y * SMID_CURSOR_STRIDE,
               src_pixels + (size_t)y * src_stride, copy_row);
    }

    uint8_t *image = p + SMID_CURSOR_IMAGE_OFF;
    uint32_t hash = cursor_image_hash(cursor_set, image);
    uint32_t shape = UINT32_MAX;
    bool cached = false;
    if (!force_upload) {
        for (uint32_t i = 0; i <= SMID_CURSOR_CACHE_MAX; i++) {
            if (cursor_cache_matches(&src->cursor_cache[i], cursor_set, image, hash)) {
                shape = i;
                cached = true;
                break;
            }
        }
    }
    if (!cached) {
        if (src->cursor_next_shape > SMID_CURSOR_CACHE_MAX) {
            src->cursor_next_shape -= SMID_CURSOR_CACHE_REWIND;
        }
        shape = src->cursor_next_shape++;
        if (shape > SMID_CURSOR_CACHE_MAX) {
            shape = SMID_CURSOR_CACHE_MAX;
        }
        struct smid_cursor_cache_entry *e = &src->cursor_cache[shape];
        e->valid = true;
        e->shape = shape;
        e->hash = hash;
        e->width = cursor_set->width;
        e->height = cursor_set->height;
        e->hot_x = (uint32_t)cursor_set->hot_x;
        e->hot_y = (uint32_t)cursor_set->hot_y;
        e->pixel_format = cursor_set->pixel_format;
        e->stride = cursor_set->stride;
        memcpy(e->image, image, SMID_CURSOR_IMAGE_BYTES);
    }

    s->cursor_shape_slot = shape;
    smid_put32(p + 25, shape);
    s->cursor_select_pkt[21] = (uint8_t)(target & 1);
    smid_put32(s->cursor_select_pkt + 22, shape);
    s->cursor_select_pkt[26] = 1;

    int rc = 0;
    if (!cached) {
        uint64_t rx_before = 0;
        uint64_t code_before = 0;
        pthread_mutex_lock(&src->usb_lock);
        struct smid_usb *usb = src->usb;
        if (!usb) {
            pthread_mutex_unlock(&src->usb_lock);
            pthread_mutex_unlock(&src->cursor_lock);
            return -1;
        }
        smid_usb_rx_snapshot(usb, 0x42, &rx_before, &code_before);
        (void)rx_before;
        smid_tx_result_reset(&s->cursor_upload_result);
        struct smid_tx_item item = {
            .priority = SMID_TX_PRIO_CURSOR,
            .endpoint = SMID_EP_BULK_OUT,
            .xfer = SMID_USB_XFER_BULK,
            .timeout_ms = 1000,
            .data = s->cursor_image_pkt,
            .len = SMID_CURSOR_IMAGE_LEN,
            .result = &s->cursor_upload_result,
            .name = "cursor-image",
        };
        rc = smid_transport_submit(&usb->tx, &item);
        pthread_mutex_unlock(&src->usb_lock);
        if (rc) {
            pthread_mutex_unlock(&src->cursor_lock);
            return rc;
        }
        s->cursor_upload_tx_done = false;
        s->cursor_upload_shape = shape;
        s->cursor_upload_code_before = code_before;
        s->cursor_upload_start_us = smid_mono_us();
        atomic_store_explicit(&s->cursor_upload_inflight, true, memory_order_release);
        pthread_mutex_unlock(&src->cursor_lock);
        return 0;
    }
    rc = cursor_select_locked(s);
    pthread_mutex_unlock(&src->cursor_lock);
    return rc;
}

static int cursor_send_image(struct smid_evdi_stream *s, struct evdi_cursor_set *cursor_set) {
    return cursor_send_image_ex(s, cursor_set, false);
}

static void evdi_update_handler(int buffer_id, void *user_data) {
    struct smid_evdi_stream *s = user_data;
    (void)buffer_id;
    if (!atomic_load_explicit(&s->dpms_on, memory_order_acquire)) {
        atomic_store_explicit(&s->requested, false, memory_order_release);
        return;
    }
    int n = SMID_EVDI_MAX_RECTS;
    uint8_t dirty = 0;
    pthread_rwlock_wrlock(&s->buffer_lock);
    evdi_grab_pixels(s->handle, s->rects, &n);
    dirty = screen_dirty_mask_locked(s);
    pthread_rwlock_unlock(&s->buffer_lock);

    if (dirty) {
        atomic_fetch_or_explicit(&s->dirty_tiles, dirty, memory_order_release);
        atomic_fetch_add_explicit(&s->stat_dirty, 1, memory_order_relaxed);
        signal_damage(s->source);
    }
    atomic_store_explicit(&s->requested, false, memory_order_release);
}

static void evdi_dpms_handler(int dpms_mode, void *user_data) {
    struct smid_evdi_stream *s = user_data;
    bool on = dpms_mode == 0;
    bool old = atomic_exchange_explicit(&s->dpms_on, on, memory_order_acq_rel);

    if (on) {
        atomic_store_explicit(&s->requested, false, memory_order_release);
        atomic_store_explicit(&s->blank_tiles, 0, memory_order_release);
        atomic_fetch_or_explicit(&s->dirty_tiles, 0x3, memory_order_release);
        signal_damage(s->source);
    } else {
        atomic_store_explicit(&s->requested, false, memory_order_release);
        atomic_store_explicit(&s->dirty_tiles, 0, memory_order_release);
        if (old) {
            atomic_store_explicit(&s->blank_tiles, 0x3, memory_order_release);
            signal_damage(s->source);
        }
    }

    if (old != on) {
        smid_logf("evdi%d: dpms %s\n", s->index, on ? "on" : "off");
        update_device_power(s->source);
    }
}

static void evdi_cursor_set_handler(struct evdi_cursor_set cursor_set, void *user_data) {
    struct smid_evdi_stream *s = user_data;
    struct smid_evdi_source *src = s->source;
    if (src->cursor_events && source_usb_locked(src)) {
        pthread_mutex_lock(&src->cursor_event_lock);
        s->pending_cursor_enabled = cursor_set.enabled && cursor_set.buffer &&
                cursor_set.width > 0 && cursor_set.height > 0 &&
                cursor_set.width <= SMID_CURSOR_SIDE &&
                cursor_set.height <= SMID_CURSOR_SIDE;
        s->pending_cursor_width = cursor_set.width;
        s->pending_cursor_height = cursor_set.height;
        s->pending_cursor_hot_x = cursor_set.hot_x;
        s->pending_cursor_hot_y = cursor_set.hot_y;
        s->pending_cursor_pixel_format = cursor_set.pixel_format;
        s->pending_cursor_stride = cursor_set.stride;
        memset(s->pending_cursor_image, 0, sizeof(s->pending_cursor_image));
        if (s->pending_cursor_enabled) {
            const uint8_t *src_pixels = (const uint8_t *)cursor_set.buffer;
            uint32_t src_stride = cursor_set.stride ? cursor_set.stride : cursor_set.width * 4u;
            uint32_t copy_row = cursor_set.width * 4u;
            if (copy_row > src_stride) {
                copy_row = src_stride;
            }
            if (copy_row > SMID_CURSOR_STRIDE) {
                copy_row = SMID_CURSOR_STRIDE;
            }
            for (uint32_t y = 0; y < cursor_set.height; y++) {
                memcpy(s->pending_cursor_image + (size_t)y * SMID_CURSOR_STRIDE,
                       src_pixels + (size_t)y * src_stride, copy_row);
            }
            s->pending_cursor_stride = SMID_CURSOR_STRIDE;
        }
        s->pending_cursor_set = true;
        pthread_cond_signal(&src->cursor_event_cond);
        pthread_mutex_unlock(&src->cursor_event_lock);
    }
    free(cursor_set.buffer);
}

static void evdi_cursor_move_handler(struct evdi_cursor_move cursor_move, void *user_data) {
    struct smid_evdi_stream *s = user_data;
    struct smid_evdi_source *src = s->source;
    if (src->cursor_events && source_usb_locked(src)) {
        pthread_mutex_lock(&src->cursor_event_lock);
        s->pending_cursor_x = cursor_move.x;
        s->pending_cursor_y = cursor_move.y;
        s->pending_cursor_move = true;
        pthread_cond_signal(&src->cursor_event_cond);
        pthread_mutex_unlock(&src->cursor_event_lock);
    }
}

static bool cursor_events_pending_locked(struct smid_evdi_source *src) {
    for (int i = 0; i < src->streams; i++) {
        if (src->stream[i].pending_cursor_set ||
            src->stream[i].pending_cursor_move) {
            return true;
        }
    }
    return false;
}

static bool cursor_uploads_inflight(struct smid_evdi_source *src) {
    for (int i = 0; i < src->streams; i++) {
        if (atomic_load_explicit(&src->stream[i].cursor_upload_inflight,
                                 memory_order_acquire)) {
            return true;
        }
    }
    return false;
}

static void *cursor_thread_main(void *arg) {
    struct smid_evdi_source *src = arg;
    for (;;) {
        int stream = -1;
        bool have_set = false;
        bool have_move = false;
        bool enabled = false;
        uint32_t width = 0;
        uint32_t height = 0;
        int32_t hot_x = 0;
        int32_t hot_y = 0;
        uint32_t pixel_format = 0;
        uint32_t stride = 0;
        int32_t move_x = 0;
        int32_t move_y = 0;
        uint8_t image[SMID_CURSOR_IMAGE_BYTES];

        for (int i = 0; i < src->streams; i++) {
            cursor_pump_stream(&src->stream[i]);
        }

        pthread_mutex_lock(&src->cursor_event_lock);
        while (!atomic_load_explicit(&src->cursor_stop, memory_order_acquire) &&
               !cursor_events_pending_locked(src)) {
            if (!cursor_uploads_inflight(src)) {
                pthread_cond_wait(&src->cursor_event_cond, &src->cursor_event_lock);
                continue;
            }
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 2000000L;
            if (ts.tv_nsec >= 1000000000L) {
                ts.tv_sec++;
                ts.tv_nsec -= 1000000000L;
            }
            (void)pthread_cond_timedwait(&src->cursor_event_cond, &src->cursor_event_lock, &ts);
            break;
        }
        if (atomic_load_explicit(&src->cursor_stop, memory_order_acquire) &&
            !cursor_events_pending_locked(src)) {
            pthread_mutex_unlock(&src->cursor_event_lock);
            break;
        }
        for (int i = 0; i < src->streams; i++) {
            struct smid_evdi_stream *s = &src->stream[i];
            if (!s->pending_cursor_set && !s->pending_cursor_move) {
                continue;
            }
            stream = i;
            have_set = s->pending_cursor_set;
            have_move = s->pending_cursor_move;
            enabled = s->pending_cursor_enabled;
            width = s->pending_cursor_width;
            height = s->pending_cursor_height;
            hot_x = s->pending_cursor_hot_x;
            hot_y = s->pending_cursor_hot_y;
            pixel_format = s->pending_cursor_pixel_format;
            stride = s->pending_cursor_stride;
            move_x = s->pending_cursor_x;
            move_y = s->pending_cursor_y;
            if (have_set && enabled) {
                memcpy(image, s->pending_cursor_image, sizeof(image));
            }
            s->pending_cursor_set = false;
            s->pending_cursor_move = false;
            break;
        }
        pthread_mutex_unlock(&src->cursor_event_lock);

        if (stream < 0) {
            continue;
        }
        struct smid_evdi_stream *s = &src->stream[stream];
        if (have_set) {
            struct evdi_cursor_set cursor_set = {
                .enabled = enabled,
                .width = width,
                .height = height,
                .hot_x = hot_x,
                .hot_y = hot_y,
                .pixel_format = pixel_format,
                .stride = stride,
                .buffer = enabled ? (uint32_t *)image : NULL,
            };
            (void)cursor_send_image(s, &cursor_set);
        }
        if (have_move) {
            if (!move_x && !move_y) {
                (void)cursor_send_state(s, 0, 0, true);
                (void)cursor_send_state(s, 0, 0, false);
            } else {
                (void)cursor_send_state(s, move_x, move_y, true);
            }
        }
    }
    return NULL;
}

static bool evdi_request_next(struct smid_evdi_stream *s) {
    bool can_request = false;
    if (atomic_load_explicit(&s->mode_seen, memory_order_acquire) &&
        atomic_load_explicit(&s->dpms_on, memory_order_acquire)) {
        bool expected = false;
        can_request = atomic_compare_exchange_strong_explicit(
                &s->requested, &expected, true, memory_order_acq_rel, memory_order_acquire);
    }
    if (!can_request) {
        return false;
    }

    if (evdi_request_update(s->handle, SMID_EVDI_BUFFER_ID)) {
        evdi_update_handler(SMID_EVDI_BUFFER_ID, s);
        return true;
    }
    return false;
}

static void *evdi_thread(void *arg) {
    struct smid_evdi_stream *s = arg;
    int fd = evdi_get_event_ready(s->handle);
    while (!atomic_load_explicit(&s->stop, memory_order_acquire)) {
        if (evdi_request_next(s)) {
            continue;
        }
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        int max_fd = fd;
        if (s->wake_pipe[0] >= 0) {
            FD_SET(s->wake_pipe[0], &rfds);
            if (s->wake_pipe[0] > max_fd) {
                max_fd = s->wake_pipe[0];
            }
        }
        int rc = select(max_fd + 1, &rfds, NULL, NULL, NULL);
        if (rc < 0 && errno == EINTR) {
            continue;
        }
        if (rc < 0) {
            break;
        }
        if (s->wake_pipe[0] >= 0 && FD_ISSET(s->wake_pipe[0], &rfds)) {
            char buf[64];
            while (read(s->wake_pipe[0], buf, sizeof(buf)) > 0) {
            }
            continue;
        }
        if (FD_ISSET(fd, &rfds)) {
            evdi_handle_events(s->handle, &s->events);
        }
    }
    return NULL;
}

static int stream_start(struct smid_evdi_source *src, struct smid_evdi_stream *s,
                        int index, bool cursor_events) {
    memset(s, 0, sizeof(*s));
    s->source = src;
    s->index = index;
    s->handle = EVDI_INVALID_HANDLE;
    s->wake_pipe[0] = -1;
    s->wake_pipe[1] = -1;
    if (pthread_rwlock_init(&s->buffer_lock, NULL)) {
        smid_logf("evdi%d: buffer lock init failed\n", index);
        return -1;
    }
    s->buffer_lock_initialized = true;
    atomic_init(&s->stop, false);
    atomic_init(&s->dpms_on, true);
    atomic_init(&s->mode_seen, false);
    atomic_init(&s->requested, false);
    atomic_init(&s->dirty_tiles, 0);
    atomic_init(&s->blank_tiles, 0);
    cursor_init_stream(s);

    s->handle = evdi_open_attached_to(NULL);
    if (s->handle == EVDI_INVALID_HANDLE) {
        smid_logf("evdi%d: failed to open or add device\n", index);
        return -1;
    }
    s->events.dpms_handler = evdi_dpms_handler;
    s->events.mode_changed_handler = evdi_mode_handler;
    s->events.update_ready_handler = evdi_update_handler;
    s->events.cursor_set_handler = evdi_cursor_set_handler;
    s->events.cursor_move_handler = evdi_cursor_move_handler;
    s->events.user_data = s;
    evdi_enable_cursor_events(s->handle, cursor_events);

    if (pipe(s->wake_pipe)) {
        smid_logf("evdi%d: wake pipe failed: %s\n", index, strerror(errno));
        return -1;
    }
    for (int i = 0; i < 2; i++) {
        int flags = fcntl(s->wake_pipe[i], F_GETFL);
        if (flags >= 0) {
            (void)fcntl(s->wake_pipe[i], F_SETFL, flags | O_NONBLOCK);
        }
    }

    uint8_t edid[128];
    make_1080p_edid(edid, index);
    evdi_connect2(s->handle, edid, sizeof(edid),
                  SMID_WIDTH * SMID_STREAM_H, SMID_WIDTH * SMID_STREAM_H * 60u);
    if (pthread_create(&s->thread, NULL, evdi_thread, s)) {
        smid_logf("evdi%d: thread create failed\n", index);
        stream_stop(s);
        return -1;
    }
    s->thread_started = true;
    return 0;
}

static void stream_stop(struct smid_evdi_stream *s) {
    atomic_store_explicit(&s->stop, true, memory_order_release);
    if (s->thread_started) {
        if (s->wake_pipe[1] >= 0) {
            char b = 0;
            ssize_t ignored = write(s->wake_pipe[1], &b, 1);
            (void)ignored;
        }
        pthread_join(s->thread, NULL);
        s->thread_started = false;
    }
    if (s->handle != EVDI_INVALID_HANDLE) {
        if (s->buffer_lock_initialized) {
            pthread_rwlock_wrlock(&s->buffer_lock);
            if (s->buffer) {
                evdi_unregister_buffer(s->handle, SMID_EVDI_BUFFER_ID);
            }
            pthread_rwlock_unlock(&s->buffer_lock);
        }
        evdi_disconnect(s->handle);
        evdi_close(s->handle);
        s->handle = EVDI_INVALID_HANDLE;
    }
    if (s->wake_pipe[0] >= 0) {
        close(s->wake_pipe[0]);
    }
    if (s->wake_pipe[1] >= 0) {
        close(s->wake_pipe[1]);
    }
    if (s->cursor_upload_result_ready) {
        if (atomic_load_explicit(&s->cursor_upload_inflight, memory_order_acquire) &&
            !s->cursor_upload_tx_done) {
            (void)smid_tx_result_wait(&s->cursor_upload_result);
        }
        smid_tx_result_destroy(&s->cursor_upload_result);
        s->cursor_upload_result_ready = false;
    }
    if (s->buffer_lock_initialized) {
        pthread_rwlock_wrlock(&s->buffer_lock);
        free(s->buffer);
        s->buffer = NULL;
        pthread_rwlock_unlock(&s->buffer_lock);
        pthread_rwlock_destroy(&s->buffer_lock);
        s->buffer_lock_initialized = false;
    }
}

int smid_evdi_source_start(struct smid_evdi_source **out, int streams, bool cursor_events,
                           struct smid_usb *usb) {
    if (streams < 1 || streams > SMID_EVDI_MAX_STREAMS) {
        return -1;
    }
    struct smid_evdi_source *src = calloc(1, sizeof(*src));
    if (!src) {
        return -1;
    }
    src->streams = streams;
    src->cursor_events = cursor_events;
    struct smid_frame_hash_impl hash_impl = frame_hash_resolve();
    src->frame_hash_fn = hash_impl.fn;
    src->usb = usb;
    src->cursor_selected_stream = -1;
    for (int i = 0; i < SMID_EVDI_MAX_STREAMS; i++) {
        src->cursor_selected_shape[i] = UINT32_MAX;
    }
    if (pthread_mutex_init(&src->damage_lock, NULL)) {
        free(src);
        return -1;
    }
    if (pthread_mutex_init(&src->usb_lock, NULL)) {
        pthread_mutex_destroy(&src->damage_lock);
        free(src);
        return -1;
    }
    if (pthread_cond_init(&src->damage_cond, NULL)) {
        pthread_mutex_destroy(&src->usb_lock);
        pthread_mutex_destroy(&src->damage_lock);
        free(src);
        return -1;
    }
    if (pthread_mutex_init(&src->cursor_lock, NULL)) {
        pthread_cond_destroy(&src->damage_cond);
        pthread_mutex_destroy(&src->usb_lock);
        pthread_mutex_destroy(&src->damage_lock);
        free(src);
        return -1;
    }
    if (pthread_mutex_init(&src->cursor_event_lock, NULL)) {
        pthread_mutex_destroy(&src->cursor_lock);
        pthread_cond_destroy(&src->damage_cond);
        pthread_mutex_destroy(&src->usb_lock);
        pthread_mutex_destroy(&src->damage_lock);
        free(src);
        return -1;
    }
    if (pthread_cond_init(&src->cursor_event_cond, NULL)) {
        pthread_mutex_destroy(&src->cursor_event_lock);
        pthread_mutex_destroy(&src->cursor_lock);
        pthread_cond_destroy(&src->damage_cond);
        pthread_mutex_destroy(&src->usb_lock);
        pthread_mutex_destroy(&src->damage_lock);
        free(src);
        return -1;
    }
    atomic_init(&src->cursor_stop, false);
    atomic_init(&src->device_power_on, true);
    for (int i = 0; i < streams; i++) {
        if (stream_start(src, &src->stream[i], i, cursor_events)) {
            smid_evdi_source_stop(src);
            return -1;
        }
    }
    if (cursor_events && usb) {
        if (pthread_create(&src->cursor_thread, NULL, cursor_thread_main, src)) {
            smid_logf("evdi: cursor thread create failed\n");
            smid_evdi_source_stop(src);
            return -1;
        }
        src->cursor_thread_started = true;
    }

    uint64_t end_us = smid_mono_us() + 7000000u;
    while (smid_mono_us() < end_us) {
        bool ready = true;
        for (int i = 0; i < streams; i++) {
            ready = ready && atomic_load_explicit(&src->stream[i].mode_seen, memory_order_acquire);
        }
        if (ready) {
            *out = src;
            return 0;
        }
        smid_sleep_until_us(smid_mono_us() + 50000u);
    }
    smid_logf("evdi: timed out waiting for mode events\n");
    smid_evdi_source_stop(src);
    return -1;
}

void smid_evdi_source_stop(struct smid_evdi_source *src) {
    if (!src) {
        return;
    }
    atomic_store_explicit(&src->cursor_stop, true, memory_order_release);
    pthread_mutex_lock(&src->cursor_event_lock);
    pthread_cond_broadcast(&src->cursor_event_cond);
    pthread_mutex_unlock(&src->cursor_event_lock);
    if (src->cursor_thread_started) {
        pthread_join(src->cursor_thread, NULL);
        src->cursor_thread_started = false;
    }
    for (int i = 0; i < src->streams; i++) {
        stream_stop(&src->stream[i]);
    }
    pthread_cond_destroy(&src->cursor_event_cond);
    pthread_mutex_destroy(&src->cursor_event_lock);
    pthread_mutex_destroy(&src->cursor_lock);
    pthread_cond_destroy(&src->damage_cond);
    pthread_mutex_destroy(&src->usb_lock);
    pthread_mutex_destroy(&src->damage_lock);
    free(src);
}

int smid_evdi_source_poke_cursor_state(struct smid_evdi_source *src) {
    if (!src || !source_usb_locked(src) || !src->cursor_events) {
        return 0;
    }

    struct smid_cursor_cache_entry replay[SMID_EVDI_MAX_STREAMS];
    bool replay_valid[SMID_EVDI_MAX_STREAMS] = {0};
    bool replay_visible[SMID_EVDI_MAX_STREAMS] = {0};
    int32_t replay_x[SMID_EVDI_MAX_STREAMS] = {0};
    int32_t replay_y[SMID_EVDI_MAX_STREAMS] = {0};

    pthread_mutex_lock(&src->cursor_lock);
    /* USB recovery clears device-side cursor slots; replay from our local image cache. */
    for (int i = 0; i < src->streams; i++) {
        struct smid_evdi_stream *s = &src->stream[i];
        uint32_t shape = s->cursor_shape_slot;
        if (shape <= SMID_CURSOR_CACHE_MAX && src->cursor_cache[shape].valid) {
            replay[i] = src->cursor_cache[shape];
            replay_valid[i] = true;
            replay_visible[i] = s->cursor_visible;
            replay_x[i] = s->cursor_x;
            replay_y[i] = s->cursor_y;
        }
        atomic_store_explicit(&s->cursor_upload_inflight, false, memory_order_release);
        s->cursor_upload_tx_done = false;
        s->cursor_upload_shape = UINT32_MAX;
        s->cursor_shape_slot = UINT32_MAX;
    }
    src->cursor_selected_stream = -1;
    for (int i = 0; i < SMID_EVDI_MAX_STREAMS; i++) {
        src->cursor_selected_shape[i] = UINT32_MAX;
    }
    for (uint32_t i = 0; i <= SMID_CURSOR_CACHE_MAX; i++) {
        src->cursor_cache[i].valid = false;
    }
    pthread_mutex_unlock(&src->cursor_lock);

    int rc = 0;
    for (int i = 0; i < src->streams && !rc; i++) {
        if (!replay_valid[i]) {
            continue;
        }
        struct smid_cursor_cache_entry *e = &replay[i];
        struct evdi_cursor_set cursor_set = {
            .enabled = true,
            .width = e->width,
            .height = e->height,
            .hot_x = (int32_t)e->hot_x,
            .hot_y = (int32_t)e->hot_y,
            .pixel_format = e->pixel_format,
            .stride = e->stride,
            .buffer = (uint32_t *)e->image,
        };
        rc = cursor_send_image_ex(&src->stream[i], &cursor_set, true);
    }

    pthread_mutex_lock(&src->cursor_event_lock);
    pthread_cond_broadcast(&src->cursor_event_cond);
    pthread_mutex_unlock(&src->cursor_event_lock);

    for (int i = 0; i < src->streams && !rc; i++) {
        if (!replay_valid[i] || !replay_visible[i]) {
            continue;
        }
        struct smid_evdi_stream *s = &src->stream[i];
        if (atomic_load_explicit(&s->cursor_upload_inflight, memory_order_acquire)) {
            continue;
        }
        rc = cursor_send_state_force(s, replay_x[i], replay_y[i], true);
    }

    return rc;
}

void smid_evdi_source_take_stats(struct smid_evdi_source *src, struct smid_evdi_stats *stats) {
    memset(stats, 0, sizeof(*stats));
    if (!src) {
        return;
    }
    for (int i = 0; i < src->streams; i++) {
        struct smid_evdi_stream *s = &src->stream[i];
        uint64_t dirty = atomic_take_u64(&s->stat_dirty);
        stats->dirty += dirty;
        if (i < 2) {
            stats->stream_dirty[i] = dirty;
        }
    }
}

uint8_t smid_evdi_source_consume_dirty(struct smid_evdi_source *src) {
    uint8_t dirty = 0;
    if (!src) {
        return 0;
    }
    for (int i = 0; i < src->streams; i++) {
        uint8_t stream_dirty = atomic_exchange_explicit(&src->stream[i].dirty_tiles, 0,
                                                       memory_order_acquire) & 0x3u;
        if (!atomic_load_explicit(&src->stream[i].dpms_on, memory_order_acquire)) {
            stream_dirty = 0;
        }
        dirty |= (uint8_t)(stream_dirty << (i * 2));
    }
    return dirty;
}

uint8_t smid_evdi_source_consume_blank(struct smid_evdi_source *src) {
    uint8_t blank = 0;
    if (!src) {
        return 0;
    }
    for (int i = 0; i < src->streams; i++) {
        uint8_t stream_blank = atomic_exchange_explicit(&src->stream[i].blank_tiles, 0,
                                                        memory_order_acquire) & 0x3u;
        blank |= (uint8_t)(stream_blank << (i * 2));
    }
    return blank;
}

int smid_evdi_source_current_band_hashes(struct smid_evdi_source *src, int stream,
                                         struct smid_frame_hash hashes[2]) {
    if (!src || !hashes || stream < 0 || stream >= src->streams) {
        return -1;
    }
    struct smid_evdi_stream *s = &src->stream[stream];
    int rc = 0;
    pthread_rwlock_rdlock(&s->buffer_lock);
    if (!s->buffer || s->width <= 0 || s->height <= 0) {
        rc = -1;
    }
    for (int tile = 0; !rc && tile < 2; tile++) {
        if (s->have_band_hash[tile]) {
            hashes[tile] = s->last_band_hash[tile];
            continue;
        }
        size_t band_bytes = 0;
        const uint8_t *band = band_data_locked(s, tile, &band_bytes);
        if (!band || !band_bytes) {
            rc = -1;
            break;
        }
        hashes[tile] = src->frame_hash_fn(band, band_bytes);
    }
    pthread_rwlock_unlock(&s->buffer_lock);
    return rc;
}

uint64_t smid_evdi_source_damage_snapshot(struct smid_evdi_source *src) {
    if (!src) {
        return 0;
    }
    pthread_mutex_lock(&src->damage_lock);
    uint64_t generation = src->damage_generation;
    pthread_mutex_unlock(&src->damage_lock);
    return generation;
}

void smid_evdi_source_wait_damage(struct smid_evdi_source *src, uint64_t generation,
                                  uint64_t timeout_us) {
    if (!src) {
        return;
    }
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += (time_t)(timeout_us / 1000000u);
    ts.tv_nsec += (long)((timeout_us % 1000000u) * 1000u);
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }

    pthread_mutex_lock(&src->damage_lock);
    while (src->damage_generation == generation) {
        if (pthread_cond_timedwait(&src->damage_cond, &src->damage_lock, &ts) == ETIMEDOUT) {
            break;
        }
    }
    pthread_mutex_unlock(&src->damage_lock);
}

void smid_evdi_source_copy_rgb_tile(struct smid_evdi_source *src, int stream, int tile,
                                    uint8_t *rgb) {
    if (!src || stream < 0 || stream >= src->streams || tile < 0 || tile > 1) {
        memset(rgb, 0, (size_t)SMID_WIDTH * SMID_TILE_H * 3u);
        return;
    }
    struct smid_evdi_stream *s = &src->stream[stream];
    pthread_rwlock_rdlock(&s->buffer_lock);
    if (!s->buffer || s->width <= 0 || s->height <= 0) {
        memset(rgb, 0, (size_t)SMID_WIDTH * SMID_TILE_H * 3u);
        pthread_rwlock_unlock(&s->buffer_lock);
        return;
    }
    int src_w = s->width;
    int src_h = s->height;
    int stride = s->stride;
    int sy0 = tile ? 0 : SMID_LOWER_TILE_Y0;
    for (int y = 0; y < SMID_TILE_H; y++) {
        int sy = y + sy0;
        uint8_t *dst = rgb + (size_t)y * SMID_WIDTH * 3u;
        if (sy >= src_h) {
            memset(dst, 0, (size_t)SMID_WIDTH * 3u);
            continue;
        }
        const uint8_t *src_row = s->buffer + (size_t)sy * (size_t)stride;
        for (int x = 0; x < SMID_WIDTH; x++) {
            int sx = x < src_w ? x : src_w - 1;
            const uint8_t *p = src_row + (size_t)sx * 4u;
            dst[x * 3u + 0u] = p[2];
            dst[x * 3u + 1u] = p[1];
            dst[x * 3u + 2u] = p[0];
        }
    }
    pthread_rwlock_unlock(&s->buffer_lock);
}

void smid_evdi_source_copy_bgrx_tile_pitched(struct smid_evdi_source *src,
                                             int stream, int tile,
                                             uint8_t *bgrx, int dst_pitch) {
    if (!bgrx || dst_pitch < SMID_WIDTH * 4) {
        return;
    }
    if (!src || stream < 0 || stream >= src->streams || tile < 0 || tile > 1) {
        for (int y = 0; y < SMID_TILE_H; y++) {
            memset(bgrx + (size_t)y * (size_t)dst_pitch, 0, (size_t)SMID_WIDTH * 4u);
        }
        return;
    }
    struct smid_evdi_stream *s = &src->stream[stream];
    pthread_rwlock_rdlock(&s->buffer_lock);
    if (!s->buffer || s->width <= 0 || s->height <= 0) {
        for (int y = 0; y < SMID_TILE_H; y++) {
            memset(bgrx + (size_t)y * (size_t)dst_pitch, 0, (size_t)SMID_WIDTH * 4u);
        }
        pthread_rwlock_unlock(&s->buffer_lock);
        return;
    }
    int src_w = s->width;
    int src_h = s->height;
    int stride = s->stride;
    int sy0 = tile ? 0 : SMID_LOWER_TILE_Y0;
    if (src_w >= SMID_WIDTH && sy0 + SMID_TILE_H <= src_h &&
        stride == SMID_WIDTH * 4 && dst_pitch == SMID_WIDTH * 4) {
        memcpy(bgrx, s->buffer + (size_t)sy0 * (size_t)stride,
               (size_t)SMID_TILE_H * (size_t)dst_pitch);
        pthread_rwlock_unlock(&s->buffer_lock);
        return;
    }
    for (int y = 0; y < SMID_TILE_H; y++) {
        int sy = y + sy0;
        uint8_t *dst = bgrx + (size_t)y * (size_t)dst_pitch;
        if (sy >= src_h) {
            memset(dst, 0, (size_t)SMID_WIDTH * 4u);
            continue;
        }
        const uint8_t *src_row = s->buffer + (size_t)sy * (size_t)stride;
        if (src_w >= SMID_WIDTH) {
            memcpy(dst, src_row, (size_t)SMID_WIDTH * 4u);
            continue;
        }
        memcpy(dst, src_row, (size_t)src_w * 4u);
        const uint8_t *edge = src_row + (size_t)(src_w - 1) * 4u;
        for (int x = src_w; x < SMID_WIDTH; x++) {
            memcpy(dst + (size_t)x * 4u, edge, 4u);
        }
    }
    pthread_rwlock_unlock(&s->buffer_lock);
}

int smid_evdi_source_with_bgrx_frame(struct smid_evdi_source *src, int stream,
                                     smid_evdi_bgrx_frame_fn fn, void *ctx) {
    if (!src || stream < 0 || stream >= src->streams || !fn) {
        return -1;
    }
    struct smid_evdi_stream *s = &src->stream[stream];
    pthread_rwlock_rdlock(&s->buffer_lock);
    if (!s->buffer || s->width <= 0 || s->height <= 0 || s->stride <= 0) {
        pthread_rwlock_unlock(&s->buffer_lock);
        return -1;
    }
    int rc = fn(ctx, s->buffer, s->width, s->height, s->stride);
    pthread_rwlock_unlock(&s->buffer_lock);
    return rc;
}
