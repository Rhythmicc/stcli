/* Compact binary license paste codec for stcli.
 * Decodes ST. tokens into verified PGL-LICENSE-1 .key blobs
 * matching store/stcli_codec.py and store/pangulu_licensing.py byte-for-byte. */
#ifndef GPU_ID_LICENSE_CODE_H
#define GPU_ID_LICENSE_CODE_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ST_LIC3_PREFIX "ST."
#define ST_LIC3_PREFIX_LEN 3u
#define ST_LIC2_MAX_TEXT 131072u
#define ST_LIC2_MAX_BIN 65536u
#define ST_LIC2_MAX_ENTRIES 64u
#define ST_LIC2_MAX_RAW_PAYLOAD 8192u

/* Compact v3 (ST.) framing & flags */
#define ST3_EPOCH 1700000000ULL
#define ST3_HDR_VERSION 0x40u
#define ST3_HDR_MASK 0xC0u

#define ST3_FLAG_RAW 0x01u
#define ST3_FLAG_SAME_META 0x02u
#define ST3_FLAG_CUSTOM_LID 0x04u
#define ST3_FLAG_RESERVED 0x08u
#define ST3_FLAG_CUSTOM_META 0x10u

#define ST3_EXT_CUSTOM_VER 0x01u
#define ST3_EXT_SEP_NOT_BEFORE 0x02u
#define ST3_EXT_CUSTOM_CUSTOMER 0x04u
#define ST3_EXT_EXPLICIT_EXPIRY 0x08u
#define ST3_EXT_HOURS_EXPIRY 0x10u
#define ST3_EXT_RAW_ISSUED_AT 0x20u
#define ST3_EXT_RESERVED 0xC0u

typedef struct {
    unsigned char *data;
    size_t len;
} StLic2KeyBlob;

typedef struct {
    const char *product;
    const char *prefix;
    const char *term;
    char min_ver[65];
    char max_ver[65];
    char customer[129];
    uint64_t issued_at;
    uint64_t not_before;
    uint64_t not_after;
} StLic2Meta;

static uint32_t st_lic2_u32be(const unsigned char *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint32_t st_lic3_u24be(const unsigned char *p) {
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[2];
}

static int st_lic2_read_uvarint(
    const unsigned char *buf,
    size_t end,
    size_t *pos,
    uint64_t *out
) {
    uint64_t val = 0;
    for (unsigned i = 0; i < 10u; i++) {
        if (*pos >= end) return 0;
        unsigned char b = buf[(*pos)++];
        if (i == 9u) {
            /* 10th byte holds only bit 63; must terminate and be canonical (0x01). */
            if (b != 0x01u) return 0;
            val |= ((uint64_t)1u << 63);
            *out = val;
            return 1;
        }
        val |= (uint64_t)(b & 0x7fu) << (i * 7u);
        if ((b & 0x80u) == 0u) {
            if (i > 0u && b == 0u) return 0; /* non-canonical overlong uvarint */
            *out = val;
            return 1;
        }
    }
    return 0;
}

static int st_lic2_read_bytes(
    const unsigned char *buf,
    size_t end,
    size_t *pos,
    size_t max_len,
    const unsigned char **out_ptr,
    size_t *out_len
) {
    uint64_t n = 0;
    if (!st_lic2_read_uvarint(buf, end, pos, &n)) return 0;
    if (n > (uint64_t)max_len || n > (uint64_t)(end - *pos)) return 0;
    *out_ptr = buf + *pos;
    *out_len = (size_t)n;
    *pos += (size_t)n;
    return 1;
}

static int st_lic2_ascii_printable(const unsigned char *p, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (p[i] < 32u || p[i] >= 127u) return 0;
    }
    return 1;
}

static int st_lic2_b64url_val(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return (int)(c - 'A');
    if (c >= 'a' && c <= 'z') return (int)(c - 'a' + 26);
    if (c >= '0' && c <= '9') return (int)(c - '0' + 52);
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}

static int st_lic2_b64url_decode(
    const char *src,
    size_t len,
    unsigned char *dst,
    size_t dst_cap,
    size_t *out_len
) {
    size_t rem = len % 4u;
    if (len == 0u || rem == 1u) return 0;
    size_t needed = (len / 4u) * 3u + (rem == 2u ? 1u : (rem == 3u ? 2u : 0u));
    if (needed > dst_cap) return 0;

    size_t di = 0u;
    size_t full_end = len - rem;
    for (size_t i = 0; i < full_end; i += 4u) {
        int a = st_lic2_b64url_val((unsigned char)src[i]);
        int b = st_lic2_b64url_val((unsigned char)src[i + 1u]);
        int c = st_lic2_b64url_val((unsigned char)src[i + 2u]);
        int d = st_lic2_b64url_val((unsigned char)src[i + 3u]);
        if (a < 0 || b < 0 || c < 0 || d < 0) return 0;
        dst[di++] = (unsigned char)((a << 2) | (b >> 4));
        dst[di++] = (unsigned char)(((b & 0x0f) << 4) | (c >> 2));
        dst[di++] = (unsigned char)(((c & 0x03) << 6) | d);
    }
    if (rem == 2u) {
        int a = st_lic2_b64url_val((unsigned char)src[full_end]);
        int b = st_lic2_b64url_val((unsigned char)src[full_end + 1u]);
        if (a < 0 || b < 0 || (b & 0x0f) != 0) return 0;
        dst[di++] = (unsigned char)((a << 2) | (b >> 4));
    } else if (rem == 3u) {
        int a = st_lic2_b64url_val((unsigned char)src[full_end]);
        int b = st_lic2_b64url_val((unsigned char)src[full_end + 1u]);
        int c = st_lic2_b64url_val((unsigned char)src[full_end + 2u]);
        if (a < 0 || b < 0 || c < 0 || (c & 0x03) != 0) return 0;
        dst[di++] = (unsigned char)((a << 2) | (b >> 4));
        dst[di++] = (unsigned char)(((b & 0x0f) << 4) | (c >> 2));
    }
    *out_len = di;
    return 1;
}

static size_t st_lic2_b64_encode(const unsigned char *src, size_t len, char *dst) {
    static const char TBL[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t di = 0u;
    for (size_t i = 0; i < len; i += 3u) {
        size_t rem = len - i;
        uint32_t a = src[i];
        uint32_t b = (rem > 1u) ? src[i + 1u] : 0u;
        uint32_t c = (rem > 2u) ? src[i + 2u] : 0u;
        uint32_t triple = (a << 16) | (b << 8) | c;
        dst[di++] = TBL[(triple >> 18) & 0x3fu];
        dst[di++] = TBL[(triple >> 12) & 0x3fu];
        dst[di++] = (rem > 1u) ? TBL[(triple >> 6) & 0x3fu] : '=';
        dst[di++] = (rem > 2u) ? TBL[triple & 0x3fu] : '=';
    }
    dst[di] = '\0';
    return di;
}

/* RFC 3174 SHA-1 */
static uint32_t st_lic2_rotl32(uint32_t v, unsigned n) {
    return (v << n) | (v >> (32u - n));
}

static void st_lic2_sha1(const unsigned char *data, size_t len, unsigned char out[20]) {
    uint32_t h[5] = {
        0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u, 0xc3d2e1f0u,
    };
    unsigned char block[64];
    size_t offset = 0u;
    uint64_t total_bits = (uint64_t)len * 8u;
    int phase = 0;

    while (1) {
        if (offset + 64u <= len) {
            memcpy(block, data + offset, 64u);
            offset += 64u;
        } else if (phase == 0) {
            size_t rem = len - offset;
            memset(block, 0, 64u);
            if (rem) memcpy(block, data + offset, rem);
            offset = len;
            block[rem] = 0x80u;
            if (rem <= 55u) {
                for (int i = 0; i < 8; i++) {
                    block[56 + i] = (unsigned char)((total_bits >> ((7 - i) * 8)) & 0xffu);
                }
                phase = 2;
            } else {
                phase = 1;
            }
        } else if (phase == 1) {
            memset(block, 0, 64u);
            for (int i = 0; i < 8; i++) {
                block[56 + i] = (unsigned char)((total_bits >> ((7 - i) * 8)) & 0xffu);
            }
            phase = 2;
        } else {
            break;
        }

        uint32_t w[80];
        for (int i = 0; i < 16; i++) {
            w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
                   ((uint32_t)block[i * 4 + 2] << 8) | (uint32_t)block[i * 4 + 3];
        }
        for (int i = 16; i < 80; i++) {
            w[i] = st_lic2_rotl32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1u);
        }

        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5a827999u;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ed9eba1u;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8f1bbcdcu;
            } else {
                f = b ^ c ^ d;
                k = 0xca62c1d6u;
            }
            uint32_t temp = st_lic2_rotl32(a, 5u) + f + e + k + w[i];
            e = d; d = c; c = st_lic2_rotl32(b, 30u); b = a; a = temp;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
        if (phase == 2) break;
    }

    for (int i = 0; i < 5; i++) {
        out[i * 4] = (unsigned char)((h[i] >> 24) & 0xffu);
        out[i * 4 + 1] = (unsigned char)((h[i] >> 16) & 0xffu);
        out[i * 4 + 2] = (unsigned char)((h[i] >> 8) & 0xffu);
        out[i * 4 + 3] = (unsigned char)(h[i] & 0xffu);
    }
}

static void st_lic3_deterministic_lid_hex(
    const char *product,
    const char *customer,
    uint64_t issued_at,
    uint64_t not_before,
    uint64_t not_after,
    const char *gpu_serial,
    char out_hex[17]
) {
    char msg[512];
    int n = snprintf(
        msg, sizeof(msg),
        "st-lid-v1\n%s\n%s\n%llu\n%llu\n%llu\n%s",
        product, customer,
        (unsigned long long)issued_at,
        (unsigned long long)not_before,
        (unsigned long long)not_after,
        gpu_serial
    );
    unsigned char digest[20];
    size_t len = (n > 0 && (size_t)n < sizeof(msg)) ? (size_t)n : (sizeof(msg) - 1u);
    st_lic2_sha1((const unsigned char *)msg, len, digest);
    for (int i = 0; i < 8; i++) snprintf(out_hex + i * 2, 3u, "%02x", digest[i]);
    out_hex[16] = '\0';
}

static int st_lic2_hex_chars(const unsigned char *p, size_t len) {
    if (len == 0u || len > 64u) return 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = p[i];
        int is_hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (!is_hex) return 0;
    }
    return 1;
}

static void st_lic2_format_uuid_canonical(const unsigned char u[16], char out[37]) {
    snprintf(out, 37u,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7],
             u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
}

/* Portable UTC civil calendar helpers matching pangulu_licensing.expiry() */
static void st_lic3_civil_from_days(int64_t z, int64_t *out_y, unsigned *out_m, unsigned *out_d) {
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    uint64_t doe = (uint64_t)(z - era * 146097);
    uint64_t yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    int64_t y = (int64_t)yoe + era * 400;
    uint64_t doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    uint64_t mp = (5u * doy + 2u) / 153u;
    unsigned d = (unsigned)(doy - (153u * mp + 2u) / 5u + 1u);
    unsigned m = (unsigned)(mp < 10u ? mp + 3u : mp - 9u);
    y += (m <= 2u);
    *out_y = y;
    *out_m = m;
    *out_d = d;
}

static int64_t st_lic3_days_from_civil(int64_t y, unsigned m, unsigned d) {
    y -= (m <= 2u);
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    uint64_t yoe = (uint64_t)(y - era * 400);
    uint64_t doy = (153u * (m > 2u ? m - 3u : m + 9u) + 2u) / 5u + d - 1u;
    uint64_t doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

static unsigned st_lic3_month_days(int64_t y, unsigned m) {
    static const unsigned DAYS[12] = {31u, 28u, 31u, 30u, 31u, 30u, 31u, 31u, 30u, 31u, 30u, 31u};
    if (m != 2u) return DAYS[m - 1u];
    int leap = ((y % 4 == 0) && ((y % 100 != 0) || (y % 400 == 0)));
    return leap ? 29u : 28u;
}

static int st_lic3_implicit_expiry(uint64_t issued_at, unsigned term_idx, uint64_t *out_na) {
    /* TERMS = {"hourly", "daily", "weekly", "monthly", "annual", "perpetual", "custom"} */
    if (term_idx == 5u) {
        *out_na = 0u;
        return 1;
    }
    if (term_idx == 0u || term_idx == 1u || term_idx == 2u) {
        uint64_t step = (term_idx == 0u) ? 3600ULL : (term_idx == 1u ? 86400ULL : 604800ULL);
        if (issued_at > UINT64_MAX - step) return 0;
        *out_na = issued_at + step;
        return 1;
    }
    if (term_idx == 3u || term_idx == 4u) {
        if (issued_at >= 253370764800ULL) return 0;
        int64_t days = (int64_t)(issued_at / 86400ULL);
        uint64_t tod = issued_at % 86400ULL;
        int64_t y = 0;
        unsigned m = 0u, d = 0u;
        st_lic3_civil_from_days(days, &y, &m, &d);
        int64_t ny = (term_idx == 4u) ? (y + 1) : (y + (m == 12u ? 1 : 0));
        unsigned nm = (term_idx == 4u) ? m : ((m % 12u) + 1u);
        unsigned md = st_lic3_month_days(ny, nm);
        unsigned nd = (d <= md) ? d : md;
        int64_t target_days = st_lic3_days_from_civil(ny, nm, nd);
        if (target_days < 0) return 0;
        *out_na = (uint64_t)target_days * 86400ULL + tod;
        return 1;
    }
    return 0;
}

static int st_lic3_format_serial(
    unsigned kind,
    const unsigned char *buf,
    size_t end,
    size_t *pos,
    char out[132]
) {
    static const char *const PREFIXES[] = {
        "cuda", "musa", "hip", "hc", "lisuan", "sycl"
    };
    if (kind <= 5u) {
        if (16u > end - *pos) return 0;
        const unsigned char *u = buf + *pos;
        *pos += 16u;
        snprintf(out, 132u,
                 "%s-%02x%02x%02x%02x-%02x%02x%02x%02x-%02x%02x%02x%02x-%02x%02x%02x%02x",
                 PREFIXES[kind],
                 u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7],
                 u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
        return 1;
    }
    if (kind == 6u) {
        if (16u > end - *pos) return 0;
        st_lic2_format_uuid_canonical(buf + *pos, out);
        *pos += 16u;
        return 1;
    }
    if (kind == 7u) {
        const unsigned char *p = NULL;
        size_t plen = 0;
        if (!st_lic2_read_bytes(buf, end, pos, 128u, &p, &plen) || plen == 0u) return 0;
        if (!st_valid_device_id((const char *)p, plen)) return 0;
        memcpy(out, p, plen);
        out[plen] = '\0';
        return 1;
    }
    return 0;
}

static int st_lic2_build_key_blob(
    const unsigned char *payload,
    size_t payload_len,
    const unsigned char sig[64],
    StLic2KeyBlob *out
) {
    size_t p_b64_len = ((payload_len + 2u) / 3u) * 4u;
    size_t s_b64_len = ((64u + 2u) / 3u) * 4u; /* 88 */
    size_t total = 14u + p_b64_len + 1u + s_b64_len + 1u;
    unsigned char *buf = (unsigned char *)malloc(total + 1u);
    if (!buf) return 0;
    size_t pos = 0u;
    memcpy(buf + pos, "PGL-LICENSE-1\n", 14u);
    pos += 14u;
    pos += st_lic2_b64_encode(payload, payload_len, (char *)(buf + pos));
    buf[pos++] = '\n';
    pos += st_lic2_b64_encode(sig, 64u, (char *)(buf + pos));
    buf[pos++] = '\n';
    buf[pos] = '\0';
    out->data = buf;
    out->len = pos;
    return 1;
}

static int st_lic2_strip_ws(
    const char *in,
    size_t in_len,
    char *out,
    size_t out_cap,
    size_t *out_len
) {
    if (!in || in_len > ST_LIC2_MAX_TEXT) return 0;
    size_t di = 0u;
    for (size_t i = 0; i < in_len; i++) {
        char ch = in[i];
        if (ch == '\0') return 0;
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') continue;
        if (di + 1u >= out_cap) return 0;
        out[di++] = ch;
    }
    out[di] = '\0';
    *out_len = di;
    return 1;
}

static int st_lic_is_code_prefix(const char *arg) {
    if (!arg) return 0;
    char head[ST_LIC3_PREFIX_LEN];
    size_t n = 0;
    for (const char *p = arg; *p && n < ST_LIC3_PREFIX_LEN; p++) {
        if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') continue;
        head[n++] = *p;
    }
    return n == ST_LIC3_PREFIX_LEN && memcmp(head, ST_LIC3_PREFIX, ST_LIC3_PREFIX_LEN) == 0;
}

static int st_lic2_decode_envelope(
    const char *text,
    size_t text_len,
    unsigned char **bin_out,
    size_t *bin_len_out,
    char *err,
    size_t err_cap
) {
    *bin_out = NULL;
    *bin_len_out = 0u;
    if (!text || text_len > ST_LIC2_MAX_TEXT) {
        if (err) snprintf(err, err_cap, "license code exceeds %u bytes", ST_LIC2_MAX_TEXT);
        return 0;
    }
    char *stripped = (char *)malloc(ST_LIC2_MAX_TEXT + 1u);
    unsigned char *bin = (unsigned char *)malloc(ST_LIC2_MAX_BIN);
    if (!stripped || !bin) {
        free(stripped);
        free(bin);
        if (err) snprintf(err, err_cap, "out of memory");
        return 0;
    }
    size_t slen = 0u;
    if (!st_lic2_strip_ws(text, text_len, stripped, ST_LIC2_MAX_TEXT + 1u, &slen)) {
        free(stripped);
        free(bin);
        if (err) snprintf(err, err_cap, "invalid license code input (contains NUL or exceeds max length)");
        return 0;
    }

    if (slen <= ST_LIC3_PREFIX_LEN || memcmp(stripped, ST_LIC3_PREFIX, ST_LIC3_PREFIX_LEN) != 0) {
        free(stripped);
        free(bin);
        if (err) snprintf(err, err_cap, "invalid license code prefix (expected ST.)");
        return 0;
    }

    size_t blen = 0u;
    if (!st_lic2_b64url_decode(stripped + ST_LIC3_PREFIX_LEN, slen - ST_LIC3_PREFIX_LEN,
                               bin, ST_LIC2_MAX_BIN, &blen)) {
        free(stripped);
        free(bin);
        if (err) snprintf(err, err_cap, "invalid license code base64url encoding");
        return 0;
    }
    free(stripped);

    if (blen < 5u) {
        free(bin);
        if (err) snprintf(err, err_cap, "truncated ST. license code");
        return 0;
    }
    if ((bin[0] & ST3_HDR_MASK) != ST3_HDR_VERSION) {
        free(bin);
        if (err) snprintf(err, err_cap, "unsupported ST. license code version");
        return 0;
    }
    uint32_t want_crc24 = st_lic3_u24be(bin + blen - 3u);
    uint32_t got_crc24 = (st_crc32(bin, blen - 3u) >> 8) & 0xFFFFFFu;
    if (want_crc24 != got_crc24) {
        free(bin);
        if (err) snprintf(err, err_cap, "ST. checksum failed (code may be incomplete or corrupted)");
        return 0;
    }

    *bin_out = bin;
    *bin_len_out = blen;
    return 1;
}

static int st_lic2_has_valid_crc(const char *text, size_t text_len) {
    unsigned char *bin = NULL;
    size_t blen = 0u;
    int ok = st_lic2_decode_envelope(text, text_len, &bin, &blen, NULL, 0u);
    if (ok && blen < 70u) ok = 0;
    free(bin);
    return ok;
}

static void st_lic2_free_blobs(StLic2KeyBlob *blobs, size_t count) {
    if (!blobs) return;
    for (size_t i = 0; i < count; i++) free(blobs[i].data);
    free(blobs);
}

static int st_lic2_decode(
    const char *text,
    size_t text_len,
    StLic2KeyBlob **out_blobs,
    size_t *out_count,
    char *err,
    size_t err_cap
) {
    static const char *const PRODUCTS[] = {"pangulu", "linglongamg", "xianzhikrylov"};
    static const char *const PREFIXES[] = {"pgl-", "lla-", "xzk-"};
    static const char *const TERMS[] = {
        "hourly", "daily", "weekly", "monthly", "annual", "perpetual", "custom"
    };

    *out_blobs = NULL;
    *out_count = 0u;

    unsigned char *bin = NULL;
    size_t blen = 0u;
    if (!st_lic2_decode_envelope(text, text_len, &bin, &blen, err, err_cap)) return 0;

    size_t count = (size_t)((bin[0] & 0x3Fu) + 1u);
    size_t pos = 1u;
    size_t end = blen - 3u;

    StLic2KeyBlob *blobs = (StLic2KeyBlob *)calloc(count, sizeof(StLic2KeyBlob));
    if (!blobs) {
        free(bin);
        snprintf(err, err_cap, "out of memory");
        return 0;
    }

    int prev_canonical = 0;
    StLic2Meta meta;
    memset(&meta, 0, sizeof(meta));

    for (size_t i = 0; i < count; i++) {
        if (pos >= end) {
            snprintf(err, err_cap, "truncated license entry %zu", i + 1u);
            goto fail;
        }
        unsigned char flags = bin[pos++];
        if ((flags & ST3_FLAG_RAW) != 0u) {
            if (flags != ST3_FLAG_RAW) {
                snprintf(err, err_cap, "invalid flags (0x%02x) on raw license entry %zu", (unsigned)flags, i + 1u);
                goto fail;
            }
            const unsigned char *payload = NULL;
            size_t payload_len = 0u;
            if (!st_lic2_read_bytes(bin, end, &pos, ST_LIC2_MAX_RAW_PAYLOAD, &payload, &payload_len) ||
                payload_len == 0u || memchr(payload, '\0', payload_len) != NULL) {
                snprintf(err, err_cap, "invalid payload in raw license entry %zu", i + 1u);
                goto fail;
            }
            if (64u > end - pos) {
                snprintf(err, err_cap, "truncated signature in raw license entry %zu", i + 1u);
                goto fail;
            }
            const unsigned char *sig = bin + pos;
            pos += 64u;
            if (!st_lic2_build_key_blob(payload, payload_len, sig, &blobs[i])) {
                snprintf(err, err_cap, "out of memory");
                goto fail;
            }
            prev_canonical = 0;
            continue;
        }

        if ((flags & ST3_FLAG_RESERVED) != 0u) {
            snprintf(err, err_cap, "reserved flag set on ST. entry %zu", i + 1u);
            goto fail;
        }

        if ((flags & ST3_FLAG_SAME_META) != 0u) {
            if (i == 0u || !prev_canonical) {
                snprintf(err, err_cap, "ST. entry %zu sets SAME_META without prior canonical entry", i + 1u);
                goto fail;
            }
            if ((flags & ST3_FLAG_CUSTOM_META) != 0u) {
                snprintf(err, err_cap, "ST. entry %zu combines SAME_META with CUSTOM_META", i + 1u);
                goto fail;
            }
        }
        if ((flags & ST3_FLAG_SAME_META) == 0u) {
            if (pos >= end) {
                snprintf(err, err_cap, "truncated metadata in ST. entry %zu", i + 1u);
                goto fail;
            }
            unsigned char pt = bin[pos++];
            unsigned pid = (unsigned)(pt >> 4);
            unsigned tid = (unsigned)(pt & 0x0fu);
            if (pid >= sizeof(PRODUCTS) / sizeof(PRODUCTS[0]) || tid >= sizeof(TERMS) / sizeof(TERMS[0])) {
                snprintf(err, err_cap, "unsupported product/term in ST. entry %zu", i + 1u);
                goto fail;
            }
            meta.product = PRODUCTS[pid];
            meta.prefix = PREFIXES[pid];
            meta.term = TERMS[tid];

            unsigned char ext = 0u;
            if ((flags & ST3_FLAG_CUSTOM_META) != 0u) {
                if (pos >= end) {
                    snprintf(err, err_cap, "truncated meta_ext in ST. entry %zu", i + 1u);
                    goto fail;
                }
                ext = bin[pos++];
                if ((ext & ST3_EXT_RESERVED) != 0u) {
                    snprintf(err, err_cap, "reserved ext flag set on ST. entry %zu", i + 1u);
                    goto fail;
                }
                if ((ext & ST3_EXT_EXPLICIT_EXPIRY) && (ext & ST3_EXT_HOURS_EXPIRY)) {
                    snprintf(err, err_cap, "conflicting expiry flags on ST. entry %zu", i + 1u);
                    goto fail;
                }
            }

            if ((ext & ST3_EXT_CUSTOM_VER) != 0u) {
                const unsigned char *vp = NULL;
                size_t vlen = 0u;
                if (!st_lic2_read_bytes(bin, end, &pos, 64u, &vp, &vlen) ||
                    vlen == 0u || !st_lic2_ascii_printable(vp, vlen)) {
                    snprintf(err, err_cap, "invalid min_ver in ST. entry %zu", i + 1u);
                    goto fail;
                }
                memcpy(meta.min_ver, vp, vlen);
                meta.min_ver[vlen] = '\0';
                if (!st_lic2_read_bytes(bin, end, &pos, 64u, &vp, &vlen) ||
                    !st_lic2_ascii_printable(vp, vlen)) {
                    snprintf(err, err_cap, "invalid max_ver in ST. entry %zu", i + 1u);
                    goto fail;
                }
                memcpy(meta.max_ver, vp, vlen);
                meta.max_ver[vlen] = '\0';
            } else {
                snprintf(meta.min_ver, sizeof(meta.min_ver), "0.1.0");
                meta.max_ver[0] = '\0';
            }

            if ((ext & ST3_EXT_CUSTOM_CUSTOMER) != 0u) {
                const unsigned char *cp = NULL;
                size_t clen = 0u;
                if (!st_lic2_read_bytes(bin, end, &pos, 128u, &cp, &clen) ||
                    clen == 0u || !st_lic2_ascii_printable(cp, clen)) {
                    snprintf(err, err_cap, "invalid customer in ST. entry %zu", i + 1u);
                    goto fail;
                }
                memcpy(meta.customer, cp, clen);
                meta.customer[clen] = '\0';
            } else {
                uint64_t cust_num = 0;
                if (!st_lic2_read_uvarint(bin, end, &pos, &cust_num)) {
                    snprintf(err, err_cap, "invalid customer_num in ST. entry %zu", i + 1u);
                    goto fail;
                }
                snprintf(meta.customer, sizeof(meta.customer),
                         "opensparse-account-%llu", (unsigned long long)cust_num);
            }

            if ((ext & ST3_EXT_RAW_ISSUED_AT) != 0u) {
                if (!st_lic2_read_uvarint(bin, end, &pos, &meta.issued_at)) {
                    snprintf(err, err_cap, "invalid issued_at in ST. entry %zu", i + 1u);
                    goto fail;
                }
            } else {
                if (4u > end - pos) {
                    snprintf(err, err_cap, "truncated issued_at in ST. entry %zu", i + 1u);
                    goto fail;
                }
                meta.issued_at = ST3_EPOCH + (uint64_t)st_lic2_u32be(bin + pos);
                pos += 4u;
            }

            if ((ext & ST3_EXT_SEP_NOT_BEFORE) != 0u) {
                if (!st_lic2_read_uvarint(bin, end, &pos, &meta.not_before)) {
                    snprintf(err, err_cap, "invalid not_before in ST. entry %zu", i + 1u);
                    goto fail;
                }
            } else {
                meta.not_before = meta.issued_at;
            }

            if ((ext & ST3_EXT_HOURS_EXPIRY) != 0u) {
                uint64_t hours = 0;
                if (!st_lic2_read_uvarint(bin, end, &pos, &hours) ||
                    hours < 1u || hours > (UINT64_MAX - meta.issued_at) / 3600ULL) {
                    snprintf(err, err_cap, "invalid hours expiry in ST. entry %zu", i + 1u);
                    goto fail;
                }
                meta.not_after = meta.issued_at + hours * 3600ULL;
            } else if ((ext & ST3_EXT_EXPLICIT_EXPIRY) != 0u) {
                uint64_t delta = 0;
                if (!st_lic2_read_uvarint(bin, end, &pos, &delta)) {
                    snprintf(err, err_cap, "invalid explicit expiry in ST. entry %zu", i + 1u);
                    goto fail;
                }
                if (delta == 0u) {
                    meta.not_after = 0u;
                } else {
                    if (delta > UINT64_MAX - meta.issued_at) {
                        snprintf(err, err_cap, "not_after overflow in ST. entry %zu", i + 1u);
                        goto fail;
                    }
                    meta.not_after = meta.issued_at + delta;
                }
            } else {
                if (!st_lic3_implicit_expiry(meta.issued_at, tid, &meta.not_after)) {
                    snprintf(err, err_cap, "missing custom expiry in ST. entry %zu", i + 1u);
                    goto fail;
                }
            }
        }

        char gpu_serial[132];
        unsigned serial_kind = (unsigned)((flags >> 5) & 0x07u);
        if (!st_lic3_format_serial(serial_kind, bin, end, &pos, gpu_serial)) {
            snprintf(err, err_cap, "invalid gpu_serial in ST. entry %zu", i + 1u);
            goto fail;
        }

        char lid_suffix[65];
        if ((flags & ST3_FLAG_CUSTOM_LID) != 0u) {
            const unsigned char *lp = NULL;
            size_t llen = 0u;
            if (!st_lic2_read_bytes(bin, end, &pos, 64u, &lp, &llen) ||
                !st_lic2_hex_chars(lp, llen)) {
                snprintf(err, err_cap, "invalid custom license_id in ST. entry %zu", i + 1u);
                goto fail;
            }
            memcpy(lid_suffix, lp, llen);
            lid_suffix[llen] = '\0';
        } else {
            st_lic3_deterministic_lid_hex(
                meta.product, meta.customer, meta.issued_at,
                meta.not_before, meta.not_after, gpu_serial, lid_suffix
            );
        }

        if (64u > end - pos) {
            snprintf(err, err_cap, "truncated signature in ST. entry %zu", i + 1u);
            goto fail;
        }
        const unsigned char *sig = bin + pos;
        pos += 64u;

        char payload_buf[1024];
        int pn = snprintf(
            payload_buf, sizeof(payload_buf),
            "license_id=%s%llu-%s\n"
            "product=%s\n"
            "min_ver=%s\n"
            "max_ver=%s\n"
            "gpu_serial=%s\n"
            "issued_at=%llu\n"
            "not_before=%llu\n"
            "not_after=%llu\n"
            "customer=%s\n"
            "license_tier=user\n",
            meta.prefix, (unsigned long long)meta.issued_at, lid_suffix,
            meta.product,
            meta.min_ver,
            meta.max_ver,
            gpu_serial,
            (unsigned long long)meta.issued_at,
            (unsigned long long)meta.not_before,
            (unsigned long long)meta.not_after,
            meta.customer
        );
        if (pn <= 0 || (size_t)pn >= sizeof(payload_buf)) {
            snprintf(err, err_cap, "reconstructed license payload too long on entry %zu", i + 1u);
            goto fail;
        }
        if (!st_lic2_build_key_blob((const unsigned char *)payload_buf, (size_t)pn, sig, &blobs[i])) {
            snprintf(err, err_cap, "out of memory");
            goto fail;
        }
        prev_canonical = 1;
    }

    if (pos != end) {
        snprintf(err, err_cap, "unexpected trailing bytes in license code stream");
        goto fail;
    }

    free(bin);
    *out_blobs = blobs;
    *out_count = count;
    return 1;

fail:
    free(bin);
    st_lic2_free_blobs(blobs, count);
    return 0;
}

#endif
