/*
 * VMess AEAD client (alterId 0, TCP, AES-128-GCM, no chunk masking).
 *
 * Pure protocol code on top of PSA Crypto, no sockets, so it builds and is
 * tested on the host (tests/vproxy) against the Python reference
 * (tools/vmess_ref.py in the gadget workspace).
 *
 * The device never sees the user's UUID: the provisioning script derives the
 * 16-byte command key, MD5(UUID || salt), on the computer and stores only that.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "psa/crypto.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VMESS_RAND_LEN 48          // body IV 16 | body key 16 | V 1 | auth rand 4 | nonce 8 | pad 3
#define VMESS_FIRST_MAX (16 + 18 + 8 + 64 + 255 + 16)
#define VMESS_CHUNK_PLAIN_MAX 8192 // what we send per chunk; we accept up to 16 KiB in
#define VMESS_CHUNK_OVERHEAD 18    // 2 length + 16 tag

typedef struct {
    uint8_t body_iv[16], body_key[16];
    uint8_t resp_iv[16], resp_key[16];
    uint8_t resp_v;
    uint16_t tx_count, rx_count;
    psa_key_id_t tx_key, rx_key;
    uint8_t rx_state;              // 0: response header length, 1: header body, 2: chunks, 3: done
    uint16_t rx_hdr_len;
} vmess_sess_t;

// Nested HMAC-SHA256 KDF from v2ray: KDF(key, path...). out is 32 bytes.
int vmess_kdf(const uint8_t *key, size_t key_len, const uint8_t *const *path,
              const size_t *path_len, int n_path, uint8_t out[32]);

// Starts a session to host:port. Writes the sealed request header (the first
// bytes on the wire) to out. rnd is VMESS_RAND_LEN random bytes; ts is unix
// seconds. Returns 0 or a negative PSA status.
int vmess_begin(vmess_sess_t *s, const uint8_t cmd_key[16], const char *host,
                uint16_t port, int64_t ts, const uint8_t rnd[VMESS_RAND_LEN],
                uint8_t *out, size_t cap, size_t *out_len);

// Seals one chunk (n <= VMESS_CHUNK_PLAIN_MAX; n == 0 is the end marker).
int vmess_seal(vmess_sess_t *s, const uint8_t *in, size_t n, uint8_t *out,
               size_t cap, size_t *out_len);

// Parses server bytes. Consumes at most one unit (the response header or one
// chunk) per call and reports how many bytes it used; plaintext, if any, goes
// to out (cap >= 16384). Returns 1 when more input is needed, 0 when it made
// progress, 2 at the end marker, negative on a bad packet.
int vmess_open(vmess_sess_t *s, const uint8_t *in, size_t len, size_t *used,
               uint8_t *out, size_t cap, size_t *out_len);

void vmess_end(vmess_sess_t *s);

#ifdef __cplusplus
}
#endif
