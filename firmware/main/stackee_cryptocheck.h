// 暗号の自己診断 (2026-09-17)。README §20。
//   HTTPS の証明書検証が PSA_ERROR_INVALID_SIGNATURE (-149) で落ちる事象
//   (espressif/esp-idf#18640 と同じログ) を、人手なしで切り分け・復旧するため。
//   - SHA-256 の既知ベクタ (内蔵 RAM / PSRAM) と、実際に失敗した署名
//     (ISRG Root X1 → X2 クロス証明書、RSA-4096/SHA-256) を再計算する
//   - HTTP が TLS で失敗したとき、10 分ごと、console `crypto.selftest` から呼ぶ
//   - 壊れていたら「壊れた」印を立て、main が会話の合間に再起動する
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    int sha_abc, sha_internal_4k, sha_psram_4k;   // 1 OK / 0 不一致 / 負 = 計算失敗
    int x509_parse1, x509_parse2, x509_rc;
    uint32_t x509_flags;
    int x509_sig_ok;
    bool ok;
} stackee_cryptocheck_result_t;

// 走らせる (数十 ms)。ok なら true。
bool stackee_cryptocheck_run(stackee_cryptocheck_result_t *out);

typedef struct {
    uint32_t runs, fails;
    bool     last_ok;
    bool     broken;        // 一度でも fail した (再起動するまで残る)
    int64_t  broken_at_us;
} stackee_cryptocheck_stats_t;
void stackee_cryptocheck_stats(stackee_cryptocheck_stats_t *out);
bool stackee_cryptocheck_broken(void);
