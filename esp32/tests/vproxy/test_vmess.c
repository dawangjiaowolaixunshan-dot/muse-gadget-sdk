// Host test: prints the same lines as `tools/vmess_ref.py vectors` so the two
// can be diffed byte for byte, then checks the response parser.
#include <stdio.h>
#include <string.h>

#include "vmess.h"

static void hex(const char *name, const uint8_t *p, size_t n) {
    printf("%s ", name);
    for (size_t i = 0; i < n; i++) printf("%02x", p[i]);
    printf("\n");
}

static int unhex(const char *s, uint8_t *out) {
    int n = 0;
    while (s[0] && s[1]) {
        unsigned v;
        sscanf(s, "%2x", &v);
        out[n++] = (uint8_t)v;
        s += 2;
    }
    return n;
}

int main(int argc, char **argv) {
    if (psa_crypto_init() != PSA_SUCCESS) return 1;
    // cmd_key for UUID b831381d-6324-4d53-ad4f-8cda48b30811, from the Python side.
    uint8_t ck[16];
    unhex(argv[1], ck);
    uint8_t rnd[VMESS_RAND_LEN];
    for (int i = 0; i < VMESS_RAND_LEN; i++) rnd[i] = (uint8_t)i;

    hex("cmd_key", ck, 16);
    uint8_t k[32];
    const uint8_t *p[1] = {(const uint8_t *)"AES Auth ID Encryption"};
    size_t pl[1] = {22};
    vmess_kdf(ck, 16, p, pl, 1, k);
    hex("kdf_authid_key", k, 16);

    vmess_sess_t s;
    uint8_t first[VMESS_FIRST_MAX];
    size_t fl;
    int r = vmess_begin(&s, ck, "api.muse.ai", 443, 1794873600, rnd, first, sizeof(first), &fl);
    if (r) { printf("begin failed %d\n", r); return 1; }
    hex("auth_id", first, 16);
    hex("first", first, fl);
    uint8_t ch[64];
    size_t cl;
    vmess_seal(&s, (const uint8_t *)"hello", 5, ch, sizeof(ch), &cl);
    hex("chunk0", ch, cl);
    hex("resp_key", s.resp_key, 16);
    hex("resp_iv", s.resp_iv, 16);

    // Response from the Python side: header, "world", end marker.
    static uint8_t resp[512], out[16384];
    int n = unhex(argv[2], resp), off = 0;
    char got[64] = {0};
    size_t gl = 0;
    for (;;) {
        size_t used, ol;
        r = vmess_open(&s, resp + off, (size_t)(n - off), &used, out, sizeof(out), &ol);
        if (r < 0) { printf("open failed %d\n", r); return 1; }
        off += (int)used;
        memcpy(got + gl, out, ol);
        gl += ol;
        if (r == 2) break;
        if (r == 1) { printf("truncated\n"); return 1; }
    }
    printf("resp_plain %s (%s)\n", got, off == n ? "all consumed" : "left over");
    // 验证 16 位分块计数器逼近上限时主动终止流，杜绝 AES-GCM Nonce 回绕重用
    s.tx_count = 65000;
    if (vmess_seal(&s, (const uint8_t *)"x", 1, ch, sizeof(ch), &cl) == 0) {
        printf("tx_count limit failed\n");
        return 1;
    }
    s.rx_state = 2;
    s.rx_count = 65000;
    size_t u2 = 0, ol2 = 0;
    if (vmess_open(&s, resp, 0, &u2, out, sizeof(out), &ol2) != 2) {
        printf("rx_count limit failed\n");
        return 1;
    }
    vmess_end(&s);
    return strcmp(got, "world") != 0 || off != n;
}
