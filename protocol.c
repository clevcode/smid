#include "protocol.h"

#include <string.h>

void smid_put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

uint32_t smid_get32(const uint8_t *p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void init_packet(struct smid_rawcmd *c, const char *name, uint8_t ep, uint8_t xfer,
                        uint32_t wire_len, uint32_t payload_len, uint8_t selector,
                        uint8_t cmd, uint16_t arg, uint8_t wait_code) {
    memset(c, 0, sizeof(*c));
    c->name = name;
    c->endpoint = ep;
    c->xfer = xfer;
    c->len = wire_len;
    c->wait_code = wait_code;
    memcpy(c->data, SMID_MAGIC, 12);
    smid_put32(c->data + 12, payload_len);
    smid_put32(c->data + 16, 6u | ((uint32_t)selector << 24));
    c->data[20] = cmd;
    c->data[21] = (uint8_t)arg;
    c->data[22] = (uint8_t)(arg >> 8);
}

static void init_control(struct smid_rawcmd *c, const char *name, uint8_t selector,
                         uint8_t cmd, uint16_t arg, uint8_t wait_code) {
    init_packet(c, name, SMID_EP_BULK_OUT, 3, 48, 48, selector, cmd, arg, wait_code);
}

static void init_short(struct smid_rawcmd *c, const char *name, uint32_t payload_len,
                       uint8_t selector, uint8_t cmd, uint16_t arg, uint8_t wait_code) {
    init_packet(c, name, SMID_EP_BULK_OUT, 3, 44, payload_len, selector, cmd, arg, wait_code);
}

static void init_intr(struct smid_rawcmd *c, const char *name, uint32_t payload_len,
                      uint32_t word0, uint32_t word1) {
    memset(c, 0, sizeof(*c));
    c->name = name;
    c->endpoint = SMID_EP_INTR_OUT;
    c->xfer = 1;
    c->len = payload_len == 44 ? 44 : 48;
    memcpy(c->data, SMID_MAGIC, 12);
    smid_put32(c->data + 12, payload_len);
    smid_put32(c->data + 16, word0);
    smid_put32(c->data + 20, word1);
}

static void set_mode_slot(struct smid_rawcmd *c, int slot, uint8_t source, uint8_t target,
                          uint8_t layout, uint16_t width, uint16_t height,
                          uint8_t mode, uint16_t refresh) {
    uint8_t *p = c->data + 16 + 5 + slot * 10;
    p[0] = source;
    p[1] = target;
    p[2] = layout;
    p[3] = (uint8_t)width;
    p[4] = (uint8_t)(width >> 8);
    p[5] = (uint8_t)height;
    p[6] = (uint8_t)(height >> 8);
    p[7] = mode;
    p[8] = (uint8_t)refresh;
    p[9] = (uint8_t)(refresh >> 8);
}

static void init_set_mode(struct smid_rawcmd *c, const char *name, uint8_t selector,
                          uint8_t source, uint8_t target, uint8_t layout,
                          uint8_t wait_code) {
    init_control(c, name, selector, 0x30, 0x0100, wait_code);
    set_mode_slot(c, 0, source, target, layout, SMID_WIDTH, SMID_HEIGHT, 0x20, 60);
    set_mode_slot(c, 1, 0xff, 0, 2, 0, 0, 0x20, 60);
}

static void init_layer(struct smid_rawcmd *c, const char *name, uint8_t selector,
                       uint8_t wait_code) {
    init_control(c, name, selector, 0x40, 0x0001, wait_code);
    c->data[22] = (uint8_t)SMID_WIDTH;
    c->data[23] = (uint8_t)(SMID_WIDTH >> 8);
    c->data[24] = (uint8_t)SMID_HEIGHT;
    c->data[25] = (uint8_t)(SMID_HEIGHT >> 8);
}

static void init_video_bind(struct smid_rawcmd *c, const char *name, uint8_t selector,
                            uint8_t cmd, uint8_t wait_code) {
    init_control(c, name, selector, cmd, 0x0001, wait_code);
    c->data[22] = (uint8_t)SMID_WIDTH;
    c->data[23] = (uint8_t)(SMID_WIDTH >> 8);
    c->data[24] = (uint8_t)SMID_HEIGHT;
    c->data[25] = (uint8_t)(SMID_HEIGHT >> 8);
}

static void init_set_mode_both(struct smid_rawcmd *c, const char *name, uint8_t wait_code) {
    init_control(c, name, 0x10, 0x30, 0x0100, wait_code);
    set_mode_slot(c, 0, 0x00, 0x01, 0, SMID_WIDTH, SMID_HEIGHT, 0x20, 60);
    set_mode_slot(c, 1, 0x01, 0x04, 0, SMID_WIDTH, SMID_HEIGHT, 0x20, 60);
}

void smid_make_semantic_init(struct smid_rawcmd c[SMID_RAWCMD_CAP], size_t *n) {
    size_t i = 0;
    init_control(&c[i++], "work-mode-0", 0x00, 0x32, 0x0001, 0x32);
    init_control(&c[i++], "work-mode-1", 0x10, 0x32, 0x0001, 0x32);
    init_control(&c[i++], "chip-init", 0x00, 0x77, 0x7777, 0x77);
    init_control(&c[i++], "version-driver", 0x00, 0x61, 0x0001, 0x61);
    init_control(&c[i++], "version-client", 0x00, 0x61, 0x0005, 0x61);
    init_control(&c[i++], "version-layout", 0x00, 0x61, 0x0004, 0x61);
    init_short(&c[i++], "set-client-version", 8, 0x00, 0x45, 0x3030, 0);
    memcpy(c[i - 1].data + 21, "00.01.0004", 10);
    init_short(&c[i++], "topology", 44, 0x00, 0xd7, 0x0103, 0xd7);
    c[i - 1].data[23] = 0x01;
    init_control(&c[i++], "subscribe-pnp", 0x00, 0x65, 0, 0x65);
    init_short(&c[i++], "heartbeat", 44, 0x00, 0xec, 0, 0);
    init_control(&c[i++], "mode-list-source-1", 0x00, 0x60, 0x0001, 0x60);
    smid_put32(c[i - 1].data + 24, 0x1000);
    init_control(&c[i++], "mode-list-source-4", 0x00, 0x60, 0x0004, 0x60);
    smid_put32(c[i - 1].data + 24, 0x1000);
    init_control(&c[i++], "mode-list-source-1-current", 0x00, 0x60, 0x0001, 0x60);
    init_control(&c[i++], "edid-source-1", 0x00, 0x64, 0x0001, 0x64);
    smid_put32(c[i - 1].data + 32, 1);
    init_short(&c[i++], "vport-source-0", 8, 0x00, 0x45, 0, 0);

    init_control(&c[i++], "monitor-control-create-1", 0x00, 0x31, 0x0001, 0x31);
    init_short(&c[i++], "topology-after-create", 44, 0x00, 0xd7, 0x0103, 0xd7);
    c[i - 1].data[23] = 0x01;
    init_control(&c[i++], "monitor-control-create-2", 0x00, 0x31, 0x0101, 0x31);
    init_set_mode(&c[i++], "set-mode-source-0-target-1", 0x00, 0, 1, 0, 0x30);
    init_control(&c[i++], "bind-monitor-0", 0x00, 0x3f, 0, 0x3f);
    init_control(&c[i++], "bind-source-0", 0x00, 0x41, 0, 0x41);
    init_layer(&c[i++], "layer-0", 0x00, 0x40);
    init_control(&c[i++], "monitor-control-extra-1", 0x00, 0x31, 0x0001, 0x31);
    init_control(&c[i++], "monitor-control-extra-2", 0x00, 0x31, 0x0001, 0x31);
    init_short(&c[i++], "topology-final", 44, 0x00, 0xd7, 0x0103, 0xd7);
    c[i - 1].data[23] = 0x01;

    init_control(&c[i++], "mode-list-source-4-current", 0x00, 0x60, 0x0004, 0x60);
    init_control(&c[i++], "edid-source-4", 0x00, 0x64, 0x0004, 0x64);
    smid_put32(c[i - 1].data + 32, 1);
    init_short(&c[i++], "vport-source-4", 8, 0x00, 0x45, 0x0004, 0);
    init_intr(&c[i++], "interrupt-enable", 48, 0x00400039, 0);
    init_intr(&c[i++], "interrupt-zero", 44, 0x00000c3a, 0);

    init_control(&c[i++], "monitor-control-create-1-again", 0x00, 0x31, 0x0101, 0x31);
    init_control(&c[i++], "monitor-control-create-4", 0x10, 0x31, 0x0104, 0x31);
    init_set_mode_both(&c[i++], "set-mode-both-targets", 0x30);
    init_control(&c[i++], "bind-monitor-a-after-both", 0x00, 0x3f, 0, 0x3f);
    init_control(&c[i++], "bind-source-a-after-both", 0x00, 0x41, 0, 0x41);
    init_layer(&c[i++], "layer-a-after-both", 0x00, 0x40);
    init_video_bind(&c[i++], "bind-monitor-b", 0x10, 0x3f, 0x3f);
    init_video_bind(&c[i++], "bind-source-b", 0x10, 0x41, 0x41);
    init_layer(&c[i++], "layer-b", 0x10, 0x40);
    init_control(&c[i++], "monitor-control-extra-a-both", 0x00, 0x31, 0x0001, 0x31);
    c[i - 1].data[24] = 1;
    init_control(&c[i++], "monitor-control-extra-b-both", 0x10, 0x31, 0x0004, 0x31);
    c[i - 1].data[24] = 1;
    init_short(&c[i++], "topology-both-final", 44, 0x00, 0xd7, 0x0103, 0xd7);
    c[i - 1].data[23] = 0x01;
    *n = i;
}
