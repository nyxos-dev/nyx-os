#ifndef TLS_H
#define TLS_H

// Verbose HTTPS GET diagnostic; returns 0 on a decrypted response, -1 on failure.
int tls_hello(const char* host, int iface_idx);

// Full https fetch over TLS 1.2: handshake with host:443, GET <path>, decrypt the reply
// into out[cap]. Returns the response length (>= 0) or -1 on failure; verbose gates prints.
int tls_https_fetch(const char* host, const char* path, int iface_idx,
                    uint8_t* out, uint32_t cap, int verbose);

// Like tls_https_fetch but with an explicit method and an optional form body (POST). A non-NULL
// body is sent as application/x-www-form-urlencoded with a Content-Length.
int tls_https_request(const char* host, const char* path, const char* method,
                      const uint8_t* body, uint32_t body_len, int iface_idx,
                      uint8_t* out, uint32_t cap, int verbose);

// Strict certificate enforcement: when on, a handshake is refused unless the chain anchors to a
// pinned root, the hostname matches, and the leaf certificate is in date. On by default; an explicit development override
// applies only to the current boot session.
void tls_set_strict(int on);
int  tls_get_strict(void);

// Known-answer self-test for the TLS 1.2 key schedule (master secret + AES key block).
int tls_keyschedule_selftest(void);

// Known-answer self-test for the AES-128-GCM record layer + Finished verify_data.
int tls_record_selftest(void);

// Known-answer self-test for ECDSA-P384/SHA-384 ServerKeyExchange verification (the `skp384test` command).
int tls_ske_p384_selftest(void);

#endif
