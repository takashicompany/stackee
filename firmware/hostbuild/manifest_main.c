// stackee_assets.c の拾い読みだけを Mac 上で試す。
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#define STATIC_TEST
#include "manifest_parse.inc"   // stackee_assets.c から切り出したもの (テストが作る)
int main(int argc, char **argv) {
    FILE *f = fopen(argv[1], "rb");
    static char buf[65536];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = 0;
    fclose(f);
    printf("bytes=%zu v=%d size=%d faces=%d acks=%d\n", n,
           read_int(buf, "v", -1), read_int(buf, "size", -1),
           count_array_items(buf, "faces"), count_array_items(buf, "acks"));
    // 端の場合
    printf("empty=[%d] one=[%d] nested=[%d] missing=[%d]\n",
           count_array_items("{\"a\":[]}", "a"),
           count_array_items("{\"a\":[1]}", "a"),
           count_array_items("{\"a\":[{\"b\":[1,2]},{\"b\":[3]}]}", "a"),
           count_array_items("{\"z\":1}", "a"));
    printf("comma-in-string=[%d]\n", count_array_items("{\"a\":[\"x,y\",\"z\"]}", "a"));
    return 0;
}
