#!/usr/bin/env python3
"""一次回答 (ack) の文を、字幕の帯に出す行へ割る。**実機に触らない。**

★ **規則はサーバのもの。ここはその写し。** 返答の字幕はサーバが
`public/server/stackee_server.py` の `subtitle_pages()` で行に割って
`<start_ms>\\t<text>` として渡してくる。一次回答はサーバを通らない
(素材に入っている音声をその場で鳴らす) ので、**同じ規則で前もって割った行**を
`assets/manifest.json` の `acks[].lines` に持たせる。

  ・全角 1 桁 / 半角 0.5 桁、1 行 15 桁まで
  ・まず句点・読点で区切り (clause)、長い区切りだけ桁で割る
  ・句読点や閉じ括弧で行を**始めない**
  ・15 桁を超えるときは「収まる最小の行数」で均等に割る

写しである以上ずれうるので、`tools/test_subtitle_host.py` の `AckLinesTest`
が**本物のサーバを import して** 5 文すべてで突き合わせる。ずれたら落ちる。
サーバ側のコードは 1 行も変えていない。

  python3 firmware/tools/ack_lines.py 'わかったのだ。少し待っていてほしいのだ。'
"""
import sys

# stackee_server.py と同じ値 (SUBTITLE_COLUMNS / SENTENCE_MARKS /
# CLAUSE_MARKS / NO_PAGE_START)。
COLUMNS = 15
SENTENCE_MARKS = "。．！？!?\n"
CLAUSE_MARKS = "、，,"
NO_PAGE_START = "。、．，,.！？!?」』）)】〕》〉］]｝}・ー:;：；"


def page_width(text):
    """桁数。ASCII は半角 (0.5 桁)、それ以外は全角 (1 桁)。"""
    return sum(.5 if " " <= character <= "~" else 1. for character in text)


def opens_badly(text, index):
    """index から始めると句読点・閉じ括弧で行が始まってしまうか。"""
    rest = text[index:].lstrip()
    return bool(rest) and rest[0] in NO_PAGE_START


def split_columns(text, columns=COLUMNS):
    """15 桁以内の断片へ。残りは常に「収まる最小の行数」で分け合う。"""
    pieces, start = [], 0
    while start < len(text):
        rest = page_width(text[start:])
        target = rest / -(-rest // columns)     # 切り上げ除算 = 残りの最小行数
        end, width, best = start, 0., None
        while end < len(text) and width + page_width(text[end]) <= columns:
            width += page_width(text[end])
            end += 1
            if best is None or abs(width - target) < best[1]:
                best = end, abs(width - target)
        end = best[0]
        while start + 1 < end < len(text) and opens_badly(text, end):
            end -= 1
        pieces.append(text[start:end])
        start = end
    return pieces


def clause_spans(text):
    """1 文の中の区切りの範囲。全部つなぐと元の文になる。"""
    spans, start = [], 0
    for index, character in enumerate(text):
        if character in CLAUSE_MARKS + SENTENCE_MARKS:
            spans.append((start, index + 1))
            start = index + 1
    if start < len(text):
        spans.append((start, len(text)))
    return spans


def subtitle_lines(text):
    """帯に出す行。サーバの `subtitle_pages()` が返す本文と同じ並び。"""
    lines = []
    for start, end in clause_spans(text):
        for piece in split_columns(text[start:end]):
            page = " ".join(piece.split())
            if page:
                lines.append(page)
    return lines


def main():
    for text in sys.argv[1:]:
        lines = subtitle_lines(text)
        print('%r -> %d 行' % (text, len(lines)))
        for line in lines:
            print('  %-4s %r' % ('%.1f' % page_width(line), line))
    return 0


if __name__ == '__main__':
    sys.exit(main())
