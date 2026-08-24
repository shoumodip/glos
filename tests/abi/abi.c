#include "abi.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

void s8_foo(S8 s) {
    printf("%d %d\n", s.x, s.y);
}

S8 s8_ret(i32 x, i32 y) {
    return (S8) {.x = x, .y = y};
}

void s16_foo(S16 s) {
    printf("%" PRId64 " %" PRId64 "\n", s.x, s.y);
}

void s16_bar(S16 s0, S16 s1, S16 s2, S16 s3) {
    printf("%" PRId64 " %" PRId64 "\n", s0.x, s0.y);
    printf("%" PRId64 " %" PRId64 "\n", s1.x, s1.y);
    printf("%" PRId64 " %" PRId64 "\n", s2.x, s2.y);
    printf("%" PRId64 " %" PRId64 "\n", s3.x, s3.y);
}

void s16_baz(S16 s0, S16 s1, S16 s2, S16 s3, S16 s4) {
    printf("%" PRId64 " %" PRId64 "\n", s0.x, s0.y);
    printf("%" PRId64 " %" PRId64 "\n", s1.x, s1.y);
    printf("%" PRId64 " %" PRId64 "\n", s2.x, s2.y);
    printf("%" PRId64 " %" PRId64 "\n", s3.x, s3.y);
    printf("%" PRId64 " %" PRId64 "\n", s4.x, s4.y);
}

S16 s16_ret(i64 x, i64 y) {
    return (S16) {.x = x, .y = y};
}

void s24_foo(S24 s) {
    printf("%" PRId64 " %" PRId64 " %" PRId64 "\n", s.x, s.y, s.z);
}

S24 s24_ret(i64 x, i64 y, i64 z) {
    return (S24) {.x = x, .y = y, .z = z};
}

void s3_foo(S3 s) {
    printf("%.*s\n", (int) sizeof(s.xs), s.xs);
}

S3 s3_ret(char xs[3]) {
    S3 s = {0};
    memcpy(s.xs, xs, sizeof(s.xs));
    return s;
}

void s11_foo(S11 s) {
    printf("%.*s\n", (int) sizeof(s.xs), s.xs);
}

S11 s11_ret(char xs[11]) {
    S11 s = {0};
    memcpy(s.xs, xs, sizeof(s.xs));
    return s;
}
