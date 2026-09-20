#include "stackee_uac.h"

#include <string.h>

#include "tusb.h"

#if CFG_TUD_AUDIO

#include "esp_log.h"

#include "stackee_audio.h"
#include "stackee_usb.h"

static const char *TAG = "uac";

// UAC の記述子は「常に 2 チャネル」を名乗る (TinyUSB の
// TUD_AUDIO20_MIC_ONE_CH_DESCRIPTOR は 1 チャネル)。こちらは 1 チャネルの
// まま。ES7210 の MIC1 だけを使う (現行 CircuitPython 版の boot.py も
// channel_count=1)。
#define FRAME_BYTES (STACKEE_UAC_BYTES * STACKEE_UAC_CHANNELS)

// 1 回で作るサンプル数の上限。16 kHz なら 1 ms = 16 サンプル。
#define CHUNK_SAMPLES 256

static struct {
    volatile bool streaming;
    uint32_t opens;
    uint32_t frames;
    uint32_t silence;
    uint32_t underruns;
    int16_t  mute[2];       // [0] = master, [1] = ch1
    int16_t  volume[2];
    int16_t  chunk[CHUNK_SAMPLES];
} uac;

void stackee_uac_init(void) {
    memset(&uac, 0, sizeof(uac));
}

// ---------------------------------------------------------------------------
// audio タスクから毎周
// ---------------------------------------------------------------------------
// ★ 送る量はホストが吸った量で決める (CircuitPython 版と同じ考え方)。
//   FIFO を半分まで埋め戻すだけにしておくと、こちらの生産速度が
//   ホストの USB SOF に自然に揃い、途切れ (splice) が出ない。
void stackee_uac_step(void) {
    if (!uac.streaming) {
        return;
    }
    tu_fifo_t *ff = tud_audio_get_ep_in_ff();
    if (ff == NULL) {
        return;
    }
    uint16_t target = (uint16_t)(tu_fifo_depth(ff) / 2);
    for (int guard = 0; guard < 8; guard++) {
        uint16_t have = tu_fifo_count(ff);
        if (have >= target) {
            break;
        }
        uint16_t want_bytes = (uint16_t)(target - have);
        int want = want_bytes / FRAME_BYTES;
        if (want <= 0) {
            break;
        }
        if (want > CHUNK_SAMPLES) {
            want = CHUNK_SAMPLES;
        }
        int got = stackee_audio_uac_pull(uac.chunk, want);
        if (got < 0) {
            // 会話が使っている。★ 無音で埋めて口を絶やさない
            //   (途切れさせるとホスト側が「マイクが壊れた」扱いにする)。
            memset(uac.chunk, 0, (size_t)want * FRAME_BYTES);
            got = want;
            uac.silence += (uint32_t)want;
        } else if (got == 0) {
            uac.underruns++;
            break;      // まだ DMA に溜まっていない。次の周で
        } else {
            uac.frames += (uint32_t)got;
        }
        if (uac.mute[0] || uac.mute[1]) {
            memset(uac.chunk, 0, (size_t)got * FRAME_BYTES);
        }
        if (tud_audio_write(uac.chunk, (uint16_t)(got * FRAME_BYTES)) == 0) {
            break;
        }
    }
}

void stackee_uac_stats(stackee_uac_stats_t *out) {
    if (out == NULL) {
        return;
    }
    out->enabled = true;
    out->streaming = uac.streaming;
    out->opens = uac.opens;
    out->frames = uac.frames;
    out->silence = uac.silence;
    out->underruns = uac.underruns;
    out->volume_db = (uint16_t)uac.volume[0];
    out->muted = uac.mute[0] != 0;
}

// ---------------------------------------------------------------------------
// TinyUSB の呼び出し (weak を上書き)
// ---------------------------------------------------------------------------
bool tud_audio_set_itf_cb(uint8_t rhport, tusb_control_request_t const *p_request) {
    (void)rhport;
    uint8_t itf = (uint8_t)tu_u16_low(p_request->wIndex);
    bool streaming = (tu_u16_low(p_request->wValue) != 0);
    if (itf != STACKEE_UAC_ITF_AS) {
        return true;
    }
    uac.streaming = streaming;
    if (streaming) {
        uac.opens++;
        ESP_LOGI(TAG, "ホストが USB マイクを開いた");
    } else {
        ESP_LOGI(TAG, "ホストが USB マイクを閉じた");
        stackee_audio_uac_release();
    }
    return true;
}

bool tud_audio_set_itf_close_ep_cb(uint8_t rhport,
                                   tusb_control_request_t const *p_request) {
    (void)rhport;
    uint8_t itf = (uint8_t)tu_u16_low(p_request->wIndex);
    if (itf == STACKEE_UAC_ITF_AS) {
        uac.streaming = false;
        stackee_audio_uac_release();
    }
    return true;
}

bool tud_audio_set_req_entity_cb(uint8_t rhport,
                                 tusb_control_request_t const *p_request,
                                 uint8_t *pBuff) {
    (void)rhport;
    uint8_t ch = (uint8_t)tu_u16_low(p_request->wValue);
    uint8_t sel = (uint8_t)tu_u16_high(p_request->wValue);
    uint8_t entity = (uint8_t)tu_u16_high(p_request->wIndex);
    if (p_request->bRequest != AUDIO20_CS_REQ_CUR) {
        return false;
    }
    if (entity != STACKEE_UAC_ENTITY_FEATURE_UNIT || ch > 1) {
        return false;
    }
    switch (sel) {
        case AUDIO20_FU_CTRL_MUTE:
            uac.mute[ch] = ((audio20_control_cur_1_t *)pBuff)->bCur;
            return true;
        case AUDIO20_FU_CTRL_VOLUME:
            uac.volume[ch] = ((audio20_control_cur_2_t *)pBuff)->bCur;
            return true;
        default:
            return false;
    }
}

bool tud_audio_get_req_entity_cb(uint8_t rhport,
                                 tusb_control_request_t const *p_request) {
    uint8_t ch = (uint8_t)tu_u16_low(p_request->wValue);
    uint8_t sel = (uint8_t)tu_u16_high(p_request->wValue);
    uint8_t entity = (uint8_t)tu_u16_high(p_request->wIndex);

    if (entity == STACKEE_UAC_ENTITY_INPUT_TERMINAL) {
        if (sel != AUDIO20_TE_CTRL_CONNECTOR) {
            return false;
        }
        audio20_desc_channel_cluster_t ret;
        ret.bNrChannels = STACKEE_UAC_CHANNELS;
        ret.bmChannelConfig = (audio20_channel_config_t)0;
        ret.iChannelNames = 0;
        return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request,
                                                          &ret, sizeof(ret));
    }
    if (entity == STACKEE_UAC_ENTITY_FEATURE_UNIT) {
        if (ch > 1) {
            return false;
        }
        if (sel == AUDIO20_FU_CTRL_MUTE) {
            return tud_audio_buffer_and_schedule_control_xfer(
                rhport, p_request, &uac.mute[ch], sizeof(uac.mute[ch]));
        }
        if (sel == AUDIO20_FU_CTRL_VOLUME) {
            if (p_request->bRequest == AUDIO20_CS_REQ_CUR) {
                return tud_audio_buffer_and_schedule_control_xfer(
                    rhport, p_request, &uac.volume[ch], sizeof(uac.volume[ch]));
            }
            if (p_request->bRequest == AUDIO20_CS_REQ_RANGE) {
                audio20_control_range_2_n_t(1) ret;
                ret.wNumSubRanges = 1;
                ret.subrange[0].bMin = -90 * 256;
                ret.subrange[0].bMax = 90 * 256;
                ret.subrange[0].bRes = 256;
                return tud_audio_buffer_and_schedule_control_xfer(
                    rhport, p_request, &ret, sizeof(ret));
            }
        }
        return false;
    }
    if (entity == STACKEE_UAC_ENTITY_CLOCK_SOURCE) {
        if (sel == AUDIO20_CS_CTRL_SAM_FREQ) {
            if (p_request->bRequest == AUDIO20_CS_REQ_CUR) {
                audio20_control_cur_4_t cur = {.bCur = (int32_t)STACKEE_UAC_RATE};
                return tud_audio_buffer_and_schedule_control_xfer(
                    rhport, p_request, &cur, sizeof(cur));
            }
            if (p_request->bRequest == AUDIO20_CS_REQ_RANGE) {
                audio20_control_range_4_n_t(1) ret;
                ret.wNumSubRanges = 1;
                ret.subrange[0].bMin = (int32_t)STACKEE_UAC_RATE;
                ret.subrange[0].bMax = (int32_t)STACKEE_UAC_RATE;
                ret.subrange[0].bRes = 0;
                return tud_audio_buffer_and_schedule_control_xfer(
                    rhport, p_request, &ret, sizeof(ret));
            }
            return false;
        }
        if (sel == AUDIO20_CS_CTRL_CLK_VALID) {
            audio20_control_cur_1_t cur = {.bCur = 1};
            return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request,
                                                              &cur, sizeof(cur));
        }
        return false;
    }
    return false;
}

#else   // CFG_TUD_AUDIO == 0 (dev プロファイル)

void stackee_uac_init(void) {}
void stackee_uac_step(void) {}

void stackee_uac_stats(stackee_uac_stats_t *out) {
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
}

#endif
