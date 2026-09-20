// ホストビルド用の SHA-256。**実機では使わない** (実機は mbedtls)。
//
// stackee_otacore.c は SHA-256 の実体を外から差し込む作りなので、ホストでは
// これを渡す。libcrypto を要求しないのは、hostbuild が `cc` 1 本で通ることを
// 守るため (hostbuild/README.md)。値は tools/test_ota_host.py が Python の
// hashlib と突き合わせる。
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t state[8];
    uint64_t bits;
    uint8_t  buf[64];
    size_t   len;
} sha256_stub_t;

void sha256_stub_init(sha256_stub_t *ctx);
void sha256_stub_update(sha256_stub_t *ctx, const void *data, size_t len);
void sha256_stub_final(sha256_stub_t *ctx, uint8_t out[32]);
