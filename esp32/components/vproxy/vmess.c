/*
 * VMess AEAD client. See vmess.h.
 */
#include "vmess.h"

#include <string.h>

#define KDF_BASE "VMess AEAD KDF"
#define ALG_GCM PSA_ALG_GCM

// ---- hashing ----

static int sha256(const uint8_t *a, size_t an, const uint8_t *b, size_t bn, uint8_t out[32]) {
    psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
    size_t n;
    psa_status_t st = psa_hash_setup(&op, PSA_ALG_SHA_256);
    if (st == PSA_SUCCESS && an) st = psa_hash_update(&op, a, an);
    if (st == PSA_SUCCESS && bn) st = psa_hash_update(&op, b, bn);
    if (st == PSA_SUCCESS) st = psa_hash_finish(&op, out, 32, &n);
    if (st != PSA_SUCCESS) psa_hash_abort(&op);
    return (int)st;
}

// The KDF stacks HMACs: level 0 is SHA-256, level i is HMAC keyed with
// keys[i-1] whose hash function is level i-1. Messages here are short, so the
// two-part input (prefix block, message) is passed down instead of streamed.
typedef struct {
    const uint8_t *const *keys;
    const size_t *klen;
} kdf_chain_t;

static int nh(const kdf_chain_t *c, int level, const uint8_t *a, size_t an,
              const uint8_t *b, size_t bn, uint8_t out[32]) {
    if (level == 0) return sha256(a, an, b, bn, out);
    const uint8_t *k = c->keys[level - 1];
    size_t kl = c->klen[level - 1];
    if (kl > 64) return PSA_ERROR_INVALID_ARGUMENT;   // not needed by VMess
    // pad block | message. Each level down prepends a 64-byte block, so with
    // at most 5 levels and a 16-byte key the message stays under 16 + 5 * 64.
    uint8_t pad[64 + 352];
    if (an + bn > sizeof(pad) - 64) return PSA_ERROR_INVALID_ARGUMENT;
    uint8_t inner[32];
    for (int i = 0; i < 64; i++) pad[i] = (uint8_t)((i < (int)kl ? k[i] : 0) ^ 0x36);
    memcpy(pad + 64, a, an);
    memcpy(pad + 64 + an, b, bn);
    int r = nh(c, level - 1, pad, 64, pad + 64, an + bn, inner);
    if (r) return r;
    for (int i = 0; i < 64; i++) pad[i] = (uint8_t)((i < (int)kl ? k[i] : 0) ^ 0x5c);
    return nh(c, level - 1, pad, 64, inner, 32, out);
}

int vmess_kdf(const uint8_t *key, size_t key_len, const uint8_t *const *path,
              const size_t *path_len, int n_path, uint8_t out[32]) {
    const uint8_t *keys[6];
    size_t klen[6];
    if (n_path > 5) return PSA_ERROR_INVALID_ARGUMENT;
    keys[0] = (const uint8_t *)KDF_BASE;
    klen[0] = sizeof(KDF_BASE) - 1;
    for (int i = 0; i < n_path; i++) {
        keys[i + 1] = path[i];
        klen[i + 1] = path_len[i];
    }
    kdf_chain_t c = {keys, klen};
    return nh(&c, n_path + 1, key, key_len, NULL, 0, out);
}

static int kdf_s(const uint8_t *key, size_t kl, const char *label,
                 const uint8_t *p2, size_t p2l, const uint8_t *p3, size_t p3l, uint8_t out[32]) {
    const uint8_t *path[3] = {(const uint8_t *)label, p2, p3};
    size_t len[3] = {strlen(label), p2l, p3l};
    return vmess_kdf(key, kl, path, len, p2 ? (p3 ? 3 : 2) : 1, out);
}

// ---- AES ----

static int import_key(const uint8_t k[16], psa_algorithm_t alg, psa_key_usage_t usage,
                      psa_key_id_t *id) {
    psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&a, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&a, 128);
    psa_set_key_algorithm(&a, alg);
    psa_set_key_usage_flags(&a, usage);
    psa_status_t st = psa_import_key(&a, k, 16, id);
    psa_reset_key_attributes(&a);
    return (int)st;
}

static int gcm_once(int enc, const uint8_t key[16], const uint8_t nonce[12],
                    const uint8_t *aad, size_t aadl, const uint8_t *in, size_t n,
                    uint8_t *out, size_t cap, size_t *olen) {
    psa_key_id_t id = 0;
    int r = import_key(key, ALG_GCM, enc ? PSA_KEY_USAGE_ENCRYPT : PSA_KEY_USAGE_DECRYPT, &id);
    if (r) return r;
    r = enc ? psa_aead_encrypt(id, ALG_GCM, nonce, 12, aad, aadl, in, n, out, cap, olen)
            : psa_aead_decrypt(id, ALG_GCM, nonce, 12, aad, aadl, in, n, out, cap, olen);
    psa_destroy_key(id);
    return r;
}

static uint32_t crc32_ieee(const uint8_t *p, size_t n) {
    uint32_t c = 0xffffffffu;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1u)));
    }
    return ~c;
}

static uint32_t fnv1a32(const uint8_t *p, size_t n) {
    uint32_t h = 0x811c9dc5u;
    while (n--) {
        h ^= *p++;
        h *= 0x01000193u;
    }
    return h;
}

static void be16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void be32(uint8_t *p, uint32_t v) { be16(p, (uint16_t)(v >> 16)); be16(p + 2, (uint16_t)v); }

static void chunk_nonce(uint16_t count, const uint8_t iv[16], uint8_t n[12]) {
    be16(n, count);
    memcpy(n + 2, iv + 2, 10);
}

// ---- session ----

int vmess_begin(vmess_sess_t *s, const uint8_t ck[16], const char *host, uint16_t port,
                int64_t ts, const uint8_t rnd[VMESS_RAND_LEN], uint8_t *out, size_t cap,
                size_t *out_len) {
    memset(s, 0, sizeof(*s));
    size_t hl = strlen(host);
    if (hl == 0 || hl > 255) return PSA_ERROR_INVALID_ARGUMENT;
    memcpy(s->body_iv, rnd, 16);
    memcpy(s->body_key, rnd + 16, 16);
    s->resp_v = rnd[32];
    const uint8_t *rand4 = rnd + 33, *conn_nonce = rnd + 37, *pad = rnd + 45;
    size_t pad_len = pad[0] % 4;

    // Plain request header.
    uint8_t h[1 + 16 + 16 + 5 + 2 + 2 + 255 + 3 + 4];
    size_t n = 0;
    h[n++] = 1;
    memcpy(h + n, s->body_iv, 16); n += 16;
    memcpy(h + n, s->body_key, 16); n += 16;
    h[n++] = s->resp_v;
    h[n++] = 0x01;                              // Opt: chunk stream, no masking
    h[n++] = (uint8_t)((pad_len << 4) | 0x03);  // padding length | AES-128-GCM
    h[n++] = 0;
    h[n++] = 0x01;                              // TCP
    be16(h + n, port); n += 2;
    h[n++] = 2;                                 // domain
    h[n++] = (uint8_t)hl;
    memcpy(h + n, host, hl); n += hl;
    memcpy(h + n, pad, pad_len); n += pad_len;
    be32(h + n, fnv1a32(h, n)); n += 4;

    if (cap < 16 + 18 + 8 + n + 16) return PSA_ERROR_BUFFER_TOO_SMALL;

    // Auth ID.
    uint8_t k[32], nonce[32], aid_plain[16], *aid = out;
    int r = kdf_s(ck, 16, "AES Auth ID Encryption", NULL, 0, NULL, 0, k);
    if (r) return r;
    for (int i = 0; i < 8; i++) aid_plain[i] = (uint8_t)((uint64_t)ts >> (56 - 8 * i));
    memcpy(aid_plain + 8, rand4, 4);
    be32(aid_plain + 12, crc32_ieee(aid_plain, 12));
    psa_key_id_t ecb = 0;
    size_t ol;
    r = import_key(k, PSA_ALG_ECB_NO_PADDING, PSA_KEY_USAGE_ENCRYPT, &ecb);
    if (r) return r;
    r = psa_cipher_encrypt(ecb, PSA_ALG_ECB_NO_PADDING, aid_plain, 16, aid, 16, &ol);
    psa_destroy_key(ecb);
    if (r) return r;

    // Sealed length, then the connection nonce, then the sealed header.
    uint8_t lenb[2];
    be16(lenb, (uint16_t)n);
    if ((r = kdf_s(ck, 16, "VMess Header AEAD Key_Length", aid, 16, conn_nonce, 8, k))) return r;
    if ((r = kdf_s(ck, 16, "VMess Header AEAD Nonce_Length", aid, 16, conn_nonce, 8, nonce))) return r;
    if ((r = gcm_once(1, k, nonce, aid, 16, lenb, 2, out + 16, 18, &ol))) return r;
    memcpy(out + 34, conn_nonce, 8);
    if ((r = kdf_s(ck, 16, "VMess Header AEAD Key", aid, 16, conn_nonce, 8, k))) return r;
    if ((r = kdf_s(ck, 16, "VMess Header AEAD Nonce", aid, 16, conn_nonce, 8, nonce))) return r;
    if ((r = gcm_once(1, k, nonce, aid, 16, h, n, out + 42, cap - 42, &ol))) return r;
    *out_len = 42 + ol;

    // Body keys.
    uint8_t d[32];
    if ((r = sha256(s->body_key, 16, NULL, 0, d))) return r;
    memcpy(s->resp_key, d, 16);
    if ((r = sha256(s->body_iv, 16, NULL, 0, d))) return r;
    memcpy(s->resp_iv, d, 16);
    if ((r = import_key(s->body_key, ALG_GCM, PSA_KEY_USAGE_ENCRYPT, &s->tx_key))) return r;
    if ((r = import_key(s->resp_key, ALG_GCM, PSA_KEY_USAGE_DECRYPT, &s->rx_key))) return r;
    memset(k, 0, sizeof(k));
    return 0;
}

int vmess_seal(vmess_sess_t *s, const uint8_t *in, size_t n, uint8_t *out, size_t cap,
               size_t *out_len) {
    if (n > VMESS_CHUNK_PLAIN_MAX || cap < n + VMESS_CHUNK_OVERHEAD) return PSA_ERROR_BUFFER_TOO_SMALL;
    uint8_t nonce[12];
    size_t ol;
    chunk_nonce(s->tx_count, s->body_iv, nonce);
    int r = psa_aead_encrypt(s->tx_key, ALG_GCM, nonce, 12, NULL, 0, in, n, out + 2, cap - 2, &ol);
    if (r) return r;
    be16(out, (uint16_t)ol);
    s->tx_count++;
    *out_len = ol + 2;
    return 0;
}

int vmess_open(vmess_sess_t *s, const uint8_t *in, size_t len, size_t *used, uint8_t *out,
               size_t cap, size_t *out_len) {
    uint8_t k[32], nonce[32], tmp[2 + 255];
    size_t ol;
    int r;
    *used = 0;
    *out_len = 0;
    switch (s->rx_state) {
    case 0:
        if (len < 18) return 1;
        if ((r = kdf_s(s->resp_key, 16, "AEAD Resp Header Len Key", NULL, 0, NULL, 0, k))) return r;
        if ((r = kdf_s(s->resp_iv, 16, "AEAD Resp Header Len IV", NULL, 0, NULL, 0, nonce))) return r;
        if ((r = gcm_once(0, k, nonce, NULL, 0, in, 18, tmp, 2, &ol))) return r;
        s->rx_hdr_len = (uint16_t)((tmp[0] << 8) | tmp[1]);
        if (s->rx_hdr_len < 4 || s->rx_hdr_len > sizeof(tmp)) return PSA_ERROR_INVALID_SIGNATURE;
        s->rx_state = 1;
        *used = 18;
        return 0;
    case 1:
        if (len < (size_t)s->rx_hdr_len + 16) return 1;
        if ((r = kdf_s(s->resp_key, 16, "AEAD Resp Header Key", NULL, 0, NULL, 0, k))) return r;
        if ((r = kdf_s(s->resp_iv, 16, "AEAD Resp Header IV", NULL, 0, NULL, 0, nonce))) return r;
        if ((r = gcm_once(0, k, nonce, NULL, 0, in, s->rx_hdr_len + 16u, tmp, sizeof(tmp), &ol))) return r;
        if (tmp[0] != s->resp_v) return PSA_ERROR_INVALID_SIGNATURE;
        s->rx_state = 2;
        *used = s->rx_hdr_len + 16u;
        return 0;
    case 2: {
        if (len < 2) return 1;
        size_t l = (size_t)((in[0] << 8) | in[1]);
        if (l < 16 || l - 16 > cap) return PSA_ERROR_INVALID_SIGNATURE;
        if (len < 2 + l) return 1;
        chunk_nonce(s->rx_count, s->resp_iv, nonce);
        r = psa_aead_decrypt(s->rx_key, ALG_GCM, nonce, 12, NULL, 0, in + 2, l, out, cap, &ol);
        if (r) return r;
        s->rx_count++;
        *used = 2 + l;
        *out_len = ol;
        if (ol == 0) {
            s->rx_state = 3;
            return 2;
        }
        return 0;
    }
    default:
        return 2;
    }
}

void vmess_end(vmess_sess_t *s) {
    if (s->tx_key) psa_destroy_key(s->tx_key);
    if (s->rx_key) psa_destroy_key(s->rx_key);
    memset(s, 0, sizeof(*s));
}
