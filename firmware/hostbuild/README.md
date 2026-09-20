# hostbuild — 実機なしで C を動かす

DESIGN.md §8 の「テスト: キー処理は Mac 上でホストビルドして打鍵列テストを
回す」ための土台。3 つの入り口がある。

| 入り口 | 何を確かめるか | 走らせ方 |
|---|---|---|
| `keyseq_main.c` | **打鍵列テスト**。QMK の quantum + `main/qmk_port` をそのままビルドし、時刻つきの押下 / 解放の列を流して HID レポートの列を見る | `python3 tools/test_keyseq_host.py` |
| `console_main.c` | `main/stackee_console.c` が組み立てた枠を、**ホスト側の実物** (`firmware/kmk/tools/stackee_console_client.py` の `FrameParser`) で読み解けるか | `python3 tools/test_console_host.py` |
| `manifest_main.c` | `main/stackee_assets.c` の目録の拾い読みを、**本物の** `firmware/kmk/stackee_assets/manifest.json` に当てる | 同上 |
| `render_main.c` | **段階 2**。`main/stackee_draw.c` ほかに本物の素材を通して CRC32 を出す | `python3 tools/test_render_host.py` |
| `faceanim_main.c` | **段階 2**。顔の状態機械の時刻 | `python3 tools/test_faceanim_host.py` |
| `cfg_main.c` | **段階 3**。settings.toml / Wi-Fi 登録簿 / 選び方 / 音量のレジスタ値 / JSON / 会話 URL。**現行 CircuitPython 版の同じ関数と突き合わせる** | `python3 tools/test_cfg_host.py` |
| `talk_main.c` | **段階 3**。会話の状態機械に台本を流す (偽の時計・マイク・スピーカー・通信) | `python3 tools/test_talk_host.py` |
| `wifi_main.c` | **段階 3**。Wi-Fi の状態機械に台本を流す (偽の無線) | `python3 tools/test_wifi_host.py` |
| `subtitle_main.c` | **字幕**。`main/stackee_font16.c` と `stackee_draw.c` に本物の `assets/font16.bin` を通し、帯 (240x30) の CRC32 と字形を出す | `python3 tools/test_subtitle_host.py` |

`stub/` は ESP-IDF と TinyUSB の代わり。呼び出しの形だけ合わせた最小のもので、
実機の挙動を真似しているわけではない (電池は常に 77%、NVS は常に空、など)。

打鍵列テスト (`keyseq_main.c`) は **stub を使わない**。QMK と橋渡し層は
ESP-IDF に依存していないので、そのままホストの libc でビルドできる。
橋渡し層が要求する「ハードに触る出口」(時計・NVS・再起動・独自キー) だけを
`keyseq_main.c` 自身が持つ。時計はテストが 1 ms ずつ進める偽の時計なので、
TAPPING_TERM の境界を 1 ms 単位で狙える。

台本の書き方は `keyseq_main.c` の冒頭に書いてある。手で流すこともできる:

    cc -O1 -std=gnu11 ... -o keyseq    # 実際の引数は tools/test_keyseq_host.py
    printf 't 50\nd 0 1\nt 90\nu 0 1\nt 200\n' | ./keyseq

**ここで通っても実機で動く保証にはならない。** I2C も USB も入っていない。
見ているのは「同じ押下列に同じレポート列が出るか」だけ。
