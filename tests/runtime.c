/* Exercise the production send planner without starting EVDI/USB. */
#define main smid_program_main
#include "main.c"
#undef main
#include <assert.h>

static unsigned hash_calls;
int smid_evdi_source_current_band_hashes(struct smid_evdi_source *src, int stream,
                                        struct smid_frame_hash hashes[2]) {
    (void)src;
    (void)stream;
    hash_calls++;
    hashes[0] = (struct smid_frame_hash){1, 2};
    hashes[1] = (struct smid_frame_hash){3, 4};
    return 0;
}

int main(void) {
    struct smid_video_retained_band retained[4] = {0};
    struct smid_video_send_plan plan;
    /* Populate a valid retained basis, then verify disabling it is effective. */
    for (unsigned i = 0; i < SMID_VIDEO_RETAINED_PERIOD; i++) {
        video_retained_band_note_packet(&retained[0], (struct smid_frame_hash){1, 2});
        video_retained_band_note_packet(&retained[1], (struct smid_frame_hash){3, 4});
    }
    assert(video_make_send_plan(NULL, 1, 15, retained, true, &plan) == 0);
    assert(plan.send_mask == 3 && plan.companion_mask == 2);
    hash_calls = 0;
    for (unsigned dirty = 0; dirty < 16; dirty++) {
        assert(video_make_send_plan(NULL, dirty, 15, retained, false, &plan) == 0);
        unsigned expected = (dirty & 3 ? 3 : 0) | (dirty & 12 ? 12 : 0);
        assert(plan.send_mask == expected && plan.companion_mask == 0);
        assert(plan.hash_valid_mask == 0);
    }
    assert(hash_calls == 0);
    assert(video_make_send_plan(NULL, 4, 3, retained, false, &plan) == 0);
    assert(plan.send_mask == 0); /* Ignore damage on a disabled display. */

    uint8_t packet[24] = {0};
    memcpy(packet, SMID_MAGIC, 12);
    const uint32_t sequences[] = {0, 1, 65536, 0xffffffffu};
    for (unsigned target = 0; target < 2; target++) {
        for (unsigned i = 0; i < SMID_ARRAY_LEN(sequences); i++) {
            smid_put32(packet + 16, target ? 0x010101e9u : 0x010001e9u);
            smid_put32(packet + 20, sequences[i]);
            int stream = -1;
            uint32_t seq = 0;
            assert(smid_parse_frame_ack(packet, sizeof(packet), &stream, &seq));
            assert(stream == (int)target && seq == sequences[i]);
            assert(!smid_parse_frame_ack(packet, 23, &stream, &seq));
            packet[17] = 0; /* A different status is not an accepted-frame ACK. */
            assert(!smid_parse_frame_ack(packet, sizeof(packet), &stream, &seq));
        }
    }
    puts("PASS: retained-frame opt-out, static-frame skipping, both ACK targets and 32-bit sequences");
    return 0;
}
