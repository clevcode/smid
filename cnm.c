#include "cnm.h"

#include "log.h"
#include "protocol.h"

#include <stdbool.h>
#include <string.h>

struct jpeg_cnm_info {
    int width;
    int height;
    int components;
    int restart_interval;
    int scan_data_offset;
    int qtables;
    int huff_tables;
    uint8_t samp_h[4];
    uint8_t samp_v[4];
    uint8_t comp_id[4];
    uint8_t qtable[4];
    uint8_t dc_table[4];
    uint8_t ac_table[4];
    uint8_t qdata[4][64];
    uint8_t huff_bits[4][16];
    uint8_t huff_vals[4][256];
    int huff_val_count[4];
};

static int align_up_int(int v, int a) {
    return (v + a - 1) & ~(a - 1);
}

static uint16_t be16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static void put64_le(uint8_t *p, uint64_t v) {
    smid_put32(p, (uint32_t)v);
    smid_put32(p + 4, (uint32_t)(v >> 32));
}

static int parse_jpeg_for_cnm(const uint8_t *jpeg, size_t len, struct jpeg_cnm_info *info) {
    memset(info, 0, sizeof(*info));
    if (len < 4 || jpeg[0] != 0xff || jpeg[1] != 0xd8) {
        return -1;
    }

    size_t pos = 2;
    while (pos + 4 <= len) {
        while (pos < len && jpeg[pos] == 0xff) {
            pos++;
        }
        if (pos >= len) {
            return -1;
        }
        uint8_t marker = jpeg[pos++];
        if (marker == 0xd9) {
            return 0;
        }
        if (marker == 0x01 || (marker >= 0xd0 && marker <= 0xd7)) {
            continue;
        }
        if (pos + 2 > len) {
            return -1;
        }
        uint16_t seg_len = be16(jpeg + pos);
        if (seg_len < 2 || pos + seg_len > len) {
            return -1;
        }
        const uint8_t *seg = jpeg + pos + 2;
        size_t payload = seg_len - 2u;
        if (marker == 0xda) {
            int n = payload ? seg[0] : 0;
            info->components = n;
            if (payload < (size_t)(1 + n * 2 + 3)) {
                return -1;
            }
            for (int i = 0; i < n && i < 4; i++) {
                uint8_t selector = seg[2 + i * 2];
                for (int c = 0; c < info->components && c < 4; c++) {
                    if (info->comp_id[c] == seg[1 + i * 2]) {
                        info->dc_table[c] = (uint8_t)(selector >> 4);
                        info->ac_table[c] = (uint8_t)(selector & 0x0f);
                    }
                }
            }
            info->scan_data_offset = (int)(pos + seg_len);
            return 0;
        }

        if (marker == 0xc0 && payload >= 6) {
            info->height = be16(seg + 1);
            info->width = be16(seg + 3);
            int n = seg[5];
            info->components = n;
            if (payload < (size_t)(6 + n * 3)) {
                return -1;
            }
            for (int i = 0; i < n && i < 4; i++) {
                const uint8_t *c = seg + 6 + i * 3;
                uint8_t sampling = c[1];
                info->comp_id[i] = c[0];
                info->samp_h[i] = (uint8_t)(sampling >> 4);
                info->samp_v[i] = (uint8_t)(sampling & 0x0f);
                info->qtable[i] = c[2];
            }
        } else if (marker == 0xdb) {
            size_t off = 0;
            while (off < payload) {
                uint8_t pq_tq = seg[off++];
                int precision = pq_tq >> 4;
                int table = pq_tq & 0x0f;
                size_t qlen = precision ? 128u : 64u;
                if (off + qlen > payload) {
                    return -1;
                }
                if (table < 4 && !precision) {
                    info->qtables |= 1 << table;
                    memcpy(info->qdata[table], seg + off, 64);
                }
                off += qlen;
            }
        } else if (marker == 0xc4) {
            size_t off = 0;
            while (off + 17 <= payload) {
                uint8_t tc_th = seg[off++];
                int klass = tc_th >> 4;
                int table = tc_th & 0x0f;
                size_t bits_off = off;
                int count = 0;
                for (int i = 0; i < 16; i++) {
                    count += seg[off + i];
                }
                off += 16;
                if (off + (size_t)count > payload) {
                    return -1;
                }
                if (table < 4) {
                    int vendor_table = (klass & 1) | ((table << 1) & 2);
                    info->huff_tables |= 1 << vendor_table;
                    memcpy(info->huff_bits[vendor_table], seg + bits_off, 16);
                    memcpy(info->huff_vals[vendor_table], seg + off, (size_t)count);
                    info->huff_val_count[vendor_table] = count;
                }
                off += (size_t)count;
            }
        } else if (marker == 0xdd && payload >= 2) {
            info->restart_interval = be16(seg);
        }
        pos += seg_len;
    }
    return -1;
}

static void build_derived_huffman(uint8_t *block) {
    for (int t = 0; t < 4; t++) {
        const uint8_t *bits = block + 0x338u + (size_t)t * 0x100u;
        uint8_t *valptr = block + 0xa50u + (size_t)t * 0x10u;
        uint8_t *min_tab = block + 0x850u + (size_t)t * 0x40u;
        uint8_t *max_tab = block + 0x950u + (size_t)t * 0x40u;
        bool started = false;
        int code = 0;
        uint8_t symbol_index = 0;
        for (int i = 0; i < 16; i++) {
            if (bits[i] == 0) {
                valptr[i] = 0xff;
                if (started) {
                    code *= 2;
                }
                smid_put32(min_tab + (size_t)i * 4u, 0xffffu);
                smid_put32(max_tab + (size_t)i * 4u, 0xffffu);
                continue;
            }

            valptr[i] = symbol_index;
            smid_put32(min_tab + (size_t)i * 4u, (uint32_t)code);
            symbol_index = (uint8_t)(symbol_index + bits[i]);
            code = code - 1 + bits[i];
            started = true;
            smid_put32(max_tab + (size_t)i * 4u, (uint32_t)code);
            code = code * 2 + 2;
        }
    }
}

static int subsampling_mode(const struct jpeg_cnm_info *info) {
    int key = ((int)info->samp_h[0] & 3) << 2 | ((int)info->samp_v[0] & 3);
    if (key == 9) {
        return 1;
    }
    if (key == 5) {
        return 3;
    }
    if (key == 6) {
        return 2;
    }
    if (key == 10) {
        return 0;
    }
    return 4;
}

static void fill_layout_fields(uint8_t *block, int mode, int width, int height) {
    switch (mode) {
    case 0:
        put64_le(block + 0xa9c, 0x300000002ull);
        put64_le(block + 0xaa4, 0xa00000006ull);
        put64_le(block + 0xaac, 0x500000005ull);
        smid_put32(block + 0x7c, (uint32_t)align_up_int(width, 16));
        smid_put32(block + 0x80, (uint32_t)align_up_int(height, 16));
        put64_le(block + 0xac4, 0x1000000010ull);
        break;
    case 1:
        put64_le(block + 0xa9c, 0x300000003ull);
        put64_le(block + 0xaa4, 0x900000004ull);
        put64_le(block + 0xaac, 0x500000005ull);
        smid_put32(block + 0x7c, (uint32_t)align_up_int(width, 16));
        smid_put32(block + 0x80, (uint32_t)align_up_int(height, 8));
        put64_le(block + 0xac4, 0x800000010ull);
        break;
    case 2:
        put64_le(block + 0xa9c, 0x300000003ull);
        put64_le(block + 0xaa4, 0x600000004ull);
        put64_le(block + 0xaac, 0x500000005ull);
        smid_put32(block + 0x7c, (uint32_t)align_up_int(width, 8));
        smid_put32(block + 0x80, (uint32_t)align_up_int(height, 16));
        put64_le(block + 0xac4, 0x1000000008ull);
        break;
    case 3:
        put64_le(block + 0xa9c, 0x300000004ull);
        put64_le(block + 0xaa4, 0x500000003ull);
        put64_le(block + 0xaac, 0x500000005ull);
        smid_put32(block + 0x7c, (uint32_t)align_up_int(width, 8));
        smid_put32(block + 0x80, (uint32_t)align_up_int(height, 8));
        put64_le(block + 0xac4, 0x800000008ull);
        break;
    default:
        put64_le(block + 0xa9c, 0x100000004ull);
        put64_le(block + 0xaa4, 0x500000001ull);
        put64_le(block + 0xaac, 0);
        smid_put32(block + 0x7c, (uint32_t)align_up_int(width, 8));
        smid_put32(block + 0x80, (uint32_t)align_up_int(height, 8));
        put64_le(block + 0xac4, 0x800000008ull);
        break;
    }
}

static void build_local_cnm_block(uint8_t *block, const struct jpeg_cnm_info *info,
                                  uint32_t encoded_len) {
    memset(block, 0, 0xb28);
    smid_put32(block + 0x14, encoded_len);
    smid_put32(block + 0x74, (uint32_t)info->width);
    smid_put32(block + 0x78, (uint32_t)info->height);
    smid_put32(block + 0x84, (uint32_t)info->scan_data_offset);
    smid_put32(block + 0x88, (uint32_t)info->scan_data_offset);
    smid_put32(block + 0x8c, (uint32_t)(info->scan_data_offset >> 8));
    uint32_t scan_pack = (uint32_t)((info->scan_data_offset >> 2) & 0x3c);
    if (((info->scan_data_offset >> 8) & 1) != 0) {
        scan_pack += 0x40;
    }
    smid_put32(block + 0x90, scan_pack);
    smid_put32(block + 0x94, (uint32_t)((info->scan_data_offset & 0xf) << 3));
    smid_put32(block + 0x98, (uint32_t)subsampling_mode(info));
    smid_put32(block + 0x9c, (uint32_t)info->restart_interval);

    for (int i = 0; i < info->components && i < 4; i++) {
        uint8_t *d = block + 0x738u + (size_t)i * 6u;
        d[0] = info->comp_id[i];
        d[1] = info->samp_h[i];
        d[2] = info->samp_v[i];
        d[3] = info->qtable[i];
        d[4] = info->dc_table[i];
        d[5] = info->ac_table[i];
    }
    for (int t = 0; t < 4; t++) {
        if (info->qtables & (1 << t)) {
            memcpy(block + 0x750u + (size_t)t * 0x40u, info->qdata[t], 64);
        }
        if (info->huff_tables & (1 << t)) {
            memcpy(block + 0x338u + (size_t)t * 0x100u, info->huff_bits[t], 16);
            memcpy(block + 0x0b0u + (size_t)t * 0xa2u, info->huff_vals[t],
                   (size_t)info->huff_val_count[t]);
        }
    }

    block[0xa4] = (uint8_t)(((block[0x742] | (block[0x73c] * 2u)) * 2u) | block[0x748]);
    block[0xa8] = (uint8_t)(((block[0x743] | (block[0x73d] * 2u)) * 2u) | block[0x749]);
    block[0xac] = (uint8_t)(((block[0x741] | (block[0x73b] * 2u)) * 2u) | block[0x747]);
    build_derived_huffman(block);
    fill_layout_fields(block, subsampling_mode(info), info->width, info->height);
    smid_put32(block + 0xad0, (uint32_t)info->scan_data_offset);
    smid_put32(block + 0xad4, encoded_len);
}

static int build_local_cnm_header(const uint8_t *jpeg, uint32_t encoded_len,
                                  uint8_t *cnm_out, uint32_t cnm_cap) {
    if (cnm_cap < 0x1651u) {
        return -1;
    }
    struct jpeg_cnm_info info;
    if (parse_jpeg_for_cnm(jpeg, encoded_len, &info) ||
        info.width <= 0 || info.height <= 0 || info.components <= 0 ||
        info.scan_data_offset <= 0) {
        return -1;
    }
    memset(cnm_out, 0, cnm_cap);
    cnm_out[0] = 2;
    build_local_cnm_block(cnm_out + 1, &info, encoded_len);
    build_local_cnm_block(cnm_out + 0xb29, &info, encoded_len);
    return 0x1651;
}

int smid_cnm_init(struct smid_cnm *cnm) {
    memset(cnm, 0, sizeof(*cnm));
    cnm->initialized = true;
    return 0;
}

void smid_cnm_destroy(struct smid_cnm *cnm) {
    memset(cnm, 0, sizeof(*cnm));
}

int smid_cnm_build_header_base(struct smid_cnm *cnm, uint8_t *jpeg_slot,
                               uint32_t encoded_len, uint8_t *cnm_out,
                               bool patch_base, uint32_t base) {
    (void)cnm;
    memset(cnm_out, 0, SMID_CNM_SIZE);
    int rc = build_local_cnm_header(jpeg_slot, encoded_len, cnm_out, SMID_CNM_SIZE);
    if (rc <= 0) {
        smid_logf("local cnm header generation failed rc=%d\n", rc);
        return -1;
    }

    if (patch_base) {
        uint32_t end = base + encoded_len;
        smid_put32(cnm_out + 1, end);
        smid_put32(cnm_out + 13, base);
        smid_put32(cnm_out + 0xb29, end);
        smid_put32(cnm_out + 0xb35, base);
    }
    return 0;
}
