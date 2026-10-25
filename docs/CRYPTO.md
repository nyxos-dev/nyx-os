# NyxOS cryptography & the TLS trust model

NyxOS speaks real **TLS 1.2** to real HTTPS servers, and every primitive underneath is
written from scratch in freestanding C — no ported crypto library, no `libgcc`, and
**no floating point** (the kernel is built `-mno-sse`; the bignum and elliptic-curve
maths are all integer). Each primitive is pinned by a **known-answer test** in the CI
self-test battery, and the certificate trust model is **key-pinned roots**: a chain is
trusted only if it anchors to a bundled root *public key*.

This document maps the primitives, the TLS 1.2 handshake, and — the part that actually
decides whether a connection is safe — the X.509 trust model.

---

## At a glance

| File | Primitive |
|---|---|
| `kernel/crypto/sha256.c` | SHA-256, **HMAC-SHA256**, **PBKDF2** (the login KDF), hex |
| `kernel/crypto/sha512.c` | SHA-384 / SHA-512 |
| `kernel/crypto/aes_gcm.c` | AES-128-GCM (the TLS record AEAD) |
| `kernel/crypto/rsa.c` | RSA PKCS#1 v1.5 / PSS signature **verify** (SHA-256/512) |
| `kernel/crypto/p256.c`, `p384.c` | NIST P-256 / P-384 — ECDHE + **ECDSA verify** |
| `kernel/crypto/curve25519.c` | X25519 (Curve25519 ECDHE) |
| `kernel/crypto/csprng.c` | `csprng_bytes()` — the cryptographic RNG |
| `kernel/crypto/der.c` | DER / ASN.1 reader (parses X.509) |
| `kernel/crypto/x509.c` + `x509_roots.h` | Certificate-chain verification + the pinned root store |
| `kernel/crypto/tls/tls.c`, `tls_prf.c` | TLS 1.2 handshake, key schedule, record layer |
| `kernel/crypto/base64.c`, `utf8.c` | Base64 + strict UTF-8 (encoding, not secrets) |

---

## Primitives

- **Hashing & KDF.** SHA-256/384/512 (streaming `init`/`update`/`final`). On top:
  `hmac_sha256` and `pbkdf2_hmac_sha256` — the latter is the **login KDF** (`auth.c`
  hashes every password with it; see the `pbkdf2` KAT and the auth flow).
- **AEAD.** `aes128_gcm_encrypt` — AES-128 in Galois/Counter Mode, the TLS 1.2 record
  cipher (confidentiality + a per-record authentication tag).
- **Signatures (verify only).** `rsa_pkcs1_sha256_verify` / `rsa_pkcs1_sha512_verify` /
  `rsa_pss_sha256_verify`, and `p256_ecdsa_verify` / `p384_ecdsa_verify`. NyxOS is a
  *client*, so it verifies signatures (server certs, ServerKeyExchange) but never signs.
- **Key agreement (ECDHE).** `p256_*` / `p384_*` scalar multiplication and `x25519` —
  ephemeral Diffie-Hellman so each session has forward secrecy.
- **Randomness.** `csprng_bytes()` — the single cryptographic RNG behind TLS nonces,
  the random DNS transaction IDs (anti-spoofing), and per-account password salts.
- **Parsing / encoding.** `der.c` reads ASN.1 (the whole X.509 parse rests on it);
  `base64.c` and the strict `utf8.c` validator handle text, not secrets.

Everything is integer-only: RSA is bignum modular exponentiation, the NIST curves are
GF(p) field arithmetic, all without a hardware multiplier library.

---

## TLS 1.2 handshake

`tls_https_fetch(host, path, …)` (`tls/tls.c`) runs the whole exchange:

1. **ClientHello** — TCP-connect to `host:443`, send a real TLS 1.2 ClientHello with
   SNI, the cipher-suite list, and extensions (`tls_hello` is the standalone starter).
2. **Server flight** — parse **ServerHello** (the chosen cipher), the **Certificate**
   chain, **ServerKeyExchange** (the server's ephemeral ECDHE public + its signature),
   and ServerHelloDone.
3. **Verify** — check the ServerKeyExchange signature (e.g. ECDSA-P384/SHA-384, the
   `tls_ske_p384` KAT) and validate the certificate chain (below).
4. **Key agreement** — ECDHE: derive the pre-master secret, then the **master secret**
   and the AES-GCM key block via the TLS PRF (`tls_prf.c`; the `tls_keysched` KAT).
5. **Finished** — ChangeCipherSpec, then an encrypted Finished whose `verify_data` binds
   the whole transcript (the `tls_record` KAT covers the AES-GCM record + Finished).
6. **Fetch** — send the `GET`/`POST` inside encrypted records, decrypt the reply.

`tls_https_request` adds an explicit method + an optional form body (POST as
`application/x-www-form-urlencoded`).

---

## The X.509 trust model — key-pinned roots

Parsing a certificate proves nothing on its own; the question is *whose key signed it*.
`x509_verify_chain(certs, lens, n, msg, msgcap)` (`x509.c`) answers it:

- **Walk the chain.** `certs[0]` is the leaf; each certificate is verified under the
  **next** one's public key (RSA or ECDSA). An intermediate must assert
  **basicConstraints CA:TRUE** — an end-entity cert cannot sign another cert (issue #49,
  landed v6.4.69).
- **Anchor to a pinned root.** The top certificate's key must match one of the bundled
  trust anchors in `NYX_TRUSTED_ROOTS[]` (`x509_roots.h`) — the *public keys* of five
  real root/intermediate CAs (a mix of **P-384 EC** and **RSA-2048/4096**, captured for
  the test sites). Pinning the anchor key is simpler than a full CA store but sound: a
  forged chain can't produce a signature that verifies under a pinned key.

The result is a three-way verdict, not a boolean (`x509.h`):

| Code | Meaning |
|---|---|
| `X509_OK` (0) | Every link verified **and** the top is a pinned trusted root |
| `X509_INCOMPLETE` (1) | Links checked, but not anchored (unknown root or unsupported sig-alg) — **not** a detected forgery |
| `X509_FORGED` (−1) | A supported-algorithm link failed cryptographic verification — an actual bad signature |

Two more leaf checks complete the picture:

- **`x509_check_host(leaf, len, host)`** — the leaf must cover the hostname via a
  subjectAltName **dNSName** (case-insensitive, one leading `*.` wildcard label honoured).
- **`x509_check_validity(cert, len)`** — the cert must be within its notBefore/notAfter
  window per the RTC.

**Strict mode is on by default since 6.5.251.** A handshake is refused unless the
supported checks establish a pinned chain, matching hostname, valid leaf dates
and a verified key-exchange signature. `tlsstrict` reports the current setting.
`tlsstrict off` is an explicit, session-only development override; `tlsstrict on`
restores enforcement. It also affects Selene's HTTPS client. A missing or empty
Certificate message, a detected forged chain or an invalid key-exchange signature
is rejected in either mode.

The bundled keys are a small test-oriented trust set, not a general CA store.
Strict mode can refuse sites with otherwise valid certificates whose roots or
signature algorithms are unsupported. These checks do not implement complete
PKIX validation: intermediate validity dates and revocation are not checked.
The leaf-date check depends on the RTC. An encrypted response in development
override mode does not establish an authenticated server identity.

---

## Security posture & the KAT battery

Fifteen of the CI self-test battery's 28 KATs pin this subsystem (see
`docs/ARCHITECTURE.md` for the whole battery):

`sha512`, `pbkdf2`, `csprng`, `aes_gcm`, `curve25519`, `tls_prf`, `tls_keysched`,
`tls_record`, `tls_ske_p384`, `der`, `base64`, `utf8`, `p256`, `p384`, `rsa`, `x509`.

The `x509` KAT is adversarial: it accepts a real chain, **rejects a one-byte-tampered
copy as forged**, and rejects a root-less prefix as not-anchored. Together with the
network-side parser KATs (`dns`, `httpparse`, `tcpcksum` — see `docs/NETWORK.md`), the
whole path from untrusted bytes to a trusted session is locked against regression.

---

## Where things live / extending

- A new hash or cipher → `kernel/crypto/<name>.c` + a `<name>_selftest()` wired into the
  battery (mirror any existing crypto KAT).
- A new trust anchor → add its public key to `NYX_TRUSTED_ROOTS[]` in `x509_roots.h`.
- Anything that consumes attacker-controlled bytes (a new cert extension, a TLS message)
  must stay bounds-safe and come with an adversarial KAT.

See also `docs/ARCHITECTURE.md` (whole-system overview) and `docs/NETWORK.md` (the
TCP/IP stack that carries TLS).
