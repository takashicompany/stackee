#!/usr/bin/env python3
"""このツリーがどこに置かれているかを **1 か所で** 決める。

同じ `firmware/` が 2 つの形で存在する。

    公開リポジトリ (takashicompany/stackee)   <repo>/firmware/tools/...
    親リポジトリ (非公開) の git subtree      <repo>/public/firmware/tools/...

どちらでも道具が動くように、場所の解決はここに集める。**非公開側にしか
無いもの**(CircuitPython 版の `firmware/kmk`、像の退避先 `firmware/cp-uac/cmp`)
は「あれば使う、無ければ None か代替」にする。無いときに落ちるのではなく、
呼び手が「飛ばす」判断をできるようにするため。
"""
import os
from pathlib import Path

TOOLS = Path(__file__).resolve().parent     # firmware/tools
FW = TOOLS.parent                           # firmware        (旧 firmware/idf)
BASE = FW.parent                            # 公開: リポジトリ直下 / 非公開: public
OUTER = BASE.parent                         # 非公開リポジトリのルート (公開側では外)


def _first_dir(*candidates):
    for cand in candidates:
        if cand is not None and Path(cand).is_dir():
            return Path(cand)
    return None


# 操作盤の JavaScript。公開側は <repo>/docs/js、非公開側は public/docs/js。
WEB = _first_dir(BASE / 'docs' / 'js', OUTER / 'public' / 'docs' / 'js')

# 現行 CircuitPython 版 (非公開)。移植元の実装を import して突き合わせる
# テストが使う。公開リポジトリだけの clone では None になる。
KMK = _first_dir(OUTER / 'firmware' / 'kmk')
KMK_TOOLS = _first_dir(KMK / 'tools') if KMK else None

# 素材。公開側だけで完結するよう firmware/assets を先に見て、
# 無いものだけ非公開側の stackee_assets から拾う。
ASSETS = FW / 'assets'
KMK_ASSETS = _first_dir(KMK / 'stackee_assets') if KMK else None

# 像の既定の置き場 (build.sh の出力先)。
DEFAULT_IMAGE = FW / 'build' / 'stackee.bin'


def add_kmk_tools(path_list):
    """`firmware/kmk/tools` があれば sys.path に足す。足したら True。"""
    if KMK_TOOLS is None:
        return False
    text = str(KMK_TOOLS)
    if text not in path_list:
        path_list.insert(0, text)
    return True


def asset(name):
    """素材 1 つの場所。firmware/assets を先に見る。無ければ None。"""
    here = ASSETS / name
    if here.is_file():
        return here
    if KMK_ASSETS is not None and (KMK_ASSETS / name).is_file():
        return KMK_ASSETS / name
    return None


def backups_dir():
    """像の退避先。

    非公開側の `firmware/cp-uac/cmp` があればそこ (従来どおり)。
    無ければ `STACKEE_BACKUP_DIR`、それも無ければ
    `~/.local/share/stackee/backups`。リポジトリの外へ逃がす。
    """
    cmp_dir = OUTER / 'firmware' / 'cp-uac' / 'cmp'
    if cmp_dir.is_dir():
        return cmp_dir
    env = os.environ.get('STACKEE_BACKUP_DIR')
    if env:
        return Path(env).expanduser()
    return Path.home() / '.local' / 'share' / 'stackee' / 'backups'


def skip_module(reason, module_name):
    """移植元 (非公開) が無いので、この検査ファイルを丸ごと飛ばす。

    公開リポジトリだけを clone した木では、現行 CircuitPython 版
    (`firmware/kmk`) が無い検査がいくつかある。**無いのは異常ではない**ので、

      * 直接走らせたとき (`python3 tools/test_xxx.py`) は「飛ばした」と
        出して終了コード 0 で降りる。README の一括実行が赤くならない。
      * pytest から読まれたときは SkipTest を投げる。pytest が skipped として
        数える。

    使い方: `tree.skip_module('...が無い', __name__)`
    """
    import unittest
    if module_name == '__main__':
        print('SKIP: %s' % reason)
        raise SystemExit(0)
    raise unittest.SkipTest(reason)
