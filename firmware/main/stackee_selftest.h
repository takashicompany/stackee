// ui.selftest が順に描く「ステータスバーの代表 6 状態」。
//
// ★ 実機 (stackee_ui.c) と Mac の期待値 (tools/render_expected.py) と
//   ホストビルド (hostbuild/render_main.c) の 3 者がこの同じ並びを使う。
//   ずれると照合が「不一致」ではなく「別のものを比べている」になるので、
//   表は 1 か所 (ここ) にしか置かない。Python 側は tools/test_render_host.py
//   が同じ並びであることを機械照合する。
//
// 0..4 は firmware/kmk/tools/preview_status_bar.py の SCENARIOS と同じ。
// 5 は起動直後の既定 (電池を読む前・音量 20・Wi-Fi off・BLE 広告中)。
#pragma once

#include "stackee_draw.h"

#define STACKEE_SELFTEST_BARS 6

const stackee_bar_state_t *stackee_selftest_bar(int index);
const char *stackee_selftest_bar_name(int index);
