// QMK のデバッグ出力の出口。
//
// third_party/qmk/quantum/logging/print.h は
//   __has_include_next("_print.h") があればそれを使う (= プラットフォーム側が
//   print_printf を用意する)、無ければ QMK 同梱の小さな printf を使う
// という作りになっている。QMK 同梱の printf (lib/printf) は取り込みたくない
// ので、こちらで出口を用意する。
//
// ★ 実際に文字が出るのは debug_enable を立てたときだけ (QMK の既定は off)。
//   入力タスクの経路に printf が挟まらないよう、既定のままにしておくこと。
#pragma once

#include <stdarg.h>

void stackee_qmk_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#define print_printf stackee_qmk_printf
