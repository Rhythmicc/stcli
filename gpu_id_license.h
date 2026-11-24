/* Client-side offline license inspection and installation for Solved Tech solvers.
 * Decodes ST. license codes into verified PGL-LICENSE-1 keys under
 * ~/.solved-tech/<LibName>/ (or /etc/solved-tech/<LibName>/ with --system),
 * replacing prior keys for the same (product, gpu_serial). */
#ifndef GPU_ID_LICENSE_H
#define GPU_ID_LICENSE_H

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#ifdef _WIN32
#include <conio.h>
#include <direct.h>
#include <io.h>
#include <windows.h>
#else
#include <dirent.h>
#include <signal.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>
#endif

#define ST_MAX_KEY_BYTES 65536u
#define ST_MAX_LICENSE_BATCH 512u
#define ST_PATH_CAP 4096

typedef struct {
    const char *product;
    const char *lib_name;
} StProductInfo;

static const StProductInfo ST_PRODUCTS[] = {
    {"pangulu", "PanguLU"},
    {"linglongamg", "LinglongAMG"},
    {"xianzhikrylov", "XianzhiKrylov"},
};

typedef struct {
    char product[32];
    char lib_name[32];
    char license_id[132];
    char gpu_serial[132];
    char order_id[132];
    char term[36];
    uint64_t issued_at;
    uint64_t not_before;
    uint64_t not_after;
    int has_issued_at;
    int has_not_before;
    char sha256[65];
    unsigned char *raw;
    size_t raw_len;
} StLicense;

typedef struct {
    char target_name[256];
    char target_path[ST_PATH_CAP];
    char product[32];
    char lib_name[32];
    char license_id[132];
    char gpu_serial[132];
    uint64_t not_after;
    char sha256[65];
    const char *action;
} StApplyResult;

typedef struct {
    char product[32];
    char lib_name[32];
    char filename[256];
    char path[ST_PATH_CAP];
    char status[16];
    char license_id[132];
    char gpu_serial[132];
    char order_id[132];
    char term[36];
    uint64_t issued_at;
    uint64_t not_before;
    uint64_t not_after;
    char sha256[65];
    char error[256];
} StListedLicense;

typedef enum {
    ST_SCOPE_USER = 0,
    ST_SCOPE_SYSTEM = 1,
} StScope;

/* Defined in gpu_id.c */
static void json_string(FILE *out, const char *s);
static int st_use_color(void);

static int st_find_product_index(const char *product) {
    if (!product || !*product) return 999;
    for (size_t i = 0; i < sizeof(ST_PRODUCTS) / sizeof(ST_PRODUCTS[0]); i++) {
        if (!strcmp(product, ST_PRODUCTS[i].product)) return (int)i;
    }
    return 999;
}

static const StProductInfo *st_find_product(const char *name, int case_insensitive) {
    if (!name || !*name) return NULL;
    for (size_t i = 0; i < sizeof(ST_PRODUCTS) / sizeof(ST_PRODUCTS[0]); i++) {
        if (case_insensitive) {
            const char *a = name, *b = ST_PRODUCTS[i].lib_name;
            int eq = 1;
            while (*a && *b) {
                if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) { eq = 0; break; }
                a++; b++;
            }
            if (eq && *a == '\0' && *b == '\0') return &ST_PRODUCTS[i];
        } else {
            if (!strcmp(name, ST_PRODUCTS[i].product)) return &ST_PRODUCTS[i];
        }
    }
    return NULL;
}

static int st_path_join(char *out, size_t cap, const char *dir, const char *leaf) {
    size_t dl = strlen(dir);
    size_t ll = strlen(leaf);
    if (!dl || !ll) return 0;
    while (dl > 1 && (dir[dl - 1] == '/' || dir[dl - 1] == '\\')) dl--;
#ifdef _WIN32
    char sep = '\\';
#else
    char sep = '/';
#endif
    int need_sep = !(dl == 1 && (dir[0] == '/' || dir[0] == '\\'));
    if (dl + (size_t)need_sep + ll + 1 > cap) return 0;
    memcpy(out, dir, dl);
    size_t pos = dl;
    if (need_sep) out[pos++] = sep;
    memcpy(out + pos, leaf, ll);
    out[pos + ll] = '\0';
    return 1;
}

static int st_has_suffix(const char *s, const char *suffix) {
    size_t sl = strlen(s), xl = strlen(suffix);
    return sl >= xl && !strcmp(s + (sl - xl), suffix);
}

static int st_bytes_contains(const unsigned char *hay, size_t hay_len, const char *needle) {
    size_t nlen = strlen(needle);
    if (!nlen || hay_len < nlen) return 0;
    for (size_t i = 0; i <= hay_len - nlen; i++) {
        if (!memcmp(hay + i, needle, nlen)) return 1;
    }
    return 0;
}

/* FIPS 180-4 SHA-256 */
static void st_sha256_raw(const unsigned char *data, size_t len, unsigned char out[32]) {
    static const uint32_t K[64] = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
        0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
        0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
        0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
        0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
        0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
        0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
        0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
    };
    uint32_t state[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
    };
    unsigned char block[64];
    size_t offset = 0;
    uint64_t total_bits = (uint64_t)len * 8u;
    int padded_phase = 0;

    while (1) {
        if (offset + 64u <= len) {
            memcpy(block, data + offset, 64u);
            offset += 64u;
        } else if (padded_phase == 0) {
            size_t rem = len - offset;
            memset(block, 0, 64u);
            if (rem) memcpy(block, data + offset, rem);
            offset = len;
            block[rem] = 0x80u;
            if (rem <= 55u) {
                for (int i = 0; i < 8; i++) block[56 + i] = (unsigned char)((total_bits >> ((7 - i) * 8)) & 0xffu);
                padded_phase = 2;
            } else {
                padded_phase = 1;
            }
        } else if (padded_phase == 1) {
            memset(block, 0, 64u);
            for (int i = 0; i < 8; i++) block[56 + i] = (unsigned char)((total_bits >> ((7 - i) * 8)) & 0xffu);
            padded_phase = 2;
        } else {
            break;
        }

        uint32_t w[64];
        for (int i = 0; i < 16; i++) {
            w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
                   ((uint32_t)block[i * 4 + 2] << 8) | (uint32_t)block[i * 4 + 3];
        }
        for (int i = 16; i < 64; i++) {
            uint32_t s0 = ((w[i - 15] >> 7) | (w[i - 15] << 25)) ^ ((w[i - 15] >> 18) | (w[i - 15] << 14)) ^ (w[i - 15] >> 3);
            uint32_t s1 = ((w[i - 2] >> 17) | (w[i - 2] << 15)) ^ ((w[i - 2] >> 19) | (w[i - 2] << 13)) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
        uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
        for (int i = 0; i < 64; i++) {
            uint32_t S1 = ((e >> 6) | (e << 26)) ^ ((e >> 11) | (e << 21)) ^ ((e >> 25) | (e << 7));
            uint32_t ch = (e & f) ^ ((~e) & g);
            uint32_t t1 = h + S1 + ch + K[i] + w[i];
            uint32_t S0 = ((a >> 2) | (a << 30)) ^ ((a >> 13) | (a << 19)) ^ ((a >> 22) | (a << 10));
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = S0 + maj;
            h = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }
        state[0] += a; state[1] += b; state[2] += c; state[3] += d;
        state[4] += e; state[5] += f; state[6] += g; state[7] += h;
        if (padded_phase == 2) break;
    }

    for (int i = 0; i < 8; i++) {
        out[i * 4] = (unsigned char)((state[i] >> 24) & 0xffu);
        out[i * 4 + 1] = (unsigned char)((state[i] >> 16) & 0xffu);
        out[i * 4 + 2] = (unsigned char)((state[i] >> 8) & 0xffu);
        out[i * 4 + 3] = (unsigned char)(state[i] & 0xffu);
    }
}

static void st_sha256_hex(const unsigned char *data, size_t len, char hex_out[65]) {
    unsigned char raw[32];
    st_sha256_raw(data, len, raw);
    for (int i = 0; i < 32; i++) snprintf(hex_out + i * 2, 3, "%02x", raw[i]);
    hex_out[64] = '\0';
}

/* IEEE 802.3 CRC-32 */
static uint32_t st_crc32(const unsigned char *data, size_t len) {
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ ((crc & 1u) ? 0xedb88320u : 0u);
        }
    }
    return ~crc;
}

static int st_b64_val(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static int st_b64_decode(const char *src, size_t len, unsigned char *dst, size_t dst_cap, size_t *out_len) {
    if (!len || (len % 4u) != 0u) return 0;
    size_t pad = 0;
    if (src[len - 1] == '=') {
        pad++;
        if (src[len - 2] == '=') pad++;
    }
    size_t needed = (len / 4u) * 3u - pad;
    if (needed > dst_cap) return 0;
    size_t di = 0;
    for (size_t i = 0; i < len; i += 4u) {
        int last = (i + 4u == len);
        int a = st_b64_val((unsigned char)src[i]);
        int b = st_b64_val((unsigned char)src[i + 1]);
        if (a < 0 || b < 0) return 0;
        unsigned char c3 = (unsigned char)src[i + 2];
        unsigned char c4 = (unsigned char)src[i + 3];
        if (c3 == '=') {
            if (!last || c4 != '=' || (b & 0x0f) != 0) return 0;
            dst[di++] = (unsigned char)((a << 2) | (b >> 4));
        } else {
            int c = st_b64_val(c3);
            if (c < 0) return 0;
            if (c4 == '=') {
                if (!last || (c & 0x03) != 0) return 0;
                dst[di++] = (unsigned char)((a << 2) | (b >> 4));
                dst[di++] = (unsigned char)(((b & 0x0f) << 4) | (c >> 2));
            } else {
                int d = st_b64_val(c4);
                if (d < 0) return 0;
                dst[di++] = (unsigned char)((a << 2) | (b >> 4));
                dst[di++] = (unsigned char)(((b & 0x0f) << 4) | (c >> 2));
                dst[di++] = (unsigned char)(((c & 0x03) << 6) | d);
            }
        }
    }
    *out_len = di;
    return 1;
}

static int st_parse_u64(const char *s, size_t len, uint64_t *out) {
    if (!len) return 0;
    uint64_t v = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') return 0;
        uint64_t d = (uint64_t)(s[i] - '0');
        if (v > (UINT64_MAX - d) / 10u) return 0;
        v = v * 10u + d;
    }
    *out = v;
    return 1;
}

static int st_valid_license_id(const char *s, size_t len) {
    if (!len || len > 128) return 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (!isalnum(c) && c != '.' && c != '_' && c != '-') return 0;
    }
    return 1;
}

static int st_valid_device_id(const char *s, size_t len) {
    if (!len || len > 128) return 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (!isalnum(c) && c != '_' && c != '.' && c != ':' && c != '/' && c != '-') return 0;
    }
    return 1;
}

#include "gpu_id_license_code.h"

static int st_valid_safe_token(const char *s, size_t len, size_t max_len) {
    if (!len || len > max_len) return 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (!isalnum(c) && c != '.' && c != '_' && c != '-') return 0;
    }
    return 1;
}

static void st_license_free(StLicense *lic) {
    if (!lic) return;
    free(lic->raw);
    lic->raw = NULL;
    lic->raw_len = 0;
}

static int st_parse_license_blob(
    const unsigned char *blob,
    size_t len,
    StLicense *out,
    char *err,
    size_t err_cap
) {
    memset(out, 0, sizeof(*out));
    if (!blob || len == 0 || len > ST_MAX_KEY_BYTES) {
        snprintf(err, err_cap, "license size (%zu bytes) must be 1..%u", len, ST_MAX_KEY_BYTES);
        return 0;
    }
    if (memchr(blob, '\0', len) != NULL) {
        snprintf(err, err_cap, "license file contains NUL bytes");
        return 0;
    }
    size_t lead = 0;
    while (lead < len && (blob[lead] == ' ' || blob[lead] == '\t' || blob[lead] == '\r' || blob[lead] == '\n')) lead++;
    if (lead < len && blob[lead] == '{' && st_bytes_contains(blob, len, "OPENSPARSE-DEMO-NOT-A-LICENSE-1")) {
        snprintf(err, err_cap, "demo artifact (OPENSPARSE-DEMO-NOT-A-LICENSE-1) cannot be used for activation");
        return 0;
    }

    const char *text = (const char *)blob;
    const char *line_start[3];
    size_t line_len[3];
    size_t pos = 0;
    for (int li = 0; li < 3; li++) {
        if (pos >= len) {
            snprintf(err, err_cap, "invalid license format (expected 3 lines under PGL-LICENSE-1)");
            return 0;
        }
        size_t s = pos;
        while (pos < len && text[pos] != '\n') pos++;
        size_t e = pos;
        if (e > s && text[e - 1] == '\r') e--;
        line_start[li] = text + s;
        line_len[li] = e - s;
        if (pos < len && text[pos] == '\n') pos++;
    }
    if (pos != len) {
        snprintf(err, err_cap, "invalid license format (unexpected trailing data after 3rd line)");
        return 0;
    }
    if (line_len[0] != 13u || memcmp(line_start[0], "PGL-LICENSE-1", 13u) != 0) {
        snprintf(err, err_cap, "invalid license header (expected PGL-LICENSE-1)");
        return 0;
    }

    unsigned char sig[64];
    size_t sig_len = 0;
    if (!st_b64_decode(line_start[2], line_len[2], sig, sizeof(sig), &sig_len) || sig_len != 64u) {
        snprintf(err, err_cap, "invalid license signature encoding (expected 64-byte base64 signature)");
        return 0;
    }

    size_t payload_cap = (line_len[1] / 4u + 1u) * 3u + 1u;
    unsigned char *payload = (unsigned char *)malloc(payload_cap);
    if (!payload) {
        snprintf(err, err_cap, "out of memory");
        return 0;
    }
    size_t payload_len = 0;
    if (!st_b64_decode(line_start[1], line_len[1], payload, payload_cap - 1u, &payload_len) || payload_len == 0) {
        free(payload);
        snprintf(err, err_cap, "invalid license payload base64 encoding");
        return 0;
    }
    if (memchr(payload, '\0', payload_len) != NULL) {
        free(payload);
        snprintf(err, err_cap, "license payload contains NUL bytes");
        return 0;
    }
    payload[payload_len] = '\0';

    int has_lid = 0, has_prod = 0, has_dev = 0, has_na = 0;
    size_t p = 0;
    while (p < payload_len) {
        size_t ls = p;
        while (p < payload_len && payload[p] != '\n') p++;
        size_t le = p;
        if (le > ls && payload[le - 1] == '\r') le--;
        if (p < payload_len && payload[p] == '\n') p++;
        if (le == ls) continue;

        const char *line = (const char *)(payload + ls);
        size_t llen = le - ls;
        const char *eq = (const char *)memchr(line, '=', llen);
        if (!eq) {
            free(payload);
            snprintf(err, err_cap, "malformed payload line (missing '=')");
            return 0;
        }
        size_t klen = (size_t)(eq - line);
        const char *val = eq + 1;
        size_t vlen = llen - klen - 1u;
        for (size_t i = 0; i < vlen; i++) {
            unsigned char vc = (unsigned char)val[i];
            if (vc < 32 || vc >= 127) {
                free(payload);
                snprintf(err, err_cap, "non-ASCII character in payload field");
                return 0;
            }
        }

        if (klen == 10 && !memcmp(line, "license_id", 10)) {
            if (has_lid || !st_valid_license_id(val, vlen)) {
                free(payload);
                snprintf(err, err_cap, "invalid or duplicate license_id");
                return 0;
            }
            memcpy(out->license_id, val, vlen);
            out->license_id[vlen] = '\0';
            has_lid = 1;
        } else if (klen == 7 && !memcmp(line, "product", 7)) {
            if (has_prod || vlen >= sizeof(out->product)) {
                free(payload);
                snprintf(err, err_cap, "invalid or duplicate product");
                return 0;
            }
            char pbuf[32];
            memcpy(pbuf, val, vlen);
            pbuf[vlen] = '\0';
            const StProductInfo *info = st_find_product(pbuf, 0);
            if (!info) {
                free(payload);
                snprintf(err, err_cap, "unsupported product '%s'", pbuf);
                return 0;
            }
            snprintf(out->product, sizeof(out->product), "%s", info->product);
            snprintf(out->lib_name, sizeof(out->lib_name), "%s", info->lib_name);
            has_prod = 1;
        } else if (klen == 10 && !memcmp(line, "gpu_serial", 10)) {
            if (has_dev || !st_valid_device_id(val, vlen)) {
                free(payload);
                snprintf(err, err_cap, "invalid or duplicate gpu_serial");
                return 0;
            }
            memcpy(out->gpu_serial, val, vlen);
            out->gpu_serial[vlen] = '\0';
            has_dev = 1;
        } else if (klen == 9 && !memcmp(line, "not_after", 9)) {
            if (has_na || !st_parse_u64(val, vlen, &out->not_after)) {
                free(payload);
                snprintf(err, err_cap, "invalid or duplicate not_after");
                return 0;
            }
            has_na = 1;
        } else if (klen == 9 && !memcmp(line, "issued_at", 9)) {
            if (!st_parse_u64(val, vlen, &out->issued_at)) {
                free(payload);
                snprintf(err, err_cap, "invalid issued_at");
                return 0;
            }
            out->has_issued_at = 1;
        } else if (klen == 10 && !memcmp(line, "not_before", 10)) {
            if (!st_parse_u64(val, vlen, &out->not_before)) {
                free(payload);
                snprintf(err, err_cap, "invalid not_before");
                return 0;
            }
            out->has_not_before = 1;
        } else if (klen == 8 && !memcmp(line, "order_id", 8)) {
            if (st_valid_safe_token(val, vlen, sizeof(out->order_id) - 1u)) {
                memcpy(out->order_id, val, vlen);
                out->order_id[vlen] = '\0';
            }
        } else if (klen == 4 && !memcmp(line, "term", 4)) {
            if (st_valid_safe_token(val, vlen, sizeof(out->term) - 1u)) {
                memcpy(out->term, val, vlen);
                out->term[vlen] = '\0';
            }
        }
    }
    free(payload);

    if (!has_lid || !has_prod || !has_dev || !has_na) {
        snprintf(err, err_cap, "license payload missing required fields (license_id, product, gpu_serial, not_after)");
        return 0;
    }

    out->raw = (unsigned char *)malloc(len);
    if (!out->raw) {
        snprintf(err, err_cap, "out of memory");
        return 0;
    }
    memcpy(out->raw, blob, len);
    out->raw_len = len;
    st_sha256_hex(blob, len, out->sha256);
    return 1;
}

static int st_append_license_blob(
    const unsigned char *blob,
    size_t blob_len,
    const char *src_label,
    StLicense **items,
    size_t *item_count,
    size_t *item_cap,
    char *err,
    size_t err_cap
) {
    if (*item_count >= *item_cap) {
        if (*item_cap >= ST_MAX_LICENSE_BATCH) {
            snprintf(err, err_cap, "too many license keys in batch (limit %u)", ST_MAX_LICENSE_BATCH);
            return 0;
        }
        size_t new_cap = (*item_cap == 0) ? 16u : (*item_cap * 2u);
        if (new_cap > ST_MAX_LICENSE_BATCH) new_cap = ST_MAX_LICENSE_BATCH;
        StLicense *tmp = (StLicense *)realloc(*items, new_cap * sizeof(StLicense));
        if (!tmp) {
            snprintf(err, err_cap, "out of memory");
            return 0;
        }
        *items = tmp;
        *item_cap = new_cap;
    }
    char suberr[256];
    StLicense lic;
    if (!st_parse_license_blob(blob, blob_len, &lic, suberr, sizeof(suberr))) {
        snprintf(err, err_cap, "%s: %s", src_label, suberr);
        return 0;
    }
    (*items)[(*item_count)++] = lic;
    return 1;
}

static int st_read_regular_file(
    const char *path,
    size_t max_bytes,
    unsigned char **out_buf,
    size_t *out_len,
    char *err,
    size_t err_cap
) {
    *out_buf = NULL;
    *out_len = 0;
#ifdef _WIN32
    DWORD attrs = GetFileAttributesA(path);
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        snprintf(err, err_cap, "%.160s: file not found", path);
        return 0;
    }
    if ((attrs & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
        snprintf(err, err_cap, "%.160s: must be a regular non-symlink file", path);
        return 0;
    }
    int fd = _open(path, _O_RDONLY | _O_BINARY);
    if (fd < 0) {
        snprintf(err, err_cap, "%.160s: %.80s", path, strerror(errno));
        return 0;
    }
    struct _stat st;
    if (_fstat(fd, &st) != 0 || (st.st_mode & _S_IFREG) == 0) {
        _close(fd);
        snprintf(err, err_cap, "%.160s: must be a regular file", path);
        return 0;
    }
#else
    struct stat lst;
    if (lstat(path, &lst) != 0) {
        snprintf(err, err_cap, "%.160s: %.80s", path, strerror(errno));
        return 0;
    }
    if (S_ISLNK(lst.st_mode) || !S_ISREG(lst.st_mode)) {
        snprintf(err, err_cap, "%.160s: must be a regular non-symlink file", path);
        return 0;
    }
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) {
        snprintf(err, err_cap, "%.160s: %.80s", path, strerror(errno));
        return 0;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        snprintf(err, err_cap, "%.160s: must be a regular file", path);
        return 0;
    }
#endif
    if (st.st_size <= 0 || (uint64_t)st.st_size > (uint64_t)max_bytes) {
#ifdef _WIN32
        _close(fd);
#else
        close(fd);
#endif
        snprintf(err, err_cap, "%.140s: size (%lld bytes) must be 1..%zu", path, (long long)st.st_size, max_bytes);
        return 0;
    }
    size_t target = (size_t)st.st_size;
    unsigned char *buf = (unsigned char *)malloc(target + 1u);
    if (!buf) {
#ifdef _WIN32
        _close(fd);
#else
        close(fd);
#endif
        snprintf(err, err_cap, "out of memory");
        return 0;
    }
    size_t got = 0;
    while (got < target) {
#ifdef _WIN32
        unsigned int chunk = (target - got > 65536u) ? 65536u : (unsigned int)(target - got);
        int r = _read(fd, buf + got, chunk);
#else
        ssize_t r = read(fd, buf + got, target - got);
#endif
        if (r < 0) {
#ifndef _WIN32
            if (errno == EINTR) continue;
#endif
            free(buf);
#ifdef _WIN32
            _close(fd);
#else
            close(fd);
#endif
            snprintf(err, err_cap, "%.150s: read error: %.80s", path, strerror(errno));
            return 0;
        }
        if (r == 0) break;
        got += (size_t)r;
    }
#ifdef _WIN32
    _close(fd);
#else
    close(fd);
#endif
    if (got != target) {
        free(buf);
        snprintf(err, err_cap, "%.160s: short read (file modified during read)", path);
        return 0;
    }
    buf[got] = '\0';
    *out_buf = buf;
    *out_len = got;
    return 1;
}

static int st_load_code_text(
    const char *text,
    size_t text_len,
    const char *src_label,
    StLicense **items,
    size_t *item_count,
    size_t *item_cap,
    char *err,
    size_t err_cap
) {
    char suberr[256];
    StLic2KeyBlob *blobs = NULL;
    size_t blob_count = 0u;
    if (!st_lic2_decode(text, text_len, &blobs, &blob_count, suberr, sizeof(suberr))) {
        snprintf(err, err_cap, "%s: %s", src_label, suberr);
        return 0;
    }
    for (size_t i = 0; i < blob_count; i++) {
        char label[128];
        snprintf(label, sizeof(label), "%s (entry %zu)", src_label, i + 1u);
        if (!st_append_license_blob(blobs[i].data, blobs[i].len, label,
                                    items, item_count, item_cap, err, err_cap)) {
            st_lic2_free_blobs(blobs, blob_count);
            return 0;
        }
    }
    st_lic2_free_blobs(blobs, blob_count);
    return 1;
}

#ifndef _WIN32
static volatile sig_atomic_t st_paste_sigint = 0;
static void st_on_paste_sigint(int sig) {
    (void)sig;
    st_paste_sigint = 1;
}
#endif

static int st_buf_push(char **buf, size_t *len, size_t *cap, char ch, int *fail_code) {
    if (*len >= ST_LIC2_MAX_TEXT) { *fail_code = 1; return 0; }
    if (*len >= *cap) {
        size_t ncap = (*cap * 2u > ST_LIC2_MAX_TEXT) ? ST_LIC2_MAX_TEXT : (*cap * 2u);
        char *tmp = (char *)realloc(*buf, ncap + 1u);
        if (!tmp) { *fail_code = 2; return 0; }
        *buf = tmp;
        *cap = ncap;
    }
    (*buf)[(*len)++] = ch;
    return 1;
}

static int st_read_paste_stdin(char **out_text, size_t *out_len, char *err, size_t err_cap) {
    *out_text = NULL;
    *out_len = 0u;

    size_t cap = 4096u;
    size_t len = 0u;
    char *buf = (char *)malloc(cap + 1u);
    if (!buf) {
        snprintf(err, err_cap, "out of memory");
        return 0;
    }

    int fail_code = 0; /* 1=max_bytes, 2=oom, 3=nul, 4=io */
    int tty_prompted = 0;
#ifdef _WIN32
    if (_isatty(_fileno(stdin))) {
        tty_prompted = 1;
        fputs("Paste license code: ", stderr);
        fflush(stderr);
        int line_has_nonws = 0, ch;
        while ((ch = _getch()) != EOF) {
            unsigned char c = (unsigned char)ch;
            if (c == 0x03u) { fail_code = 5; break; }
            if (c == 0x04u || c == 0x1au) break;
            if (c == '\0') { fail_code = 3; break; }
            if (c == 0x08u || c == 0x7fu) {
                if (len > 0 && buf[len - 1] != '\n') {
                    unsigned char del = (unsigned char)buf[--len];
                    if (del >= 0x21u && del <= 0x7eu) {
                        fputs("\b \b", stderr);
                        fflush(stderr);
                    }
                    line_has_nonws = 0;
                    for (size_t k = len; k > 0 && buf[k - 1] != '\n'; k--) {
                        if (buf[k - 1] != ' ' && buf[k - 1] != '\t') { line_has_nonws = 1; break; }
                    }
                }
                continue;
            }
            if (c == '\r' || c == '\n') {
                if (!st_buf_push(&buf, &len, &cap, '\n', &fail_code)) break;
                if (!line_has_nonws) break;
                line_has_nonws = 0;
                if (len > ST_LIC3_PREFIX_LEN && st_lic2_has_valid_crc(buf, len)) break;
                continue;
            }
            if (c >= 0x21u && c <= 0x7eu) {
                line_has_nonws = 1;
                if (!st_buf_push(&buf, &len, &cap, (char)c, &fail_code)) break;
                fputc('*', stderr);
                fflush(stderr);
            } else if (c == ' ' || c == '\t') {
                if (!st_buf_push(&buf, &len, &cap, (char)c, &fail_code)) break;
            }
        }
    } else
#else
    if (isatty(STDIN_FILENO)) {
        tty_prompted = 1;
        fputs("Paste license code: ", stderr);
        fflush(stderr);

        struct termios orig_tio;
        int have_tio = (tcgetattr(STDIN_FILENO, &orig_tio) == 0);
        if (have_tio) {
            struct termios raw = orig_tio;
            raw.c_lflag &= ~(unsigned)(ICANON | ECHO | ISIG);
            raw.c_cc[VMIN] = 1;
            raw.c_cc[VTIME] = 0;
            (void)tcsetattr(STDIN_FILENO, TCSANOW, &raw);
        }
        struct sigaction sa, old_sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = st_on_paste_sigint;
        sigemptyset(&sa.sa_mask);
        st_paste_sigint = 0;
        int have_sa = (sigaction(SIGINT, &sa, &old_sa) == 0);

        int line_has_nonws = 0, prev_cr = 0, done = 0;
        unsigned char chunk[1024];
        while (!done && !st_paste_sigint) {
            ssize_t r = read(STDIN_FILENO, chunk, sizeof(chunk));
            if (r < 0) {
                if (errno == EINTR) continue;
                fail_code = 4;
                break;
            }
            if (r == 0) break;
            int saw_nl = 0;
            char echo_buf[1024];
            size_t echo_n = 0;
            for (ssize_t i = 0; i < r; i++) {
                unsigned char c = chunk[i];
                if (c == 0x03u) { st_paste_sigint = 1; done = 1; break; }
                if (c == 0x04u) { done = 1; break; }
                if (c == '\0') { fail_code = 3; done = 1; break; }
                if (c == '\n' && prev_cr) { prev_cr = 0; continue; }
                prev_cr = (c == '\r');
                if (c == 0x7fu || c == 0x08u) {
                    if (len > 0 && buf[len - 1] != '\n') {
                        unsigned char del = (unsigned char)buf[--len];
                        if (del >= 0x21u && del <= 0x7eu) {
                            if (echo_n + 3u > sizeof(echo_buf)) {
                                (void)fwrite(echo_buf, 1u, echo_n, stderr);
                                echo_n = 0;
                            }
                            echo_buf[echo_n++] = '\b';
                            echo_buf[echo_n++] = ' ';
                            echo_buf[echo_n++] = '\b';
                        }
                        line_has_nonws = 0;
                        for (size_t k = len; k > 0 && buf[k - 1] != '\n'; k--) {
                            if (buf[k - 1] != ' ' && buf[k - 1] != '\t') { line_has_nonws = 1; break; }
                        }
                    }
                    continue;
                }
                if (c == '\r' || c == '\n') {
                    if (!st_buf_push(&buf, &len, &cap, '\n', &fail_code)) { done = 1; break; }
                    if (!line_has_nonws) { done = 1; break; }
                    line_has_nonws = 0;
                    saw_nl = 1;
                    continue;
                }
                if (c == ' ' || c == '\t') {
                    if (!st_buf_push(&buf, &len, &cap, (char)c, &fail_code)) { done = 1; break; }
                    continue;
                }
                if (c >= 0x21u && c <= 0x7eu) {
                    line_has_nonws = 1;
                    if (!st_buf_push(&buf, &len, &cap, (char)c, &fail_code)) { done = 1; break; }
                    if (echo_n + 1u > sizeof(echo_buf)) {
                        (void)fwrite(echo_buf, 1u, echo_n, stderr);
                        echo_n = 0;
                    }
                    echo_buf[echo_n++] = '*';
                }
            }
            if (echo_n > 0) {
                (void)fwrite(echo_buf, 1u, echo_n, stderr);
                fflush(stderr);
            }
            if (!done && !fail_code && !st_paste_sigint) {
                int check_now = saw_nl;
                if (!check_now) {
                    fd_set rfds;
                    struct timeval tv;
                    FD_ZERO(&rfds);
                    FD_SET(STDIN_FILENO, &rfds);
                    tv.tv_sec = 0;
                    tv.tv_usec = 0;
                    if (select(STDIN_FILENO + 1, &rfds, NULL, NULL, &tv) == 0) check_now = 1;
                }
                if (check_now && len > ST_LIC3_PREFIX_LEN && st_lic2_has_valid_crc(buf, len)) {
                    done = 1;
                }
            }
        }

        if (have_tio) (void)tcsetattr(STDIN_FILENO, TCSANOW, &orig_tio);
        if (have_sa) (void)sigaction(SIGINT, &old_sa, NULL);
        if (st_paste_sigint) {
            fputc('\n', stderr);
            fflush(stderr);
            free(buf);
            snprintf(err, err_cap, "interrupted");
            return 0;
        }
    } else
#endif
    {
        while (1) {
            if (len >= cap) {
                if (cap >= ST_LIC2_MAX_TEXT) {
                    unsigned char extra;
                    if (fread(&extra, 1u, 1u, stdin) == 1u) { fail_code = 1; }
                    break;
                }
                size_t ncap = (cap * 2u > ST_LIC2_MAX_TEXT) ? ST_LIC2_MAX_TEXT : (cap * 2u);
                char *tmp = (char *)realloc(buf, ncap + 1u);
                if (!tmp) { fail_code = 2; break; }
                buf = tmp;
                cap = ncap;
            }
            size_t got = fread(buf + len, 1u, cap - len, stdin);
            len += got;
            if (got == 0u) {
                if (ferror(stdin)) fail_code = 4;
                break;
            }
        }
    }

    if (tty_prompted) {
        fputc('\n', stderr);
        fflush(stderr);
    }

    if (fail_code) {
        free(buf);
        const char *msg = (fail_code == 1) ? "input exceeds %u bytes"
                        : (fail_code == 2) ? "out of memory"
                        : (fail_code == 3) ? "input contains NUL bytes"
                        : (fail_code == 5) ? "interrupted"
                                           : "stdin read error";
        if (fail_code == 1) snprintf(err, err_cap, msg, ST_LIC2_MAX_TEXT);
        else snprintf(err, err_cap, "%s", msg);
        return 0;
    }

    size_t has_nonws = 0u;
    for (size_t i = 0; i < len; i++) {
        if (buf[i] == '\0') {
            free(buf);
            snprintf(err, err_cap, "input contains NUL bytes");
            return 0;
        }
        if (buf[i] != ' ' && buf[i] != '\t' && buf[i] != '\r' && buf[i] != '\n') has_nonws = 1u;
    }
    if (!has_nonws) {
        free(buf);
        snprintf(err, err_cap, "no license code received");
        return 0;
    }
    buf[len] = '\0';
    *out_text = buf;
    *out_len = len;
    return 1;
}

static void st_build_key_filename(const char *product, const char *gpu_serial, char *out, size_t out_cap) {
    static const char suffix[] = ".license.key";
    size_t suflen = sizeof(suffix) - 1u;
    size_t plen = strlen(product);
    size_t glen = strlen(gpu_serial);
    int all_simple = (glen > 0 && glen <= 80u);
    if (all_simple) {
        for (size_t i = 0; i < glen; i++) {
            unsigned char c = (unsigned char)gpu_serial[i];
            if (!isalnum(c) && c != '_' && c != '-') {
                all_simple = 0;
                break;
            }
        }
    }
    if (all_simple && plen + 1u + glen + suflen + 1u <= out_cap) {
        size_t pos = 0;
        memcpy(out + pos, product, plen); pos += plen;
        out[pos++] = '-';
        memcpy(out + pos, gpu_serial, glen); pos += glen;
        memcpy(out + pos, suffix, suflen + 1u);
        return;
    }
    char san[65];
    size_t slen = glen < 64u ? glen : 64u;
    for (size_t i = 0; i < slen; i++) {
        unsigned char c = (unsigned char)gpu_serial[i];
        san[i] = (isalnum(c) || c == '_' || c == '-') ? (char)c : '_';
    }
    san[slen] = '\0';
    char hex[65];
    st_sha256_hex((const unsigned char *)gpu_serial, glen, hex);
    if (plen + 1u + slen + 1u + 8u + suflen + 1u <= out_cap) {
        size_t pos = 0;
        memcpy(out + pos, product, plen); pos += plen;
        out[pos++] = '-';
        memcpy(out + pos, san, slen); pos += slen;
        out[pos++] = '-';
        memcpy(out + pos, hex, 8u); pos += 8u;
        memcpy(out + pos, suffix, suflen + 1u);
    } else if (out_cap > 0) {
        out[0] = '\0';
    }
}

static const char *st_home_dir(void) {
#ifdef _WIN32
    const char *home = getenv("USERPROFILE");
    if (home && *home) return home;
    return NULL;
#else
    return getenv("HOME");
#endif
}

static int st_resolve_base_dir(
    StScope scope,
    char *out,
    size_t cap,
    char *err,
    size_t err_cap
) {
    if (scope == ST_SCOPE_SYSTEM) {
#ifdef _WIN32
        const char *pd = getenv("ProgramData");
        if (!pd || !*pd) pd = "C:\\ProgramData";
        if (!st_path_join(out, cap, pd, "solved-tech")) {
            snprintf(err, err_cap, "system directory path too long");
            return 0;
        }
#else
        if (strlen("/etc/solved-tech") + 1u > cap) return 0;
        snprintf(out, cap, "/etc/solved-tech");
#endif
        return 1;
    }
#ifdef _WIN32
    const char *home = getenv("USERPROFILE");
    char win_home[ST_PATH_CAP];
    if (!home || !*home) {
        const char *hd = getenv("HOMEDRIVE"), *hp = getenv("HOMEPATH");
        if (hd && hp && strlen(hd) + strlen(hp) < sizeof(win_home)) {
            snprintf(win_home, sizeof(win_home), "%s%s", hd, hp);
            home = win_home;
        }
    }
#else
    const char *home = getenv("HOME");
#endif
    if (!home || !*home) {
        snprintf(err, err_cap, "HOME is not set; pass --system");
        return 0;
    }
    if (!st_path_join(out, cap, home, ".solved-tech")) {
        snprintf(err, err_cap, "user license root path too long");
        return 0;
    }
    return 1;
}

static int st_check_real_dir(const char *path, int *exists_out, char *err, size_t err_cap) {
    *exists_out = 0;
#ifdef _WIN32
    DWORD attrs = GetFileAttributesA(path);
    if (attrs == INVALID_FILE_ATTRIBUTES) return 1;
    if ((attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0 || (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        snprintf(err, err_cap, "%s: exists and is not a real directory", path);
        return 0;
    }
    *exists_out = 1;
    return 1;
#else
    struct stat st;
    if (lstat(path, &st) != 0) {
        if (errno == ENOENT) return 1;
        snprintf(err, err_cap, "%s: %s", path, strerror(errno));
        return 0;
    }
    if (S_ISLNK(st.st_mode) || !S_ISDIR(st.st_mode)) {
        snprintf(err, err_cap, "%s: exists and is not a real directory", path);
        return 0;
    }
    *exists_out = 1;
    return 1;
#endif
}

static int st_mkdir_single(const char *path, int dir_mode, char *err, size_t err_cap) {
    int exists = 0;
    if (!st_check_real_dir(path, &exists, err, err_cap)) return 0;
    if (exists) return 1;
#ifdef _WIN32
    (void)dir_mode;
    if (_mkdir(path) != 0 && errno != EEXIST) {
        snprintf(err, err_cap, "cannot create directory %s: %s", path, strerror(errno));
        return 0;
    }
#else
    if (mkdir(path, (mode_t)dir_mode) != 0 && errno != EEXIST) {
        snprintf(err, err_cap, "cannot create directory %s: %s", path, strerror(errno));
        return 0;
    }
    if (chmod(path, (mode_t)dir_mode) != 0) {
        snprintf(err, err_cap, "cannot set permissions on %s: %s", path, strerror(errno));
        return 0;
    }
#endif
    if (!st_check_real_dir(path, &exists, err, err_cap) || !exists) {
        if (!err[0]) snprintf(err, err_cap, "%s: directory verification failed", path);
        return 0;
    }
    return 1;
}

static int st_ensure_dirs(const char *base_dir, const char *product_dir, StScope scope, char *err, size_t err_cap) {
    int mode = (scope == ST_SCOPE_SYSTEM) ? 0755 : 0700;
    if (scope == ST_SCOPE_SYSTEM) {
#ifndef _WIN32
        if (!st_mkdir_single("/etc", 0755, err, err_cap)) return 0;
#endif
    }
    /* Create parent components if base_dir is custom nested path or ~/.solved-tech */
    char partial[ST_PATH_CAP];
    size_t blen = strlen(base_dir);
    if (blen >= sizeof(partial)) {
        snprintf(err, err_cap, "directory path too long");
        return 0;
    }
    memcpy(partial, base_dir, blen + 1u);
    for (size_t i = 1; i < blen; i++) {
        if (partial[i] == '/' || partial[i] == '\\') {
#ifdef _WIN32
            if (i == 2 && partial[1] == ':') continue;
#endif
            char saved = partial[i];
            partial[i] = '\0';
            int exists = 0;
#ifdef _WIN32
            DWORD attrs = GetFileAttributesA(partial);
            exists = (attrs != INVALID_FILE_ATTRIBUTES);
            if (!exists && _mkdir(partial) != 0 && errno != EEXIST) {
                snprintf(err, err_cap, "cannot create directory %s: %s", partial, strerror(errno));
                return 0;
            }
#else
            struct stat st;
            exists = (stat(partial, &st) == 0);
            if (!exists && mkdir(partial, (mode_t)mode) != 0 && errno != EEXIST) {
                snprintf(err, err_cap, "cannot create directory %s: %s", partial, strerror(errno));
                return 0;
            }
#endif
            partial[i] = saved;
        }
    }
    if (!st_mkdir_single(base_dir, mode, err, err_cap)) return 0;
    if (!st_mkdir_single(product_dir, mode, err, err_cap)) return 0;
    return 1;
}

static int st_atomic_write_file(
    const char *dir,
    const char *target_name,
    const unsigned char *data,
    size_t len,
    int file_mode,
    char *err,
    size_t err_cap
) {
    char dest[ST_PATH_CAP];
    if (!st_path_join(dest, sizeof(dest), dir, target_name)) {
        snprintf(err, err_cap, "destination path too long");
        return 0;
    }
#ifndef _WIN32
    struct stat dest_st;
    if (lstat(dest, &dest_st) == 0) {
        if (S_ISLNK(dest_st.st_mode) || !S_ISREG(dest_st.st_mode)) {
            snprintf(err, err_cap, "%.300s: destination exists and is not a regular file", dest);
            return 0;
        }
    }
    unsigned long pid = (unsigned long)getpid();
#else
    DWORD attrs = GetFileAttributesA(dest);
    if (attrs != INVALID_FILE_ATTRIBUTES &&
        ((attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0 || (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0)) {
        snprintf(err, err_cap, "%.300s: destination exists and is not a regular file", dest);
        return 0;
    }
    unsigned long pid = (unsigned long)GetCurrentProcessId();
#endif

    char tmp_name[128], tmp_path[ST_PATH_CAP];
    int fd = -1;
    for (int attempt = 0; attempt < 16; attempt++) {
        snprintf(tmp_name, sizeof(tmp_name), ".stcli-tmp-%lu-%d.tmp", pid, attempt);
        if (!st_path_join(tmp_path, sizeof(tmp_path), dir, tmp_name)) {
            snprintf(err, err_cap, "temp path too long");
            return 0;
        }
#ifdef _WIN32
        (void)file_mode;
        fd = _open(tmp_path, _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY, _S_IREAD | _S_IWRITE);
#else
        fd = open(tmp_path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, (mode_t)file_mode);
#endif
        if (fd >= 0) break;
        if (errno != EEXIST) break;
    }
    if (fd < 0) {
        snprintf(err, err_cap, "cannot create temp file in %.300s: %s", dir, strerror(errno));
        return 0;
    }

    size_t written = 0;
    while (written < len) {
#ifdef _WIN32
        unsigned int chunk = (len - written > 65536u) ? 65536u : (unsigned int)(len - written);
        int w = _write(fd, data + written, chunk);
#else
        ssize_t w = write(fd, data + written, len - written);
#endif
        if (w <= 0) {
#ifndef _WIN32
            if (w < 0 && errno == EINTR) continue;
#endif
            int saved = errno;
#ifdef _WIN32
            _close(fd);
            DeleteFileA(tmp_path);
#else
            close(fd);
            unlink(tmp_path);
#endif
            snprintf(err, err_cap, "write failed for %.300s: %s", dest, strerror(saved));
            return 0;
        }
        written += (size_t)w;
    }

#ifdef _WIN32
    _commit(fd);
    if (_close(fd) != 0) {
        int saved = errno;
        DeleteFileA(tmp_path);
        snprintf(err, err_cap, "close failed for %.300s: %s", dest, strerror(saved));
        return 0;
    }
    if (!MoveFileExA(tmp_path, dest, MOVEFILE_REPLACE_EXISTING)) {
        DWORD le = GetLastError();
        DeleteFileA(tmp_path);
        snprintf(err, err_cap, "replace failed for %.300s (winerr %lu)", dest, (unsigned long)le);
        return 0;
    }
#else
    if (chmod(tmp_path, (mode_t)file_mode) != 0 || fsync(fd) != 0) {
        int saved = errno;
        close(fd);
        unlink(tmp_path);
        snprintf(err, err_cap, "sync/chmod failed for %.300s: %s", dest, strerror(saved));
        return 0;
    }
    if (close(fd) != 0) {
        int saved = errno;
        unlink(tmp_path);
        snprintf(err, err_cap, "close failed for %.300s: %s", dest, strerror(saved));
        return 0;
    }
    if (rename(tmp_path, dest) != 0) {
        int saved = errno;
        unlink(tmp_path);
        snprintf(err, err_cap, "rename failed for %.300s: %s", dest, strerror(saved));
        return 0;
    }
#endif
    return 1;
}

typedef struct {
    char name[256];
    char full_path[ST_PATH_CAP];
    int parsed;
    StLicense lic;
    char error[256];
} StDirKeyEntry;

static int st_cmp_dir_entry(const void *a, const void *b) {
    const StDirKeyEntry *ea = (const StDirKeyEntry *)a;
    const StDirKeyEntry *eb = (const StDirKeyEntry *)b;
    return strcmp(ea->name, eb->name);
}

static int st_scan_product_dir(
    const char *dir,
    StDirKeyEntry **out_entries,
    size_t *out_count,
    char *err,
    size_t err_cap
) {
    *out_entries = NULL;
    *out_count = 0;
    int exists = 0;
    if (!st_check_real_dir(dir, &exists, err, err_cap)) return 0;
    if (!exists) return 1;

    StDirKeyEntry *list = NULL;
    size_t count = 0, cap = 0;

#ifdef _WIN32
    char pattern[ST_PATH_CAP];
    if (!st_path_join(pattern, sizeof(pattern), dir, "*.key")) {
        snprintf(err, err_cap, "directory path too long");
        return 0;
    }
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD le = GetLastError();
        if (le == ERROR_FILE_NOT_FOUND || le == ERROR_PATH_NOT_FOUND) return 1;
        snprintf(err, err_cap, "%s: scan failed (winerr %lu)", dir, (unsigned long)le);
        return 0;
    }
    do {
        if ((fd.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) continue;
        if (fd.cFileName[0] == '.') continue;
        if (!st_has_suffix(fd.cFileName, ".key")) continue;
        char full[ST_PATH_CAP];
        if (!st_path_join(full, sizeof(full), dir, fd.cFileName)) continue;
#else
    DIR *dp = opendir(dir);
    if (!dp) {
        snprintf(err, err_cap, "%s: %s", dir, strerror(errno));
        return 0;
    }
    struct dirent *de;
    while ((de = readdir(dp)) != NULL) {
        if (de->d_name[0] == '.') continue;
        if (!st_has_suffix(de->d_name, ".key")) continue;
        char full[ST_PATH_CAP];
        if (!st_path_join(full, sizeof(full), dir, de->d_name)) continue;
        struct stat st;
        if (lstat(full, &st) != 0 || S_ISLNK(st.st_mode) || !S_ISREG(st.st_mode)) continue;
#endif
        if (count >= cap) {
            size_t ncap = (cap == 0) ? 16u : (cap * 2u);
            StDirKeyEntry *tmp = (StDirKeyEntry *)realloc(list, ncap * sizeof(StDirKeyEntry));
            if (!tmp) {
#ifdef _WIN32
                FindClose(h);
#else
                closedir(dp);
#endif
                for (size_t i = 0; i < count; i++) st_license_free(&list[i].lic);
                free(list);
                snprintf(err, err_cap, "out of memory");
                return 0;
            }
            list = tmp;
            cap = ncap;
        }
        StDirKeyEntry *ent = &list[count];
        memset(ent, 0, sizeof(*ent));
#ifdef _WIN32
        snprintf(ent->name, sizeof(ent->name), "%s", fd.cFileName);
#else
        snprintf(ent->name, sizeof(ent->name), "%s", de->d_name);
#endif
        snprintf(ent->full_path, sizeof(ent->full_path), "%s", full);
        unsigned char *blob = NULL;
        size_t blen = 0;
        char read_err[256];
        if (!st_read_regular_file(full, ST_MAX_KEY_BYTES, &blob, &blen, read_err, sizeof(read_err))) {
            snprintf(ent->error, sizeof(ent->error), "%s", read_err);
        } else {
            ent->parsed = st_parse_license_blob(blob, blen, &ent->lic, ent->error, sizeof(ent->error));
            free(blob);
        }
        count++;
#ifdef _WIN32
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    }
    closedir(dp);
#endif

    if (count > 1u) qsort(list, count, sizeof(StDirKeyEntry), st_cmp_dir_entry);
    *out_entries = list;
    *out_count = count;
    return 1;
}

static void st_free_dir_entries(StDirKeyEntry *entries, size_t count) {
    if (!entries) return;
    for (size_t i = 0; i < count; i++) st_license_free(&entries[i].lic);
    free(entries);
}

static int st_to_utc_tm(uint64_t ts, struct tm *out_tm) {
    time_t t = (time_t)ts;
    if (t < 0 || (uint64_t)t != ts) return 0;
#ifdef _WIN32
    struct tm *tmp = gmtime(&t);
    if (!tmp) return 0;
    *out_tm = *tmp;
    return 1;
#else
    return gmtime_r(&t, out_tm) != NULL;
#endif
}

static void st_format_utc(uint64_t ts, char *out, size_t cap) {
    if (ts == 0) {
        snprintf(out, cap, "perpetual");
        return;
    }
    struct tm tm_val;
    if (!st_to_utc_tm(ts, &tm_val)) {
        snprintf(out, cap, "%llu", (unsigned long long)ts);
        return;
    }
    snprintf(out, cap, "%04d-%02d-%02dT%02d:%02d:%02dZ",
             tm_val.tm_year + 1900, tm_val.tm_mon + 1, tm_val.tm_mday,
             tm_val.tm_hour, tm_val.tm_min, tm_val.tm_sec);
}

static void st_display_path(const char *path, char *out, size_t cap) {
    const char *home = st_home_dir();
    if (home && *home) {
        size_t hlen = strlen(home);
        while (hlen > 1 && (home[hlen - 1] == '/' || home[hlen - 1] == '\\')) hlen--;
        if (!strncmp(path, home, hlen) && (path[hlen] == '\0' || path[hlen] == '/' || path[hlen] == '\\')) {
            snprintf(out, cap, "~%s", path + hlen);
            return;
        }
    }
    snprintf(out, cap, "%s", path);
}

static void st_format_date_rel(
    uint64_t not_after,
    uint64_t now,
    char *date_buf,
    size_t date_cap,
    char *rel_buf,
    size_t rel_cap,
    int *expiring_soon
) {
    date_buf[0] = '\0';
    rel_buf[0] = '\0';
    if (expiring_soon) *expiring_soon = 0;
    if (not_after == 0) return;

    struct tm tm_val;
    int have_tm = st_to_utc_tm(not_after, &tm_val);
    if (now >= not_after) {
        uint64_t diff = now - not_after;
        if (have_tm && tm_val.tm_year >= -1900 && tm_val.tm_year <= 8099) {
            snprintf(date_buf, date_cap, "%04d-%02d-%02d",
                     tm_val.tm_year + 1900, tm_val.tm_mon + 1, tm_val.tm_mday);
        } else {
            snprintf(date_buf, date_cap, "%llu", (unsigned long long)not_after);
        }
        if (diff >= 48u * 3600u) snprintf(rel_buf, rel_cap, "%llud ago", (unsigned long long)(diff / 86400u));
        else if (diff >= 3600u) snprintf(rel_buf, rel_cap, "%lluh ago", (unsigned long long)(diff / 3600u));
        else if (diff >= 60u) snprintf(rel_buf, rel_cap, "%llum ago", (unsigned long long)(diff / 60u));
        else snprintf(rel_buf, rel_cap, "<1m ago");
    } else {
        uint64_t diff = not_after - now;
        if (expiring_soon) *expiring_soon = (diff <= 72u * 3600u);
        if (have_tm && tm_val.tm_year >= -1900 && tm_val.tm_year <= 8099) {
            if (diff < 48u * 3600u) {
                snprintf(date_buf, date_cap, "%04d-%02d-%02d %02d:%02d",
                         tm_val.tm_year + 1900, tm_val.tm_mon + 1, tm_val.tm_mday,
                         tm_val.tm_hour, tm_val.tm_min);
            } else {
                snprintf(date_buf, date_cap, "%04d-%02d-%02d",
                         tm_val.tm_year + 1900, tm_val.tm_mon + 1, tm_val.tm_mday);
            }
        } else {
            snprintf(date_buf, date_cap, "%llu", (unsigned long long)not_after);
        }
        if (diff >= 48u * 3600u) snprintf(rel_buf, rel_cap, "%llud", (unsigned long long)(diff / 86400u));
        else if (diff >= 3600u) snprintf(rel_buf, rel_cap, "%lluh", (unsigned long long)(diff / 3600u));
        else if (diff >= 60u) snprintf(rel_buf, rel_cap, "%llum", (unsigned long long)(diff / 60u));
        else snprintf(rel_buf, rel_cap, "<1m");
    }
}

static int st_cmp_listed_license(const void *pa, const void *pb) {
    const StListedLicense *a = (const StListedLicense *)pa;
    const StListedLicense *b = (const StListedLicense *)pb;
    int pi_a = st_find_product_index(a->product);
    int pi_b = st_find_product_index(b->product);
    if (pi_a != pi_b) return (pi_a < pi_b) ? -1 : 1;
    int inv_a = !strcmp(a->status, "invalid");
    int inv_b = !strcmp(b->status, "invalid");
    if (inv_a != inv_b) return inv_a - inv_b;
    int gs = strcmp(a->gpu_serial, b->gpu_serial);
    if (gs != 0) return gs;
    return strcmp(a->filename, b->filename);
}

static const char *st_scope_name(StScope s) {
    return (s == ST_SCOPE_SYSTEM) ? "system" : "user";
}

static void st_usage_apply(void) {
    int c = st_use_color();
    const char *b = c ? "\033[1m" : "", *d = c ? "\033[2m" : "", *r = c ? "\033[0m" : "";
    printf("%sUsage:%s stcli apply [--system] [--paste | <LICENSE_CODE>...]\n\n"
           "Install license codes into ~/.solved-tech/<LibName>/.\n\n"
           "%sOptions%s\n"
           "  --system   Install to /etc/solved-tech/<LibName>/ %s(default: ~/.solved-tech)%s\n"
           "  --paste    Read a license code from stdin (default when no code is passed)\n",
           b, r, b, r, d, r);
}

static void st_usage_list(void) {
    int c = st_use_color();
    const char *b = c ? "\033[1m" : "", *d = c ? "\033[2m" : "", *r = c ? "\033[0m" : "";
    printf("%sUsage:%s stcli list [--system] [--json]\n\n"
           "List installed licenses under ~/.solved-tech/<LibName>/.\n\n"
           "%sOptions%s\n"
           "  --system   Read from /etc/solved-tech/<LibName>/ %s(default: ~/.solved-tech)%s\n"
           "  --json     Output machine-readable JSON\n",
           b, r, b, r, d, r);
}

static int cmd_apply(int argc, char **argv) {
    StScope scope = ST_SCOPE_USER;
    int use_paste = 0;
    const char *files[ST_MAX_LICENSE_BATCH];
    size_t file_count = 0;

    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            st_usage_apply();
            return 0;
        } else if (!strcmp(argv[i], "--system")) {
            scope = ST_SCOPE_SYSTEM;
        } else if (!strcmp(argv[i], "--paste")) {
            use_paste = 1;
        } else if (!strcmp(argv[i], "--")) {
            for (int j = i + 1; j < argc; j++) {
                if (file_count >= ST_MAX_LICENSE_BATCH) {
                    fputs("Too many input files.\n", stderr);
                    return 2;
                }
                files[file_count++] = argv[j];
            }
            break;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "Invalid option for apply: %s. Use --help.\n", argv[i]);
            return 2;
        } else {
            if (file_count >= ST_MAX_LICENSE_BATCH) {
                fputs("Too many input files.\n", stderr);
                return 2;
            }
            files[file_count++] = argv[i];
        }
    }

    if (use_paste && file_count > 0u) {
        fputs("apply: --paste cannot be combined with license code arguments.\n", stderr);
        return 2;
    }
    if (!file_count) {
        use_paste = 1;
    }

    char base_dir[ST_PATH_CAP], err[ST_PATH_CAP + 512];
    err[0] = '\0';
    if (!st_resolve_base_dir(scope, base_dir, sizeof(base_dir), err, sizeof(err))) {
        fprintf(stderr, "apply: %s\n", err);
        return 1;
    }

    StLicense *batch = NULL;
    size_t batch_count = 0, batch_cap = 0;
    if (use_paste) {
        char *paste_text = NULL;
        size_t paste_len = 0u;
        if (!st_read_paste_stdin(&paste_text, &paste_len, err, sizeof(err)) ||
            !st_load_code_text(paste_text, paste_len, "paste", &batch, &batch_count, &batch_cap, err, sizeof(err))) {
            free(paste_text);
            fprintf(stderr, "apply: %s\n", err);
            for (size_t k = 0; k < batch_count; k++) st_license_free(&batch[k]);
            free(batch);
            return 1;
        }
        free(paste_text);
    } else {
        for (size_t fi = 0; fi < file_count; fi++) {
            if (!st_lic_is_code_prefix(files[fi])) {
                fprintf(stderr, "apply: invalid license code (expected ST. license code)\n");
                for (size_t k = 0; k < batch_count; k++) st_license_free(&batch[k]);
                free(batch);
                return 1;
            }
            if (!st_load_code_text(files[fi], strlen(files[fi]), "code",
                                   &batch, &batch_count, &batch_cap, err, sizeof(err))) {
                fprintf(stderr, "apply: %s\n", err);
                for (size_t k = 0; k < batch_count; k++) st_license_free(&batch[k]);
                free(batch);
                return 1;
            }
        }
    }

    for (size_t i = 0; i < batch_count; i++) {
        for (size_t j = 0; j < i; j++) {
            if (!strcmp(batch[i].product, batch[j].product) &&
                !strcmp(batch[i].gpu_serial, batch[j].gpu_serial)) {
                fprintf(stderr, "apply: duplicate license for (%s, %s) in input batch\n",
                        batch[i].lib_name, batch[i].gpu_serial);
                for (size_t k = 0; k < batch_count; k++) st_license_free(&batch[k]);
                free(batch);
                return 1;
            }
        }
    }

    StApplyResult *results = (StApplyResult *)calloc(batch_count, sizeof(StApplyResult));
    if (!results) {
        for (size_t k = 0; k < batch_count; k++) st_license_free(&batch[k]);
        free(batch);
        fputs("apply: out of memory\n", stderr);
        return 1;
    }

    int file_mode = (scope == ST_SCOPE_SYSTEM) ? 0644 : 0600;
    for (size_t i = 0; i < batch_count; i++) {
        StLicense *lic = &batch[i];
        StApplyResult *res = &results[i];
        snprintf(res->product, sizeof(res->product), "%s", lic->product);
        snprintf(res->lib_name, sizeof(res->lib_name), "%s", lic->lib_name);
        snprintf(res->license_id, sizeof(res->license_id), "%s", lic->license_id);
        snprintf(res->gpu_serial, sizeof(res->gpu_serial), "%s", lic->gpu_serial);
        res->not_after = lic->not_after;
        snprintf(res->sha256, sizeof(res->sha256), "%s", lic->sha256);

        st_build_key_filename(lic->product, lic->gpu_serial, res->target_name, sizeof(res->target_name));
        char prod_dir[ST_PATH_CAP];
        if (!st_path_join(prod_dir, sizeof(prod_dir), base_dir, lic->lib_name) ||
            !st_path_join(res->target_path, sizeof(res->target_path), prod_dir, res->target_name)) {
            fprintf(stderr, "apply: target path too long for %s\n", lic->lib_name);
            for (size_t k = 0; k < batch_count; k++) st_license_free(&batch[k]);
            free(batch);
            free(results);
            return 1;
        }

        int base_exists = 0;
        if (!st_check_real_dir(base_dir, &base_exists, err, sizeof(err))) {
            fprintf(stderr, "apply: %s\n", err);
            for (size_t k = 0; k < batch_count; k++) st_license_free(&batch[k]);
            free(batch);
            free(results);
            return 1;
        }

        StDirKeyEntry *existing = NULL;
        size_t existing_count = 0;
        if (!st_scan_product_dir(prod_dir, &existing, &existing_count, err, sizeof(err))) {
            fprintf(stderr, "apply: %s\n", err);
            for (size_t k = 0; k < batch_count; k++) st_license_free(&batch[k]);
            free(batch);
            free(results);
            return 1;
        }

        size_t match_count = 0;
        size_t exact_target_idx = (size_t)-1;
        for (size_t ei = 0; ei < existing_count; ei++) {
            if (!existing[ei].parsed) continue;
            if (!strcmp(existing[ei].lic.product, lic->product) &&
                !strcmp(existing[ei].lic.gpu_serial, lic->gpu_serial)) {
                if (!strcmp(existing[ei].name, res->target_name) &&
                    !strcmp(existing[ei].lic.sha256, lic->sha256)) {
                    exact_target_idx = ei;
                }
                match_count++;
            }
        }

        if (match_count == 1u && exact_target_idx != (size_t)-1) {
            res->action = "unchanged";
            st_free_dir_entries(existing, existing_count);
            continue;
        }

        res->action = (match_count > 0u) ? "replaced" : "installed";

        if (!st_ensure_dirs(base_dir, prod_dir, scope, err, sizeof(err)) ||
            !st_atomic_write_file(prod_dir, res->target_name, lic->raw, lic->raw_len, file_mode, err, sizeof(err))) {
            fprintf(stderr, "apply: %s\n", err);
            st_free_dir_entries(existing, existing_count);
            for (size_t k = 0; k < batch_count; k++) st_license_free(&batch[k]);
            free(batch);
            free(results);
            return 1;
        }
        for (size_t ei = 0; ei < existing_count; ei++) {
            if (!existing[ei].parsed) continue;
            if (!strcmp(existing[ei].lic.product, lic->product) &&
                !strcmp(existing[ei].lic.gpu_serial, lic->gpu_serial) &&
                strcmp(existing[ei].name, res->target_name) != 0) {
#ifdef _WIN32
                if (!DeleteFileA(existing[ei].full_path)) {
                    fprintf(stderr, "apply: failed to remove replaced key %s (winerr %lu)\n",
                            existing[ei].full_path, (unsigned long)GetLastError());
                    st_free_dir_entries(existing, existing_count);
                    for (size_t k = 0; k < batch_count; k++) st_license_free(&batch[k]);
                    free(batch);
                    free(results);
                    return 1;
                }
#else
                if (unlink(existing[ei].full_path) != 0) {
                    fprintf(stderr, "apply: failed to remove replaced key %s: %s\n",
                            existing[ei].full_path, strerror(errno));
                    st_free_dir_entries(existing, existing_count);
                    for (size_t k = 0; k < batch_count; k++) st_license_free(&batch[k]);
                    free(batch);
                    free(results);
                    return 1;
                }
#endif
            }
        }
        st_free_dir_entries(existing, existing_count);
    }

    {
        int c = st_use_color();
        const char *b = c ? "\033[1m" : "", *d = c ? "\033[2m" : "", *g = c ? "\033[32m" : "";
        const char *cy = c ? "\033[36m" : "", *r = c ? "\033[0m" : "";
        char disp_root[ST_PATH_CAP];
        st_display_path(base_dir, disp_root, sizeof(disp_root));
        printf("%s%s%s\n", d, disp_root, r);

        size_t max_gpu_w = 16;
        for (size_t i = 0; i < batch_count; i++) {
            size_t gl = strlen(results[i].gpu_serial);
            if (gl > max_gpu_w) max_gpu_w = gl;
        }

        time_t now_t = time(NULL);
        uint64_t now = (now_t > 0) ? (uint64_t)now_t : 0u;

        for (size_t pi = 0; pi < sizeof(ST_PRODUCTS) / sizeof(ST_PRODUCTS[0]); pi++) {
            const StProductInfo *p = &ST_PRODUCTS[pi];
            int has_any = 0;
            for (size_t i = 0; i < batch_count; i++) {
                if (!strcmp(results[i].product, p->product)) { has_any = 1; break; }
            }
            if (!has_any) continue;
            printf("\n%s%s%s\n", b, p->lib_name, r);
            for (size_t i = 0; i < batch_count; i++) {
                StApplyResult *res = &results[i];
                if (strcmp(res->product, p->product) != 0) continue;
                const char *glyph = "+", *col = g;
                if (!strcmp(res->action, "replaced")) { glyph = "\xe2\x86\xbb"; col = cy; }
                else if (!strcmp(res->action, "unchanged")) { glyph = "="; col = d; }
                char date_buf[64], rel_buf[64];
                st_format_date_rel(res->not_after, now, date_buf, sizeof(date_buf), rel_buf, sizeof(rel_buf), NULL);
                if (date_buf[0]) {
                    printf("  %s%s %-9s%s  %-*s  %s  %s%s%s\n",
                           col, glyph, res->action, r,
                           (int)max_gpu_w, res->gpu_serial,
                           date_buf, d, rel_buf, r);
                } else {
                    printf("  %s%s %-9s%s  %s  %sperpetual%s\n",
                           col, glyph, res->action, r,
                           res->gpu_serial, d, r);
                }
            }
        }
    }

    int write_err = ferror(stdout) || (fflush(stdout) != 0);
    for (size_t k = 0; k < batch_count; k++) st_license_free(&batch[k]);
    free(batch);
    free(results);
    if (write_err) {
        fputs("apply: failed to write output.\n", stderr);
        return 2;
    }
    return 0;
}

static int cmd_list(int argc, char **argv) {
    StScope scope = ST_SCOPE_USER;
    int format = 0;

    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            st_usage_list();
            return 0;
        } else if (!strcmp(argv[i], "--system")) {
            scope = ST_SCOPE_SYSTEM;
        } else if (!strcmp(argv[i], "--json")) {
            format = 2;
        } else {
            fprintf(stderr, "Invalid option for list: %s. Use --help.\n", argv[i]);
            return 2;
        }
    }

    char base_dir[ST_PATH_CAP], err[ST_PATH_CAP + 512];
    err[0] = '\0';
    if (!st_resolve_base_dir(scope, base_dir, sizeof(base_dir), err, sizeof(err))) {
        fprintf(stderr, "list: %s\n", err);
        return 1;
    }

    int base_exists = 0;
    if (!st_check_real_dir(base_dir, &base_exists, err, sizeof(err))) {
        fprintf(stderr, "list: %s\n", err);
        return 1;
    }

    StListedLicense *rows = NULL;
    size_t row_cnt = 0, row_cap = 0;
    time_t now_t = time(NULL);
    uint64_t now = (now_t > 0) ? (uint64_t)now_t : 0u;

    for (size_t pi = 0; pi < sizeof(ST_PRODUCTS) / sizeof(ST_PRODUCTS[0]); pi++) {
        const StProductInfo *p = &ST_PRODUCTS[pi];
        if (!base_exists) continue;
        char prod_dir[ST_PATH_CAP];
        if (!st_path_join(prod_dir, sizeof(prod_dir), base_dir, p->lib_name)) continue;

        StDirKeyEntry *entries = NULL;
        size_t ecount = 0;
        if (!st_scan_product_dir(prod_dir, &entries, &ecount, err, sizeof(err))) {
            free(rows);
            fprintf(stderr, "list: %s\n", err);
            return 1;
        }
        for (size_t ei = 0; ei < ecount; ei++) {
            if (row_cnt >= row_cap) {
                size_t ncap = (row_cap == 0) ? 16u : (row_cap * 2u);
                StListedLicense *tmp = (StListedLicense *)realloc(rows, ncap * sizeof(StListedLicense));
                if (!tmp) {
                    st_free_dir_entries(entries, ecount);
                    free(rows);
                    fputs("list: out of memory\n", stderr);
                    return 1;
                }
                rows = tmp;
                row_cap = ncap;
            }
            StListedLicense *item = &rows[row_cnt++];
            memset(item, 0, sizeof(*item));
            snprintf(item->filename, sizeof(item->filename), "%s", entries[ei].name);
            snprintf(item->path, sizeof(item->path), "%s", entries[ei].full_path);
            if (!entries[ei].parsed) {
                snprintf(item->product, sizeof(item->product), "%s", p->product);
                snprintf(item->lib_name, sizeof(item->lib_name), "%s", p->lib_name);
                snprintf(item->status, sizeof(item->status), "invalid");
                snprintf(item->error, sizeof(item->error), "%s", entries[ei].error);
            } else {
                StLicense *lic = &entries[ei].lic;
                snprintf(item->product, sizeof(item->product), "%s", lic->product);
                snprintf(item->lib_name, sizeof(item->lib_name), "%s", lic->lib_name);
                snprintf(item->license_id, sizeof(item->license_id), "%s", lic->license_id);
                snprintf(item->gpu_serial, sizeof(item->gpu_serial), "%s", lic->gpu_serial);
                snprintf(item->order_id, sizeof(item->order_id), "%s", lic->order_id);
                snprintf(item->term, sizeof(item->term), "%s", lic->term);
                item->issued_at = lic->issued_at;
                item->not_before = lic->not_before;
                item->not_after = lic->not_after;
                snprintf(item->sha256, sizeof(item->sha256), "%s", lic->sha256);
                if (lic->not_after == 0) snprintf(item->status, sizeof(item->status), "perpetual");
                else if (now >= lic->not_after) snprintf(item->status, sizeof(item->status), "expired");
                else snprintf(item->status, sizeof(item->status), "active");
            }
        }
        st_free_dir_entries(entries, ecount);
    }

    if (row_cnt > 1u) qsort(rows, row_cnt, sizeof(StListedLicense), st_cmp_listed_license);

    if (format == 2) {
        fputs("{\"schema\":\"solved-tech.licenses.v1\",\"tool_version\":\"" VERSION "\",\"scope\":", stdout);
        json_string(stdout, st_scope_name(scope));
        fputs(",\"root\":", stdout);
        json_string(stdout, base_dir);
        fputs(",\"licenses\":[", stdout);
        for (size_t i = 0; i < row_cnt; i++) {
            StListedLicense *item = &rows[i];
            char exp[48];
            if (!strcmp(item->status, "invalid")) exp[0] = '\0';
            else st_format_utc(item->not_after, exp, sizeof(exp));
            if (i) fputc(',', stdout);
            fputs("{\"product\":", stdout); json_string(stdout, item->product);
            fputs(",\"lib_name\":", stdout); json_string(stdout, item->lib_name);
            fputs(",\"filename\":", stdout); json_string(stdout, item->filename);
            fputs(",\"path\":", stdout); json_string(stdout, item->path);
            fputs(",\"status\":", stdout); json_string(stdout, item->status);
            fputs(",\"license_id\":", stdout); json_string(stdout, item->license_id);
            fputs(",\"gpu_serial\":", stdout); json_string(stdout, item->gpu_serial);
            fprintf(stdout, ",\"issued_at\":%llu,\"not_before\":%llu,\"not_after\":%llu,\"expires_utc\":",
                    (unsigned long long)item->issued_at,
                    (unsigned long long)item->not_before,
                    (unsigned long long)item->not_after);
            json_string(stdout, exp);
            fputs(",\"order_id\":", stdout); json_string(stdout, item->order_id);
            fputs(",\"term\":", stdout); json_string(stdout, item->term);
            fputs(",\"sha256\":", stdout); json_string(stdout, item->sha256);
            fputs(",\"error\":", stdout); json_string(stdout, item->error);
            fputc('}', stdout);
        }
        fputs("]}\n", stdout);
    } else {
        int c = st_use_color();
        const char *b = c ? "\033[1m" : "", *d = c ? "\033[2m" : "", *g = c ? "\033[32m" : "";
        const char *y = c ? "\033[33m" : "", *rd = c ? "\033[31m" : "", *r = c ? "\033[0m" : "";
        char disp_root[ST_PATH_CAP];
        st_display_path(base_dir, disp_root, sizeof(disp_root));
        printf("%s%s%s\n", d, disp_root, r);
        if (row_cnt == 0) {
            printf("\n  %sNo licenses installed.%s\n", d, r);
        } else {
            size_t max_gpu_w = 16;
            for (size_t i = 0; i < row_cnt; i++) {
                if (!strcmp(rows[i].status, "invalid")) continue;
                size_t gl = strlen(rows[i].gpu_serial);
                if (gl > max_gpu_w) max_gpu_w = gl;
            }
            for (size_t pi = 0; pi < sizeof(ST_PRODUCTS) / sizeof(ST_PRODUCTS[0]); pi++) {
                const StProductInfo *p = &ST_PRODUCTS[pi];
                int has_any = 0;
                for (size_t i = 0; i < row_cnt; i++) {
                    if (!strcmp(rows[i].product, p->product)) { has_any = 1; break; }
                }
                if (!has_any) continue;
                printf("\n%s%s%s\n", b, p->lib_name, r);
                for (size_t i = 0; i < row_cnt; i++) {
                    StListedLicense *item = &rows[i];
                    if (strcmp(item->product, p->product) != 0) continue;
                    if (!strcmp(item->status, "invalid")) {
                        printf("  %s! %-9s%s  %s  %s%s%s\n",
                               rd, "invalid", r, item->filename, d, item->error, r);
                    } else if (!strcmp(item->status, "perpetual")) {
                        printf("  %s\xe2\x97\x8f %-9s%s  %s\n",
                               g, "perpetual", r, item->gpu_serial);
                    } else {
                        char date_buf[64], rel_buf[64];
                        int soon = 0;
                        st_format_date_rel(item->not_after, now, date_buf, sizeof(date_buf), rel_buf, sizeof(rel_buf), &soon);
                        if (!strcmp(item->status, "expired")) {
                            printf("  %s\xc3\x97 %-9s%s  %s%-*s%s  %s%s%s  %s%s%s\n",
                                   rd, "expired", r,
                                   d, (int)max_gpu_w, item->gpu_serial, r,
                                   d, date_buf, r,
                                   rd, rel_buf, r);
                        } else {
                            const char *sc = soon ? y : g;
                            const char *rc = soon ? y : d;
                            printf("  %s\xe2\x97\x8f %-9s%s  %-*s  %s  %s%s%s\n",
                                   sc, "active", r,
                                   (int)max_gpu_w, item->gpu_serial,
                                   date_buf,
                                   rc, rel_buf, r);
                        }
                    }
                }
            }
        }
    }

    int write_err = ferror(stdout) || (fflush(stdout) != 0);
    free(rows);
    if (write_err) {
        fputs("list: failed to write output.\n", stderr);
        return 2;
    }
    return 0;
}

#endif
