#!/usr/bin/env python3
"""一次回答 (ack) の文を、字幕の帯に出す行へ割る。**実機に触らない。**

★ **規則はサーバのもの。ここはその写し。** 返答の字幕はサーバが
`public/server/stackee_server.py` の `subtitle_pages()` で行に割って
`<start_ms>\\t<text>` として渡してくる。一次回答はサーバを通らない
(素材に入っている音声をその場で鳴らす) ので、**同じ規則で前もって割った行**を
`assets/manifest.json` の `acks[].lines` に持たせる。

  ・全角 1 桁 / 半角 0.5 桁、1 行 15 桁まで (必ず 15 桁以内)
  ・文 (句点など) ごとに割り、文をまたいでは 1 行に詰めない
  ・文の中は流し込み、各行をできるだけ 15 桁まで埋める
  ・読点のあとで改行してよいのは、その行が 10 桁以上のときだけ
  ・行頭禁則は追い出し (句読点・閉じ括弧・ー・〜・小書き仮名が行頭に
    来ないよう、行末を次の行へ送る)
  ・文の最後の行が 4 桁未満なら、前の行の末尾を少しもらう

写しである以上ずれうるので、`tools/test_subtitle_host.py` の `AckLinesTest`
が**本物のサーバを import して** 5 文と見本の文で突き合わせる。ずれたら落ちる。
サーバ側のコードは 1 行も変えていない。

  python3 firmware/tools/ack_lines.py 'わかったのだ。少し待っていてほしいのだ。'
"""
import sys

# stackee_server.py と同じ値 (SUBTITLE_COLUMNS / SUBTITLE_CLAUSE_BREAK /
# SUBTITLE_MIN_LAST / SENTENCE_MARKS / CLAUSE_MARKS / NO_PAGE_START)。
COLUMNS = 15
CLAUSE_BREAK = 10
MIN_LAST = 4
SENTENCE_MARKS = "。．！？!?\n"
CLAUSE_MARKS = "、，,"
NO_PAGE_START = ("。、．，,.！？!?」』）)】〕》〉］]｝}・ー〜～:;：；"
                 "ぁぃぅぇぉっゃゅょゎァィゥェォッャュョヮヵヶ")


def split_parts(text, marks):
    """各区切り記号の直後で分ける (記号は前に付けたまま)。つなぐと元の文。"""
    parts, current = [], ""
    for character in text:
        current += character
        if character in marks:
            parts.append(current)
            current = ""
    parts.append(current)
    return [part for part in parts if part.strip()]


def page_width(text):
    """桁数。ASCII は半角 (0.5 桁)、それ以外は全角 (1 桁)。"""
    return sum(.5 if " " <= character <= "~" else 1. for character in text)


def opens_badly(text, index):
    """index から始めると行頭に来てはいけない字で行が始まってしまうか。"""
    rest = text[index:].lstrip()
    return bool(rest) and rest[0] in NO_PAGE_START


def clause_spans(text):
    """1 文の中の区切りの範囲。全部つなぐと元の文になる。

    句読点の直後が行頭に来てはいけない字なら、そこでは区切らない。
    """
    spans, start = [], 0
    for index, character in enumerate(text):
        if (character in CLAUSE_MARKS + SENTENCE_MARKS
                and not opens_badly(text, index + 1)):
            spans.append((start, index + 1))
            start = index + 1
    if start < len(text):
        spans.append((start, len(text)))
    return spans


def sentence_pages(text):
    """1 文の行 (サーバの `subtitle_pages()` の本文)。

    文を 15 桁の行へ流し込む。読点のあとで改行してよいのは、その行が
    10 桁以上埋まっているときだけ。行頭禁則は追い出し (行末を次の行へ送る)
    なので、どの行も 15 桁を超えない。最後の行が 4 桁未満なら前の行の
    末尾を少しもらう。
    """
    breaks = {end for _, end in clause_spans(text)}
    pages, start = [], 0
    while start < len(text):
        while start < len(text) and text[start].isspace():
            start += 1
        end, width, cut = start, 0., None
        while end < len(text) and width + page_width(text[end]) <= COLUMNS:
            width += page_width(text[end])
            end += 1
            if end in breaks and width >= CLAUSE_BREAK:
                cut = end
        if end < len(text) and cut is not None:
            end = cut
        while start + 1 < end < len(text) and opens_badly(text, end):
            end -= 1
        page = " ".join(text[start:end].split())
        if page:
            pages.append((start, page))
        start = end
    if len(pages) > 1 and page_width(pages[-1][1]) < MIN_LAST:
        before, cut = pages[-2][0], pages[-1][0]
        while cut - 1 > before and (page_width(text[cut:].strip()) < MIN_LAST
                                    or opens_badly(text, cut)):
            cut -= 1
        if not opens_badly(text, cut) and page_width(text[cut:].strip()) <= COLUMNS:
            pages[-2:] = [(before, " ".join(text[before:cut].split())),
                          (cut, " ".join(text[cut:].split()))]
    return [page for _offset, page in pages]


def subtitle_lines(text):
    """帯に出す行。サーバと同じく**文ごとに**割り、文をまたいでは詰めない
    (サーバの `subtitle_only_body()` が並べる本文と同じ並び)。"""
    return [page for sentence in split_parts(text, SENTENCE_MARKS)
            for page in sentence_pages(sentence)]


def main():
    for text in sys.argv[1:]:
        lines = subtitle_lines(text)
        print('%r -> %d 行' % (text, len(lines)))
        for line in lines:
            print('  %-4s %r' % ('%.1f' % page_width(line), line))
    return 0


if __name__ == '__main__':
    sys.exit(main())
