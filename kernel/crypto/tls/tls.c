// TLS 1.2 client: bounded handshake parsing, certificate and ECDHE signature
// verification, Finished exchange and AES-GCM application records.
#include "../../core/kernel.h"
#include "../../net/tcp.h"
#include "../../net/dns.h"
#include "tls.h"
#include "../ct.h"
#include "../curve25519.h"
#include "tls_prf.h"
#include "../aes_gcm.h"
#include "../sha256.h"
#include "../csprng.h"
#include "../der.h"
#include "../p256.h"
#include "../p384.h"
#include "../sha512.h"
#include "../rsa.h"
#include "../x509.h"

// Diagnostic prints inside tls_https_fetch are gated on its `verbose` parameter, so the
// `tls` command shows the full handshake while Selene fetches silently. (Only valid where
// a `verbose` variable is in scope — i.e. inside tls_https_fetch.)
#define TLSP(...) do { if (verbose) printf(__VA_ARGS__); } while (0)

static uint8_t tls_hs[1024];        // outbound ClientHello (record + handshake)
static uint8_t tls_rx[20480];       // receive buffer — holds >= one max TLS record (app data is streamed)
static uint8_t tls_hd[16384];       // reassembled handshake message stream
static uint8_t tls_client_random[32];   // saved from our ClientHello (for the key schedule)
static uint8_t tls_server_random[32];   // parsed from the ServerHello
static uint8_t tls_ts[20480];           // handshake transcript (hashed for the Finished verify_data)
static uint8_t tls_ske_params[160];     // ServerECDHParams — the data the SKE signature covers
static uint8_t tls_ske_sig[512];        // the SKE signature (ECDSA DER-Sig-Value, or an RSA sig up to 4096-bit)

// Require the supported chain, hostname, leaf-date and key-exchange checks by
// default. `tlsstrict off` is an explicit session-only development override.
// Forged chains and invalid key-exchange signatures are rejected in either mode.
static int tls_strict = 1;
void tls_set_strict(int on) { tls_strict = on ? 1 : 0; }
int  tls_get_strict(void)   { return tls_strict; }


static void put8(uint8_t* b, int* p, uint8_t v)   { b[(*p)++] = v; }
static void put16(uint8_t* b, int* p, uint16_t v) { b[(*p)++] = (uint8_t)(v >> 8); b[(*p)++] = (uint8_t)v; }
static void putn(uint8_t* b, int* p, const uint8_t* d, int n) { for (int i = 0; i < n; i++) b[(*p)++] = d[i]; }

// Build the ClientHello (record + handshake) into tls_hs; return its total length.
static int build_client_hello(const char* host) {
    uint8_t* b = tls_hs; int p = 0;

    put8(b, &p, 22);                 // record: handshake
    put16(b, &p, 0x0301);            // record version TLS 1.0 (classic ClientHello compat)
    int rec_len_at = p; put16(b, &p, 0);           // record length (backpatched)
    int hs_start = p;

    put8(b, &p, 1);                  // handshake: ClientHello
    int hs_len_at = p; put8(b, &p, 0); put8(b, &p, 0); put8(b, &p, 0);   // 24-bit length
    int ch_start = p;

    put16(b, &p, 0x0303);            // client_version TLS 1.2
    csprng_bytes(tls_client_random, 32);            // 32-byte random (CSPRNG; saved for the key schedule)
    putn(b, &p, tls_client_random, 32);
    put8(b, &p, 0);                  // session_id length 0

    static const uint16_t suites[] = {
        0xC02F, 0xC02B, 0xC030, 0xC02C,   // ECDHE-RSA/ECDSA AES-GCM
        0x009C, 0x009D,                   // RSA AES-GCM
        0x002F, 0x0035                    // RSA AES-CBC
    };
    int nsuites = (int)(sizeof(suites) / sizeof(suites[0]));
    put16(b, &p, (uint16_t)(nsuites * 2));
    for (int i = 0; i < nsuites; i++) put16(b, &p, suites[i]);

    put8(b, &p, 1); put8(b, &p, 0);  // compression: 1 method, null

    int ext_len_at = p; put16(b, &p, 0);           // extensions length (backpatched)
    int ext_start = p;

    int hlen = (int)strlen(host);                  // SNI (server_name)
    put16(b, &p, 0x0000); put16(b, &p, (uint16_t)(hlen + 5));
    put16(b, &p, (uint16_t)(hlen + 3)); put8(b, &p, 0); put16(b, &p, (uint16_t)hlen);
    putn(b, &p, (const uint8_t*)host, hlen);

    put16(b, &p, 0x000A); put16(b, &p, 8);         // supported_groups: x25519, secp256r1, secp384r1
    put16(b, &p, 6); put16(b, &p, 0x001D); put16(b, &p, 0x0017); put16(b, &p, 0x0018);

    put16(b, &p, 0x000B); put16(b, &p, 2); put8(b, &p, 1); put8(b, &p, 0);   // ec_point_formats: uncompressed

    put16(b, &p, 0x000D); put16(b, &p, 12);        // signature_algorithms
    put16(b, &p, 10);
    put16(b, &p, 0x0401); put16(b, &p, 0x0403); put16(b, &p, 0x0503); put16(b, &p, 0x0804); put16(b, &p, 0x0601);

    put16(b, &p, 0x002B); put16(b, &p, 3); put8(b, &p, 2); put16(b, &p, 0x0303);  // supported_versions: TLS 1.2

    int ext_total = p - ext_start;
    b[ext_len_at] = (uint8_t)(ext_total >> 8); b[ext_len_at + 1] = (uint8_t)ext_total;
    int ch_total = p - ch_start;
    b[hs_len_at] = 0; b[hs_len_at + 1] = (uint8_t)(ch_total >> 8); b[hs_len_at + 2] = (uint8_t)ch_total;
    int rec_total = p - hs_start;
    b[rec_len_at] = (uint8_t)(rec_total >> 8); b[rec_len_at + 1] = (uint8_t)rec_total;
    return p;
}

// Print up to `max` bytes as text, replacing non-printables (except tab/newlines) with '.'.
static void tls_print_text(const uint8_t* b, uint32_t n, uint32_t max) {
    if (n > max) n = max;
    char one[2]; one[1] = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t c = b[i];
        one[0] = (c == '\n' || c == '\r' || c == '\t' || (c >= 32 && c < 127)) ? (char)c : '.';
        printf("%s", one);
    }
    printf("\n");
}

static int tls_hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}
static void tls_hex(uint8_t* out, const char* h, int n) {
    for (int i = 0; i < n; i++) out[i] = (uint8_t)((tls_hexval(h[2 * i]) << 4) | tls_hexval(h[2 * i + 1]));
}

// Known-answer test for the TLS 1.2 key schedule: it chains the (separately KAT'd) X25519
// and PRF primitives with the EXACT labels and seed order the live handshake uses, so a
// wrong label or a swapped client/server-random order is caught here rather than only at a
// real server's Finished check. The pre-master secret is the RFC 7748 §6.1 X25519 shared
// secret; the expected master secret and key block were produced by an independent PRF.
int tls_keyschedule_selftest(void) {
    uint8_t pm[32], cr[32], sr[32], seed[64], master[48], keyblock[40], want[48], wantk[40];
    tls_hex(pm, "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742", 32);
    for (int i = 0; i < 32; i++) { cr[i] = (uint8_t)i; sr[i] = (uint8_t)(0x20 + i); }

    memcpy(seed, cr, 32); memcpy(seed + 32, sr, 32);
    tls12_prf(pm, 32, "master secret", seed, 64, master, 48);
    tls_hex(want, "3dcd0e1fa717e41ff560509c61c4039922fb8d2a7580728ef991c0748f244b0b"
                  "4125f429b4f71ed8b2084093e40953ae", 48);
    int ok_m = 1; for (int i = 0; i < 48; i++) if (master[i] != want[i]) ok_m = 0;

    memcpy(seed, sr, 32); memcpy(seed + 32, cr, 32);
    tls12_prf(master, 48, "key expansion", seed, 64, keyblock, 40);
    tls_hex(wantk, "0b98d0c76e21f26689a87b7749abe9938329f7c1e746cf68358623233e646082"
                   "eaaa7227ebba690e", 40);
    int ok_k = 1; for (int i = 0; i < 40; i++) if (keyblock[i] != wantk[i]) ok_k = 0;

    if (ok_m) printf("tlskeys: master-secret derivation PASS (48 B, label \"master secret\")\n");
    else      printf("tlskeys: master-secret derivation FAIL\n");
    if (ok_k) printf("tlskeys: key expansion PASS (40 B AES-128-GCM key block, label \"key expansion\")\n");
    else      printf("tlskeys: key expansion FAIL\n");
    printf("tlskeys: self-test %d/2 vectors passed\n", ok_m + ok_k);
    return (ok_m && ok_k) ? 0 : -1;
}

static int tls_eq(const uint8_t* a, const uint8_t* b, int n) {
    return ct_memcmp(a, b, (uint32_t)n) == 0;   // constant-time: the Finished verify_data
}                                               // is secret, so never early-out on a mismatch

// ---- TLS 1.2 AES-128-GCM record protection (RFC 5288) -------------------------------
// GCM nonce = write_IV(4) || explicit_nonce(8). AAD = seq(8) || type || version || len.
// The on-wire fragment is explicit_nonce(8) || GCM_ciphertext || GCM_tag(16); we send the
// 64-bit sequence number as the explicit nonce.

static void tls_rec_aad(uint8_t aad[13], uint64_t seq, uint8_t type, uint16_t plen) {
    for (int i = 0; i < 8; i++) aad[i] = (uint8_t)(seq >> (8 * (7 - i)));
    aad[8]  = type;
    aad[9]  = 0x03; aad[10] = 0x03;                  // TLS 1.2
    aad[11] = (uint8_t)(plen >> 8); aad[12] = (uint8_t)plen;
}

// Seal pt into out = explicit_nonce(8) || ct || tag(16); returns the fragment length.
static int tls_seal_record(const uint8_t key[16], const uint8_t iv4[4], uint64_t seq,
                           uint8_t type, const uint8_t* pt, uint32_t pt_len, uint8_t* out) {
    uint8_t nonce[12], aad[13];
    for (int i = 0; i < 4; i++) nonce[i] = iv4[i];
    for (int i = 0; i < 8; i++) { nonce[4 + i] = (uint8_t)(seq >> (8 * (7 - i))); out[i] = nonce[4 + i]; }
    tls_rec_aad(aad, seq, type, (uint16_t)pt_len);
    aes128_gcm_encrypt(key, nonce, aad, 13, pt, pt_len, out + 8, out + 8 + pt_len);
    return (int)(8 + pt_len + 16);
}

// Open a fragment (explicit_nonce(8) || ct || tag(16)); returns pt_len, or -1 on a bad tag.
static int tls_open_record(const uint8_t key[16], const uint8_t iv4[4], uint64_t seq,
                           uint8_t type, const uint8_t* in, uint32_t in_len, uint8_t* pt, uint32_t pt_max) {
    if (in_len < 8 + 16) return -1;
    uint32_t ct_len = in_len - 8 - 16;
    // The plaintext (== ciphertext length) is written into `pt`; refuse if it would not
    // fit. The record length comes off the wire, so without this a malicious server could
    // send an over-long record and make aes128_gcm_decrypt write far past `pt` — e.g. the
    // 64-byte stack buffer the Finished path passes — a remote memory-corruption primitive.
    if (ct_len > pt_max) return -1;
    uint8_t nonce[12], aad[13];
    for (int i = 0; i < 4; i++) nonce[i] = iv4[i];
    for (int i = 0; i < 8; i++) nonce[4 + i] = in[i];            // explicit nonce read off the wire
    tls_rec_aad(aad, seq, type, (uint16_t)ct_len);              // GCM plaintext length == ct length
    if (aes128_gcm_decrypt(key, nonce, aad, 13, in + 8, ct_len, in + 8 + ct_len, pt) != 0) return -1;
    return (int)ct_len;
}

// Finished.verify_data = PRF(master, label, SHA256(handshake transcript))[0..11].
static void tls_verify_data(const uint8_t master[48], const char* label,
                            const uint8_t* transcript, uint32_t tlen, uint8_t out[12]) {
    uint8_t hash[32];
    sha256_ctx_t c;
    sha256_init(&c);
    sha256_update(&c, transcript, tlen);
    sha256_final(&c, hash);
    tls12_prf(master, 48, label, hash, 32, out, 12);
}

// Known-answer test for the record layer + Finished verify_data, so the exact GCM nonce/AAD
// framing and the transcript-hash PRF are pinned offline before the live handshake (v5.9.54)
// relies on them. The expected fragment and verify_data were produced by independent code.
int tls_record_selftest(void) {
    int pass = 0, total = 0;

    uint8_t key[16], iv4[4], pt[16], frag[64], want[64], out[32];
    tls_hex(key, "000102030405060708090a0b0c0d0e0f", 16);
    tls_hex(iv4, "10111213", 4);
    memcpy(pt, "hello, nyxos tls", 16);                          // 16 bytes
    tls_hex(want, "000000000000000009e3145e244c52c6bffab247c55a84bbf924abd2649283e96c0f657ac55f4bb3", 40);

    int flen = tls_seal_record(key, iv4, 0, 0x17, pt, 16, frag);
    total++;
    if (flen == 40 && tls_eq(frag, want, 40)) { pass++; printf("tlsrec: GCM record seal PASS (exact wire bytes)\n"); }
    else                                        printf("tlsrec: GCM record seal FAIL\n");

    int olen = tls_open_record(key, iv4, 0, 0x17, frag, 40, out, sizeof(out));
    total++;
    if (olen == 16 && tls_eq(out, pt, 16)) { pass++; printf("tlsrec: GCM record open PASS (plaintext recovered)\n"); }
    else                                     printf("tlsrec: GCM record open FAIL\n");

    frag[12] ^= 0x01;                                            // corrupt one ciphertext byte
    total++;
    if (tls_open_record(key, iv4, 0, 0x17, frag, 40, out, sizeof(out)) == -1) { pass++; printf("tlsrec: record tamper rejected PASS\n"); }
    else                                                          printf("tlsrec: record tamper rejected FAIL\n");
    frag[12] ^= 0x01;

    uint8_t master[48], trans[32], vd[12], wc[12], ws[12];
    tls_hex(master, "3dcd0e1fa717e41ff560509c61c4039922fb8d2a7580728ef991c0748f244b0b"
                    "4125f429b4f71ed8b2084093e40953ae", 48);
    for (int i = 0; i < 32; i++) trans[i] = (uint8_t)i;
    tls_hex(wc, "82ddaf4f4cd337eecc6526d3", 12);
    tls_hex(ws, "31337bf745fbcf3fe2e1915a", 12);

    tls_verify_data(master, "client finished", trans, 32, vd);
    total++;
    if (tls_eq(vd, wc, 12)) { pass++; printf("tlsrec: client Finished verify_data PASS\n"); }
    else                      printf("tlsrec: client Finished verify_data FAIL\n");

    tls_verify_data(master, "server finished", trans, 32, vd);
    total++;
    if (tls_eq(vd, ws, 12)) { pass++; printf("tlsrec: server Finished verify_data PASS\n"); }
    else                      printf("tlsrec: server Finished verify_data FAIL\n");

    printf("tlsrec: self-test %d/%d checks passed\n", pass, total);
    return (pass == total) ? 0 : -1;
}

// Convert a DER INTEGER value (big-endian, maybe with a 0x00 sign pad or fewer than n bytes)
// into a fixed n-byte big-endian buffer (right-aligned). n is the curve field size (32 or 48).
static void der_int_to_n(uint8_t* out, uint32_t n, const uint8_t* v, uint32_t len) {
    while (len > n) { v++; len--; }
    for (uint32_t i = 0; i < n; i++) out[i] = 0;
    for (uint32_t i = 0; i < len; i++) out[n - len + i] = v[i];
}

// Verify a DER ECDSA signature (SEQUENCE{ INTEGER r, INTEGER s }) over `hash` (hlen bytes) using
// the leaf certificate's EC public key. hlen selects the curve: 32 -> P-256 (65-byte point / SHA-256),
// 48 -> P-384 (97-byte point / SHA-384). Returns 0 = valid, -1 = invalid, -2 = couldn't check (the
// cert's key isn't an EC point of the matching size, or the signature won't parse).
static int tls_ecdsa_leaf_verify(const uint8_t* leaf, uint32_t leaflen,
                                 const uint8_t* hash, int hlen,
                                 const uint8_t* sig, uint32_t siglen) {
    const uint8_t* Q; uint32_t Qlen;
    if (der_x509_ec_pubkey(leaf, leaflen, &Q, &Qlen) != 0 || Q[0] != 0x04) return -2;
    if (Qlen != (uint32_t)(1 + 2 * hlen)) return -2;           // point size must match the hash/curve
    der_t sd = { sig, sig + siglen }, sq;
    uint8_t tg; const uint8_t* iv; uint32_t il; uint8_t rr[48], ss[48];
    if (der_enter(&sd, 0x30, &sq) != 0) return -2;
    if (der_read(&sq, &tg, &iv, &il) != 0 || tg != 0x02) return -2;
    der_int_to_n(rr, (uint32_t)hlen, iv, il);
    if (der_read(&sq, &tg, &iv, &il) != 0 || tg != 0x02) return -2;
    der_int_to_n(ss, (uint32_t)hlen, iv, il);
    return (hlen == 32) ? p256_ecdsa_verify(Q + 1, Q + 33, hash, rr, ss)
                        : p384_ecdsa_verify(Q + 1, Q + 49, hash, rr, ss);
}

// ECDSA-P384 ServerKeyExchange verification KAT (the `skp384test` command). Extracts the P-384 key
// from a real leaf cert, then checks a known ECDSA-P384/SHA384 signature over a known 48-byte digest
// (valid accepted, one-bit-tampered digest rejected). Vectors generated by scratchpad/gen_ske384.py.
static const uint8_t SKE384_LEAF[365] = {
    0x30,0x82,0x01,0x69,0x30,0x81,0xf0,0xa0,0x03,0x02,0x01,0x02,
    0x02,0x05,0x01,0x23,0x45,0x67,0x89,0x30,0x0a,0x06,0x08,0x2a,
    0x86,0x48,0xce,0x3d,0x04,0x03,0x03,0x30,0x1e,0x31,0x1c,0x30,
    0x1a,0x06,0x03,0x55,0x04,0x03,0x0c,0x13,0x6e,0x79,0x78,0x6f,
    0x73,0x2d,0x70,0x33,0x38,0x34,0x2d,0x73,0x6b,0x65,0x2d,0x74,
    0x65,0x73,0x74,0x30,0x1e,0x17,0x0d,0x32,0x30,0x30,0x31,0x30,
    0x31,0x30,0x30,0x30,0x30,0x30,0x30,0x5a,0x17,0x0d,0x33,0x35,
    0x30,0x31,0x30,0x31,0x30,0x30,0x30,0x30,0x30,0x30,0x5a,0x30,
    0x1e,0x31,0x1c,0x30,0x1a,0x06,0x03,0x55,0x04,0x03,0x0c,0x13,
    0x6e,0x79,0x78,0x6f,0x73,0x2d,0x70,0x33,0x38,0x34,0x2d,0x73,
    0x6b,0x65,0x2d,0x74,0x65,0x73,0x74,0x30,0x76,0x30,0x10,0x06,
    0x07,0x2a,0x86,0x48,0xce,0x3d,0x02,0x01,0x06,0x05,0x2b,0x81,
    0x04,0x00,0x22,0x03,0x62,0x00,0x04,0x3c,0x32,0x86,0x58,0xa6,
    0x4e,0x30,0x61,0x45,0x2f,0xd0,0x62,0xb2,0x26,0xdb,0xba,0x9f,
    0x67,0xda,0x57,0xc5,0x07,0x72,0x5a,0x07,0x69,0x20,0x50,0x4e,
    0xd6,0xb6,0x57,0xf1,0xbd,0xeb,0x46,0xa5,0x39,0x16,0x4a,0x53,
    0x9e,0x1c,0xe7,0x35,0xd7,0x4e,0xc8,0xab,0x3e,0x15,0xeb,0xb8,
    0xe4,0x47,0xca,0xfb,0xaa,0xa2,0x20,0xca,0x5c,0x54,0x09,0x8b,
    0x02,0x7d,0x1e,0x1d,0xbf,0xeb,0xbd,0x73,0x28,0xa8,0x3c,0x6e,
    0xda,0x47,0xa2,0x82,0x41,0x92,0xb4,0xda,0x1b,0x56,0xf9,0xa3,
    0x8b,0x71,0x94,0xb9,0x33,0xe1,0xf8,0x30,0x0a,0x06,0x08,0x2a,
    0x86,0x48,0xce,0x3d,0x04,0x03,0x03,0x03,0x68,0x00,0x30,0x65,
    0x02,0x30,0x76,0x54,0x99,0xdf,0xc5,0xda,0x74,0x4d,0x5a,0xa6,
    0x38,0x51,0xd6,0xf8,0xd6,0xe9,0xc8,0xe4,0xdd,0xf6,0x1f,0xeb,
    0xac,0x6b,0x9b,0x55,0xb6,0xb8,0xdb,0x38,0x67,0xbd,0x38,0xe6,
    0x3e,0x5e,0xc0,0xaa,0x95,0x01,0x38,0x58,0x6c,0x58,0x7b,0x78,
    0x64,0x8c,0x02,0x31,0x00,0xf2,0xd4,0xcc,0x92,0x24,0x7b,0xf5,
    0xa9,0x58,0x8a,0x56,0xab,0x43,0xaf,0xf5,0x85,0x4d,0x81,0x03,
    0xc9,0xa8,0x24,0x42,0xa7,0x8f,0xdc,0x00,0x42,0x4f,0x24,0x6d,
    0x11,0x38,0x7b,0x47,0x25,0x91,0xfc,0x33,0x95,0x3c,0xc0,0xed,
    0x54,0x5a,0x05,0x26,0xdf,
};
static const uint8_t SKE384_HASH[48] = {
    0x46,0x51,0x76,0xc5,0x10,0xf1,0x96,0x01,0xdd,0x19,0x6c,0x66,
    0x63,0x15,0x6b,0xfc,0xdb,0x4b,0x2d,0xfd,0x63,0xfe,0x71,0x7b,
    0x44,0x6b,0x12,0xa2,0xe8,0x5c,0x67,0xa7,0x3d,0xda,0xc7,0x91,
    0x3d,0x79,0xa1,0x46,0x51,0x34,0x58,0x4c,0x32,0xc3,0xbc,0x26,
};
static const uint8_t SKE384_SIG[102] = {
    0x30,0x64,0x02,0x30,0x5b,0xbd,0x66,0xce,0x46,0x94,0x17,0x18,
    0x49,0x94,0xda,0x0d,0x49,0x4d,0x50,0x6d,0xd8,0x9b,0x00,0x90,
    0xc1,0xef,0x8f,0x0e,0x74,0x39,0xa8,0x0e,0xca,0xfd,0x1c,0xa4,
    0x90,0x94,0x3f,0x9f,0x7b,0x06,0xa3,0x35,0x72,0x2f,0x59,0x5a,
    0x81,0x08,0x30,0x49,0x02,0x30,0x76,0x17,0xab,0x2f,0xbb,0xa6,
    0x11,0x08,0x5f,0xab,0xb0,0xbc,0xad,0x15,0x19,0x58,0xf6,0x9f,
    0xe0,0x58,0x2f,0xd7,0x22,0x2c,0x76,0xab,0xdc,0x11,0x2a,0x57,
    0x66,0xe5,0x6c,0xfa,0xe0,0x37,0xef,0xf2,0x24,0x51,0xf0,0xa6,
    0xab,0xc2,0x8d,0x76,0xca,0x87,
};
int tls_ske_p384_selftest(void) {
    const uint8_t* Q; uint32_t Qlen;
    int e1 = (der_x509_ec_pubkey(SKE384_LEAF, sizeof(SKE384_LEAF), &Q, &Qlen) == 0 && Qlen == 97 && Q[0] == 0x04);
    int e2 = (tls_ecdsa_leaf_verify(SKE384_LEAF, sizeof(SKE384_LEAF), SKE384_HASH, 48,
                                    SKE384_SIG, sizeof(SKE384_SIG)) == 0);
    uint8_t bad[48]; for (int i = 0; i < 48; i++) bad[i] = SKE384_HASH[i]; bad[10] ^= 0x01;
    int e3 = (tls_ecdsa_leaf_verify(SKE384_LEAF, sizeof(SKE384_LEAF), bad, 48,
                                    SKE384_SIG, sizeof(SKE384_SIG)) == -1);
    int pass = e1 + e2 + e3;
    printf("ske-p384: P-384 pubkey-extract %s, valid-sig accepted %s, tampered rejected %s (%d/3)\n",
           e1 ? "PASS" : "FAIL", e2 ? "PASS" : "FAIL", e3 ? "PASS" : "FAIL", pass);
    return (pass == 3) ? 0 : -1;
}

// Full https request: TLS 1.2 handshake with host:443, send <method> <path> (with an optional
// form body for POST), decrypt the response into out[cap]. Returns the decrypted length (>= 0)
// or -1 on failure. `verbose` gates the step-by-step diagnostics (the `tls` command sets it).
int tls_https_request(const char* host, const char* path, const char* method,
                      const uint8_t* body, uint32_t body_len, int iface_idx,
                      uint8_t* out, uint32_t cap, int verbose) {
    if (!method) method = "GET";
    uint32_t ip = dns_resolve(host, iface_idx);
    if (!ip) { TLSP("TLS: cannot resolve %s\n", host); return -1; }

    int conn = tcp_connect(ip, 443, 12400);
    if (conn < 0) { TLSP("TLS: connect() to %s:443 failed\n", host); return -1; }

    uint32_t dl = get_ticks() + 5000;              // drive the TCP 3-way handshake
    while ((int32_t)(get_ticks() - dl) < 0) {
        kernel_poll_net();
        if (tcp_state(conn) == TCP_STATE_ESTABLISHED) break;
    }
    if (tcp_state(conn) != TCP_STATE_ESTABLISHED) {
        TLSP("TLS: TCP handshake to %s:443 timed out\n", host); tcp_close(conn); return -1;
    }
    TLSP("TLS: TCP connected to %s:443 — sending ClientHello...\n", host);

    int chlen = build_client_hello(host);
    if (tcp_send(conn, tls_hs, chlen) < 0) { TLSP("TLS: send failed\n"); tcp_close(conn); return -1; }

    uint32_t total = 0, start = get_ticks(), last = start;   // read the server flight
    for (;;) {
        kernel_poll_net();
        int n = tcp_recv(conn, tls_rx + total, (uint32_t)sizeof(tls_rx) - total - 1);
        if (n > 0) { total += (uint32_t)n; last = get_ticks(); if (total >= sizeof(tls_rx) - 1) break; }
        else if (n < 0) break;                     // peer closed
        uint32_t now = get_ticks();
        if (total == 0) { if ((int32_t)(now - (start + 5000)) >= 0) break; }
        else if ((int32_t)(now - (last + 1200)) >= 0) break;   // stream went quiet
        if ((int32_t)(now - (start + 12000)) >= 0) break;
    }
    if (total == 0) { TLSP("TLS: no response from %s\n", host); tcp_close(conn); return -1; }

    // Walk TLS records: gather handshake bytes, catch an Alert.
    uint32_t hn = 0, o = 0; int alert = 0, alert_level = 0, alert_desc = 0;
    while (o + 5 <= total) {
        uint8_t rtype = tls_rx[o];
        uint16_t rlen = (uint16_t)((tls_rx[o+3] << 8) | tls_rx[o+4]);
        if (o + 5 + rlen > total) break;
        if (rtype == 22) {                          // handshake
            if (hn + rlen <= sizeof(tls_hd)) { memcpy(tls_hd + hn, tls_rx + o + 5, rlen); hn += rlen; }
        } else if (rtype == 21 && rlen >= 2) {      // alert
            alert = 1; alert_level = tls_rx[o+5]; alert_desc = tls_rx[o+6];
        }
        o += 5 + rlen;
    }

    if (alert) {
        TLSP("TLS: %s sent an Alert (level %d, description %d) — handshake refused\n",
               host, alert_level, alert_desc);
        tcp_close(conn); return -1;
    }

    // Parse the handshake messages.
    uint32_t h = 0; uint16_t cipher = 0;
    int got_sh = 0, got_cert = 0, ncerts = 0; uint32_t cert0 = 0; int got_ske = 0, got_shd = 0;
    uint16_t ske_curve = 0; int ske_pub_len = 0; uint8_t ske_pub[128];   // server's ECDHE public key (x25519/P-256/P-384)
    const uint8_t* leaf_ptr = 0;                                        // leaf certificate bytes (in tls_hd)
    const uint8_t* chain_ptr[8]; uint32_t chain_len[8]; int chain_n = 0;  // the full cert chain
    uint32_t ske_params_len = 0, ske_sig_len = 0; uint16_t ske_sig_alg = 0;
    while (h + 4 <= hn) {
        uint8_t mt = tls_hd[h];
        uint32_t mlen = (uint32_t)((tls_hd[h+1] << 16) | (tls_hd[h+2] << 8) | tls_hd[h+3]);
        if (h + 4 + mlen > hn) break;
        uint8_t* m = tls_hd + h + 4;
        if (mt == 2 && mlen >= 38) {                // ServerHello
            got_sh = 1;
            memcpy(tls_server_random, m + 2, 32);    // 2-byte version, then the 32-byte random
            uint32_t q = 2 + 32;                     // version + random
            uint8_t sidl = m[q]; q += 1 + sidl;      // session id
            if (q + 1 < mlen) cipher = (uint16_t)((m[q] << 8) | m[q+1]);
        } else if (mt == 11 && mlen >= 3) {         // Certificate
            got_cert = 1;
            uint32_t clen = (uint32_t)((m[0] << 16) | (m[1] << 8) | m[2]);
            uint32_t cq = 3;
            while (cq + 3 <= 3 + clen && cq + 3 <= mlen) {
                uint32_t one = (uint32_t)((m[cq] << 16) | (m[cq+1] << 8) | m[cq+2]);
                // Record the leaf ONLY if its claimed length fits the message — same bound
                // the chain[] entry below uses. Without it, a malformed Certificate with a
                // huge 24-bit leaf length left cert0 huge and leaf_ptr into tls_hd[], and the
                // SKE-signature path later passed (leaf_ptr, cert0) to der_x509_pubkey /
                // tls_ecdsa_leaf_verify as the buffer bound — an out-of-bounds read past the
                // 16 KB handshake buffer driven entirely by attacker-supplied lengths.
                if (ncerts == 0 && cq + 3 + one <= mlen) { cert0 = one; leaf_ptr = m + cq + 3; }   // the leaf cert
                if (chain_n < 8 && cq + 3 + one <= mlen) {                 // record the whole chain
                    chain_ptr[chain_n] = m + cq + 3; chain_len[chain_n] = one; chain_n++;
                }
                ncerts++; cq += 3 + one;
            }
        } else if (mt == 12) {                      // ServerKeyExchange (ECDHE parameters)
            got_ske = 1;
            // ServerECDHParams: curve_type(1). named_curve => named_curve(2) + point_len(1) + point.
            if (mlen >= 4 && m[0] == 3) {           // 3 = named_curve
                ske_curve = (uint16_t)((m[1] << 8) | m[2]);
                uint8_t plen = m[3];
                if ((uint32_t)(4 + plen) <= mlen && plen <= sizeof(ske_pub)) {
                    ske_pub_len = plen;
                    memcpy(ske_pub, m + 4, plen);
                    uint32_t pl = 4 + (uint32_t)plen;          // ServerECDHParams = the signed data
                    if (pl <= sizeof(tls_ske_params)) { memcpy(tls_ske_params, m, pl); ske_params_len = pl; }
                    if (pl + 4 <= mlen) {                      // SignatureAndHashAlgorithm(2) + length(2) + signature
                        ske_sig_alg = (uint16_t)((m[pl] << 8) | m[pl+1]);
                        uint32_t sl = (uint32_t)((m[pl+2] << 8) | m[pl+3]);
                        if (pl + 4 + sl <= mlen && sl <= sizeof(tls_ske_sig)) { memcpy(tls_ske_sig, m + pl + 4, sl); ske_sig_len = sl; }
                    }
                }
            }
        }
        else if (mt == 14) { got_shd = 1; }         // ServerHelloDone
        h += 4 + mlen;
    }

    if (!got_sh) { TLSP("TLS: no ServerHello in %u bytes from %s\n", total, host); tcp_close(conn); return -1; }
    TLSP("TLS: ServerHello OK — negotiated cipher suite 0x%x\n", cipher);
    if (got_cert) TLSP("TLS: Certificate chain — %d cert(s), leaf %u bytes\n", ncerts, (unsigned)cert0);
    if (got_ske)  TLSP("TLS: ServerKeyExchange present (ephemeral ECDHE key, curve 0x%x)\n", ske_curve);
    if (got_shd)  TLSP("TLS: ServerHelloDone — handshake start complete.\n");

    // ---- Verify the certificate chain to a pinned trust anchor ----
    // Each certificate is verified under the next one's public key (ECDSA-P256/P384 or RSA, by its
    // signatureAlgorithm), and the topmost must carry NyxOS's pinned root key. A broken link means
    // tampering/MITM and aborts the handshake. An unanchored chain is refused by
    // default; only the explicit session development override permits it.
    if (!got_cert || chain_n == 0) {
        TLSP("TLS: missing or empty Certificate — refusing the handshake\n");
        tcp_close(conn); return -1;
    }
    if (got_cert && chain_n > 0) {
        char cmsg[96];
        int trust = x509_verify_chain(chain_ptr, chain_len, chain_n, cmsg, sizeof(cmsg));
        if (trust == X509_OK) {
            TLSP("TLS: certificate chain VERIFIED — %s (%d link%s)\n",
                 cmsg, chain_n - 1, (chain_n - 1) == 1 ? "" : "s");
        } else if (trust == X509_FORGED) {
            TLSP("TLS: certificate chain INVALID — %s — aborting (possible MITM)\n", cmsg);
            tcp_close(conn); return -1;
        } else {
            TLSP("TLS: certificate chain is not anchored: %s\n", cmsg);
        }

        // Leaf hostname (subjectAltName) + validity window (v5.9.66).
        int hm = x509_check_host(chain_ptr[0], chain_len[0], host);
        if (hm == 0)      TLSP("TLS: leaf certificate covers host '%s' (subjectAltName match)\n", host);
        else if (hm == 1) TLSP("TLS: leaf certificate does NOT cover host '%s' — wrong-host certificate\n", host);
        else              TLSP("TLS: leaf certificate has no usable subjectAltName — host not checked\n");

        int vd = x509_check_validity(chain_ptr[0], chain_len[0]);
        if (vd == 0)      TLSP("TLS: leaf certificate is within its validity period\n");
        else if (vd == 1) TLSP("TLS: leaf certificate is NOT yet valid (notBefore is in the future)\n");
        else if (vd == 2) TLSP("TLS: leaf certificate has EXPIRED (past notAfter)\n");
        else              TLSP("TLS: leaf certificate validity dates could not be parsed\n");

        // Authentication failures require an explicit development override.
        if (tls_strict && (trust != X509_OK || hm != 0 || vd != 0)) {
            TLSP("TLS: STRICT MODE — refusing %s (%s%s%suntrusted certificate)\n", host,
                 (trust != X509_OK) ? "not anchored to a trusted root; " : "",
                 (hm != 0) ? "hostname not covered; " : "",
                 (vd != 0) ? "not within validity dates; " : "");
            tcp_close(conn); return -1;
        }
    }

    // ECDHE key agreement + the TLS 1.2 key schedule (v5.9.51; P-256 v5.9.70; P-384 v5.9.80).
    if ((ske_curve == 0x001D && ske_pub_len == 32) ||                        // x25519
        (ske_curve == 0x0017 && ske_pub_len == 65 && ske_pub[0] == 0x04) ||  // secp256r1 (P-256)
        (ske_curve == 0x0018 && ske_pub_len == 97 && ske_pub[0] == 0x04)) {  // secp384r1 (P-384)
        uint8_t eph_pub[97]; uint32_t eph_pub_len; uint8_t premaster[48]; uint32_t premaster_len;
        const char* curve_name;
        if (ske_curve == 0x001D) {                                           // x25519 (RFC 7748)
            uint8_t eph_priv[32];
            csprng_bytes(eph_priv, 32);                                      // strong ephemeral key
            x25519_base(eph_pub, eph_priv);                                  // our public key
            x25519(premaster, eph_priv, ske_pub);                           // shared secret
            eph_pub_len = 32; premaster_len = 32; curve_name = "x25519";
        } else if (ske_curve == 0x0017) {                                    // NIST P-256 ECDHE
            uint8_t eph_priv[32], sx[32], sy[32];
            csprng_bytes(eph_priv, 32);                                     // ephemeral scalar d
            eph_pub[0] = 0x04;
            p256_base_scalar(eph_priv, eph_pub + 1, eph_pub + 33);          // our point = d*G (0x04||X||Y)
            p256_point_scalar(eph_priv, ske_pub + 1, ske_pub + 33, sx, sy); // shared point = d*serverPub
            memcpy(premaster, sx, 32);                                      // pre-master = its X coordinate
            eph_pub_len = 65; premaster_len = 32; curve_name = "secp256r1 (P-256)";
        } else {                                                            // NIST P-384 ECDHE
            uint8_t eph_priv[48], sx[48], sy[48];
            csprng_bytes(eph_priv, 48);                                     // ephemeral scalar d
            eph_pub[0] = 0x04;
            p384_base_scalar(eph_priv, eph_pub + 1, eph_pub + 49);          // our point = d*G (0x04||X||Y)
            p384_point_scalar(eph_priv, ske_pub + 1, ske_pub + 49, sx, sy); // shared point = d*serverPub
            memcpy(premaster, sx, 48);                                      // pre-master = its X coordinate
            eph_pub_len = 97; premaster_len = 48; curve_name = "secp384r1 (P-384)";
        }

        uint8_t seed[64], master[48], keyblock[40];
        memcpy(seed, tls_client_random, 32); memcpy(seed + 32, tls_server_random, 32);
        tls12_prf(premaster, premaster_len, "master secret", seed, 64, master, 48);
        memcpy(seed, tls_server_random, 32); memcpy(seed + 32, tls_client_random, 32);
        tls12_prf(master, 48, "key expansion", seed, 64, keyblock, 40);   // client_key|server_key|client_iv|server_iv
        const uint8_t* ckey = keyblock;              const uint8_t* skey = keyblock + 16;
        const uint8_t* civ  = keyblock + 32;         const uint8_t* siv  = keyblock + 36;
        TLSP("TLS: ECDHE(%s) — pre-master secret computed; master secret + AES-128-GCM keys derived\n", curve_name);

        // ---- Verify the ServerKeyExchange signature against the leaf certificate ----------
        // Proves the ECDHE parameters were signed by the private key matching the leaf certificate:
        // ECDSA-P256/SHA-256 (v5.9.61), ECDSA-P384/SHA-384 (v5.9.79), RSA-PKCS1-SHA256 (v5.9.71),
        // RSA-PSS-SHA256 (v5.9.74) or RSA-PKCS1-SHA512 (v5.9.92) — every common SKE scheme. The signed
        // data is HASH(client_random ‖ server_random ‖ ServerECDHParams), the hash chosen by the
        // signature algorithm. (Authenticity of the key exchange; the certificate's chain to a trusted
        // root is checked separately, above.)
        int ske_ok = 0;
        if (leaf_ptr && ske_sig_len &&
            (ske_sig_alg == 0x0403 || ske_sig_alg == 0x0503 || ske_sig_alg == 0x0401 ||
             ske_sig_alg == 0x0804 || ske_sig_alg == 0x0601)) {
            int checked = -2; const char* scheme = "?";
            if (ske_sig_alg == 0x0503) {                               // ECDSA-P384 over SHA-384
                scheme = "ECDSA-P384";
                uint8_t e[48]; sha512_ctx_t hc; sha384_init(&hc);
                sha512_update(&hc, tls_client_random, 32);
                sha512_update(&hc, tls_server_random, 32);
                sha512_update(&hc, tls_ske_params, ske_params_len);
                sha384_final(&hc, e);
                checked = tls_ecdsa_leaf_verify(leaf_ptr, cert0, e, 48, tls_ske_sig, ske_sig_len);
            } else if (ske_sig_alg == 0x0601) {                        // RSA-PKCS1 over SHA-512
                scheme = "RSA-PKCS1-SHA512";
                uint8_t e[64]; sha512_ctx_t hc; sha512_init(&hc);
                sha512_update(&hc, tls_client_random, 32);
                sha512_update(&hc, tls_server_random, 32);
                sha512_update(&hc, tls_ske_params, ske_params_len);
                sha512_final(&hc, e);
                der_pubkey_t pk;
                if (der_x509_pubkey(leaf_ptr, cert0, &pk) == 0 && pk.type == DER_KEY_RSA) {
                    const uint8_t* N = pk.rsa_n; uint32_t Nl = pk.rsa_n_len;
                    while (Nl > 1 && N[0] == 0x00) { N++; Nl--; }   // strip DER INTEGER leading zero
                    uint64_t ev = 0;
                    for (uint32_t k = 0; k < pk.rsa_e_len && k < 8; k++) ev = (ev << 8) | pk.rsa_e[k];
                    checked = rsa_pkcs1_sha512_verify(N, Nl, ev, tls_ske_sig, ske_sig_len, e);
                }
            } else {
                uint8_t e[32]; sha256_ctx_t hc; sha256_init(&hc);
                sha256_update(&hc, tls_client_random, 32);
                sha256_update(&hc, tls_server_random, 32);
                sha256_update(&hc, tls_ske_params, ske_params_len);
                sha256_final(&hc, e);
                if (ske_sig_alg == 0x0403) {                           // ECDSA-P256 over SHA-256
                    scheme = "ECDSA-P256";
                    checked = tls_ecdsa_leaf_verify(leaf_ptr, cert0, e, 32, tls_ske_sig, ske_sig_len);
                } else {                                               // RSA: PKCS1 (0x0401) or PSS (0x0804)
                    int pss = (ske_sig_alg == 0x0804);
                    scheme = pss ? "RSA-PSS" : "RSA-PKCS1";
                    der_pubkey_t pk;
                    if (der_x509_pubkey(leaf_ptr, cert0, &pk) == 0 && pk.type == DER_KEY_RSA) {
                        const uint8_t* N = pk.rsa_n; uint32_t Nl = pk.rsa_n_len;
                        while (Nl > 1 && N[0] == 0x00) { N++; Nl--; }   // strip DER INTEGER leading zero
                        uint64_t ev = 0;
                        for (uint32_t k = 0; k < pk.rsa_e_len && k < 8; k++) ev = (ev << 8) | pk.rsa_e[k];
                        checked = pss ? rsa_pss_sha256_verify(N, Nl, ev, tls_ske_sig, ske_sig_len, e)
                                      : rsa_pkcs1_sha256_verify(N, Nl, ev, tls_ske_sig, ske_sig_len, e);
                    }
                }
            }
            if (checked == 0) {
                ske_ok = 1;
                TLSP("TLS: ServerKeyExchange signature VERIFIED against the leaf certificate (%s)\n", scheme);
            } else if (checked == -1) {
                TLSP("TLS: ServerKeyExchange signature INVALID — aborting the handshake (possible MITM)\n");
                tcp_close(conn); return -1;
            } else {
                TLSP("TLS: ServerKeyExchange signature not checked (cert key/type mismatch) — proceeding unverified\n");
            }
        } else if (got_ske) {
            TLSP("TLS: ServerKeyExchange sig alg 0x%x not supported for verification — proceeding unverified\n", ske_sig_alg);
        }
        // Strict mode (v5.9.74): the ServerKeyExchange signature must also be verified — otherwise
        // an active MITM could substitute its own ECDHE parameters while presenting the real cert.
        if (tls_strict && got_ske && !ske_ok) {
            TLSP("TLS: STRICT MODE — refusing %s (ServerKeyExchange signature not verified)\n", host);
            tcp_close(conn); return -1;
        }

        // ---- Finished exchange (v5.9.54) ------------------------------------------------
        // The transcript is every handshake-layer message, in order, with no record headers:
        // ClientHello || ServerHello..ServerHelloDone || our ClientKeyExchange (+ our Finished).
        if ((uint32_t)(chlen - 5) + hn + 110 + 16 > sizeof(tls_ts)) {
            TLSP("TLS: handshake transcript too large to buffer — skipping Finished\n");
            tcp_close(conn); return -1;
        }
        uint32_t tn = 0;
        memcpy(tls_ts + tn, tls_hs + 5, (uint32_t)chlen - 5); tn += (uint32_t)chlen - 5;   // ClientHello
        memcpy(tls_ts + tn, tls_hd, hn); tn += hn;                                         // server flight

        uint8_t cke[110];                            // ClientKeyExchange (type 16): pointlen(1) + point (P-384 point = 97)
        uint32_t cke_body = 1 + eph_pub_len;         // the ECDH public point, length-prefixed
        uint32_t cke_len  = 4 + cke_body;            // 4-byte handshake header + body
        cke[0] = 0x10; cke[1] = 0; cke[2] = (uint8_t)(cke_body >> 8); cke[3] = (uint8_t)cke_body;
        cke[4] = (uint8_t)eph_pub_len; memcpy(cke + 5, eph_pub, eph_pub_len);
        memcpy(tls_ts + tn, cke, cke_len); tn += cke_len;

        uint8_t cvd[12], cfin[16];                   // client Finished = verify_data over the transcript so far
        tls_verify_data(master, "client finished", tls_ts, tn, cvd);
        cfin[0] = 0x14; cfin[1] = 0; cfin[2] = 0; cfin[3] = 0x0c; memcpy(cfin + 4, cvd, 12);
        memcpy(tls_ts + tn, cfin, 16); tn += 16;

        uint8_t svd_exp[12];                         // the value the server's Finished must carry
        tls_verify_data(master, "server finished", tls_ts, tn, svd_exp);

        uint8_t f2[220]; uint32_t on = 0;            // flight 2: ClientKeyExchange + ChangeCipherSpec + Finished
        f2[on++] = 0x16; f2[on++] = 0x03; f2[on++] = 0x03; f2[on++] = (uint8_t)(cke_len >> 8); f2[on++] = (uint8_t)cke_len;   // CKE record
        memcpy(f2 + on, cke, cke_len); on += cke_len;
        f2[on++] = 0x14; f2[on++] = 0x03; f2[on++] = 0x03; f2[on++] = 0x00; f2[on++] = 0x01; f2[on++] = 0x01;  // CCS
        uint8_t sealed[64];
        int sl = tls_seal_record(ckey, civ, 0, 0x16, cfin, 16, sealed);   // GCM-seal the Finished, client seq 0
        f2[on++] = 0x16; f2[on++] = 0x03; f2[on++] = 0x03; f2[on++] = (uint8_t)(sl >> 8); f2[on++] = (uint8_t)sl;
        memcpy(f2 + on, sealed, (uint32_t)sl); on += (uint32_t)sl;

        TLSP("TLS: sending ClientKeyExchange + ChangeCipherSpec + encrypted Finished...\n");
        if (tcp_send(conn, f2, (int)on) < 0) { TLSP("TLS: flight-2 send failed\n"); tcp_close(conn); return -1; }

        uint32_t t2 = 0, s2 = get_ticks(), l2 = s2;  // read the server's ChangeCipherSpec + Finished
        for (;;) {
            kernel_poll_net();
            int n = tcp_recv(conn, tls_rx + t2, (uint32_t)sizeof(tls_rx) - t2 - 1);
            if (n > 0) { t2 += (uint32_t)n; l2 = get_ticks(); }
            else if (n < 0) break;
            uint32_t now = get_ticks();
            if (t2 == 0) { if ((int32_t)(now - (s2 + 5000)) >= 0) break; }
            else if ((int32_t)(now - (l2 + 1000)) >= 0) break;
            if ((int32_t)(now - (s2 + 10000)) >= 0) break;
        }

        int verified = 0, sawccs = 0, s_alert = 0, s_desc = 0;
        uint32_t pp = 0;
        while (pp + 5 <= t2) {
            uint8_t rt = tls_rx[pp];
            uint16_t rl = (uint16_t)((tls_rx[pp+3] << 8) | tls_rx[pp+4]);
            if (pp + 5 + rl > t2) break;
            if (rt == 20) { sawccs = 1; }                       // ChangeCipherSpec
            else if (rt == 21 && rl >= 2 && !sawccs) { s_alert = 1; s_desc = tls_rx[pp+6]; }  // plaintext alert
            else if (rt == 22 && sawccs) {                      // the encrypted Finished
                uint8_t fin[64];
                int pl = tls_open_record(skey, siv, 0, 0x16, tls_rx + pp + 5, rl, fin, sizeof(fin));   // server seq 0
                if (pl == 16 && fin[0] == 0x14) {
                    if (tls_eq(fin + 4, svd_exp, 12)) verified = 1;
                    else TLSP("TLS: server Finished verify_data MISMATCH — transcript/keys disagree\n");
                } else if (pl < 0) {
                    TLSP("TLS: could not decrypt the server Finished (bad key or tag)\n");
                }
            }
            pp += 5 + rl;
        }
        if (s_alert) {
            TLSP("TLS: %s sent an Alert (description %d) after our Finished — handshake rejected\n", host, s_desc);
            tcp_close(conn); return -1;
        }
        if (!verified) {
            TLSP("TLS: handshake did not complete — no verified server Finished received\n");
            tcp_close(conn); return -1;
        }
        TLSP("TLS: *** HANDSHAKE COMPLETE — server Finished verified; encrypted channel established with %s ***\n", host);

        // ---- Encrypted https GET over the established channel ---------------------------
        // Our client Finished was record seq 0, so this GET is client seq 1; the server's
        // Finished was seq 0, so its first application-data reply is server seq 1.
        uint8_t req[640]; uint32_t reqn = 0;
        int is_post = (method[0] == 'P' || method[0] == 'p');
        const char* r_ver    = " HTTP/1.1\r\nHost: ";
        const char* r_common = "\r\nConnection: close\r\nUser-Agent: NyxOS-Selene\r\n";
        for (const char* s = method;   *s; s++) req[reqn++] = (uint8_t)*s;
        req[reqn++] = ' ';
        for (const char* s = path;     *s && reqn < sizeof(req) - 300; s++) req[reqn++] = (uint8_t)*s;
        for (const char* s = r_ver;    *s; s++) req[reqn++] = (uint8_t)*s;
        for (const char* s = host;     *s && reqn < sizeof(req) - 200; s++) req[reqn++] = (uint8_t)*s;
        for (const char* s = r_common; *s; s++) req[reqn++] = (uint8_t)*s;
        if (is_post && body && body_len > 0) {          // form body: add the content headers + data
            char clh[96];
            int cn = snprintf(clh, sizeof(clh),
                "Content-Type: application/x-www-form-urlencoded\r\nContent-Length: %u\r\n\r\n", body_len);
            for (int k = 0; k < cn && reqn < sizeof(req); k++) req[reqn++] = (uint8_t)clh[k];
            for (uint32_t k = 0; k < body_len && reqn < sizeof(req); k++) req[reqn++] = body[k];
        } else {
            req[reqn++] = '\r'; req[reqn++] = '\n';      // end of the (bodyless) header block
        }

        uint8_t grec[5 + sizeof(req) + 24];             // one application-data record (type 0x17)
        int gl = tls_seal_record(ckey, civ, 1, 0x17, req, reqn, grec + 5);   // client seq 1
        grec[0] = 0x17; grec[1] = 0x03; grec[2] = 0x03; grec[3] = (uint8_t)(gl >> 8); grec[4] = (uint8_t)gl;
        TLSP("TLS: sending an encrypted \"%s %s\" over the channel...\n", method, path);
        if (tcp_send(conn, grec, 5 + gl) < 0) { TLSP("TLS: request send failed\n"); tcp_close(conn); return -1; }

        // Read and decrypt the response by STREAMING: decrypt each complete TLS record as it
        // arrives, then compact tls_rx. The page is bounded by `cap` (the caller's buffer — up to
        // 64 KB), not by tls_rx, which now only has to hold a single record at a time. (Before,
        // the whole encrypted response had to fit in tls_rx, so pages over ~16 KB were truncated.)
        uint32_t r3 = 0, s3 = get_ticks(), l3 = s3;
        uint32_t pn = 0; uint64_t sseq = 1; int done = 0;
        while (!done) {
            kernel_poll_net();
            int n = tcp_recv(conn, tls_rx + r3, (uint32_t)sizeof(tls_rx) - r3);
            if (n > 0) {
                r3 += (uint32_t)n; l3 = get_ticks();
                uint32_t q = 0;
                while (q + 5 <= r3) {                 // drain every complete record at the front
                    uint8_t rt = tls_rx[q];
                    uint32_t rl2 = (uint32_t)((tls_rx[q+3] << 8) | tls_rx[q+4]);
                    if (rl2 > sizeof(tls_rx) - 5) { done = 1; break; }   // oversize/illegal record
                    if (q + 5 + rl2 > r3) break;                         // not fully arrived yet
                    if (rt == 0x17) {                                    // application data
                        if (pn + rl2 > cap) { done = 1; break; }         // caller buffer full
                        int pl = tls_open_record(skey, siv, sseq, 0x17, tls_rx + q + 5, rl2, out + pn, cap - pn);
                        if (pl < 0) { TLSP("TLS: could not decrypt application-data record %u\n", (unsigned)sseq); done = 1; break; }
                        pn += (uint32_t)pl; sseq++;
                    } else if (rt == 0x15) { done = 1; break; }          // close_notify — end of stream
                    q += 5 + rl2;
                }
                if (q > 0) { for (uint32_t i = q; i < r3; i++) tls_rx[i - q] = tls_rx[i]; r3 -= q; }  // compact
            }
            else if (n < 0) break;                   // server closed (Connection: close)
            uint32_t now = get_ticks();
            if (pn == 0 && r3 == 0) { if ((int32_t)(now - (s3 + 8000)) >= 0) break; }   // nothing yet
            else if ((int32_t)(now - (l3 + 1500)) >= 0) break;                          // idle after data
            if ((int32_t)(now - (s3 + 20000)) >= 0) break;                              // hard cap
        }
        tcp_close(conn);

        if (pn == 0) { TLSP("TLS: no application data decrypted\n"); return -1; }
        if (verbose) {
            TLSP("TLS: decrypted %u bytes of https response:\n", pn);
            printf("------------------------------------------------------------\n");
            tls_print_text(out, pn, 480);
            printf("------------------------------------------------------------\n");
            TLSP("TLS: *** https fetch over TLS succeeded for %s ***\n", host);
            TLSP("TLS: %s\n", tls_strict
                 ? "Supported server authentication checks enforced."
                 : "Development override active: server identity is not assured.");
        }
        return (int)pn;
    } else if (got_ske) {
        TLSP("TLS: server chose curve 0x%x; only x25519 (0x001d), secp256r1 (0x0017) and secp384r1 (0x0018) are wired up — no key agreement this run.\n", ske_curve);
    }
    tcp_close(conn);
    return -1;   // handshake reached but nothing was fetched
}

// Convenience wrapper: a plain GET (no body), the common case.
int tls_https_fetch(const char* host, const char* path, int iface_idx,
                    uint8_t* out, uint32_t cap, int verbose) {
    return tls_https_request(host, path, "GET", 0, 0, iface_idx, out, cap, verbose);
}

// The `tls <host>` diagnostic command: verbosely fetch host:443 "/" into a scratch buffer.
// Uses a 64 KB heap buffer (like Selene) so it can hold large pages, not just the 16 KB tls_hd.
int tls_hello(const char* host, int iface_idx) {
    uint32_t cap = 64 * 1024;
    uint8_t* buf = (uint8_t*)kmalloc(cap);
    if (!buf) { buf = tls_hd; cap = (uint32_t)sizeof(tls_hd); }   // fall back to the scratch buffer
    int n = tls_https_fetch(host, "/", iface_idx, buf, cap, 1);
    if (n >= 0) printf("TLS: total decrypted response = %d bytes (buffer cap %u)\n", n, cap);
    if (buf != tls_hd) kfree(buf);
    return (n >= 0) ? 0 : -1;
}
