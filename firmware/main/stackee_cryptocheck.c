#include "stackee_cryptocheck.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "mbedtls/x509_crt.h"
#include "psa/crypto.h"
#include "sdkconfig.h"

#include "stackee_selftest_certs.h"

static const char *TAG = "cryptocheck";

// sha256("abc") / sha256('a' x 4096)
static const uint8_t SHA_ABC[32] = {
    0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
    0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad };
static const uint8_t SHA_A4096[32] = {
    0xc9,0x3e,0xee,0x2d,0x0d,0xb0,0x2f,0x10,0xac,0xc7,0x46,0x0d,0x95,0x76,0xe1,0x22,
    0xdc,0xf8,0xcd,0x53,0xc4,0xbf,0x8d,0xfc,0xae,0x1b,0x3e,0x74,0xeb,0xcf,0xff,0x5a };

static stackee_cryptocheck_stats_t s_stats;

static int check_sha(const uint8_t *msg, size_t len, const uint8_t *want) {
    uint8_t out[32];
    size_t  got = 0;
    psa_status_t st = psa_hash_compute(PSA_ALG_SHA_256, msg, len, out, sizeof(out), &got);
    if (st != PSA_SUCCESS || got != 32) {
        return -(int)st;
    }
    return memcmp(out, want, 32) == 0 ? 1 : 0;
}

bool stackee_cryptocheck_run(stackee_cryptocheck_result_t *out) {
    stackee_cryptocheck_result_t r;
    memset(&r, 0, sizeof(r));
    psa_crypto_init();
    uint8_t *in_int = heap_caps_malloc(4096, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint8_t *in_ps  = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    r.sha_abc = check_sha((const uint8_t *)"abc", 3, SHA_ABC);
    r.sha_internal_4k = r.sha_psram_4k = -999;
    if (in_int != NULL) { memset(in_int, 'a', 4096); r.sha_internal_4k = check_sha(in_int, 4096, SHA_A4096); }
    if (in_ps  != NULL) { memset(in_ps,  'a', 4096); r.sha_psram_4k    = check_sha(in_ps,  4096, SHA_A4096); }
    free(in_int);
    free(in_ps);

    mbedtls_x509_crt trust, chain;
    mbedtls_x509_crt_init(&trust);
    mbedtls_x509_crt_init(&chain);
    r.x509_parse1 = mbedtls_x509_crt_parse(&trust, (const uint8_t *)STACKEE_SELFTEST_X1_PEM,
                                           sizeof(STACKEE_SELFTEST_X1_PEM));
    r.x509_parse2 = mbedtls_x509_crt_parse(&chain, (const uint8_t *)STACKEE_SELFTEST_X2_CROSS_PEM,
                                           sizeof(STACKEE_SELFTEST_X2_CROSS_PEM));
    r.x509_rc = -1;
    if (r.x509_parse1 == 0 && r.x509_parse2 == 0) {
        r.x509_rc = mbedtls_x509_crt_verify(&chain, &trust, NULL, NULL, &r.x509_flags, NULL, NULL);
    }
    mbedtls_x509_crt_free(&trust);
    mbedtls_x509_crt_free(&chain);
    // 時計が無いことによる日付の不一致は署名の話ではない。
    uint32_t sig_flags = r.x509_flags & ~(uint32_t)(MBEDTLS_X509_BADCERT_EXPIRED | MBEDTLS_X509_BADCERT_FUTURE);
    r.x509_sig_ok = (r.x509_parse1 == 0 && r.x509_parse2 == 0 && (r.x509_rc == 0 || sig_flags == 0)) ? 1 : 0;
    r.ok = (r.sha_abc == 1 && r.sha_internal_4k == 1 && r.sha_psram_4k == 1 && r.x509_sig_ok);

    s_stats.runs++;
    s_stats.last_ok = r.ok;
    if (!r.ok) {
        s_stats.fails++;
        if (!s_stats.broken) {
            s_stats.broken = true;
            s_stats.broken_at_us = esp_timer_get_time();
        }
        ESP_LOGE(TAG, "★ 暗号の自己診断 NG: sha abc=%d int4k=%d psram4k=%d / x509 parse=%d,%d rc=%d flags=0x%lx sig_ok=%d (hw_sha=%d hw_mpi=%d)",
                 r.sha_abc, r.sha_internal_4k, r.sha_psram_4k, r.x509_parse1, r.x509_parse2,
                 r.x509_rc, (unsigned long)r.x509_flags, r.x509_sig_ok,
#ifdef CONFIG_MBEDTLS_HARDWARE_SHA
                 1,
#else
                 0,
#endif
#ifdef CONFIG_MBEDTLS_HARDWARE_MPI
                 1
#else
                 0
#endif
                 );
    }
    if (out != NULL) { *out = r; }
    return r.ok;
}

void stackee_cryptocheck_stats(stackee_cryptocheck_stats_t *out) {
    if (out != NULL) { *out = s_stats; }
}

bool stackee_cryptocheck_broken(void) {
    return s_stats.broken;
}
