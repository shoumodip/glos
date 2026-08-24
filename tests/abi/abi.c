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

void f4_foo(F4 f) {
    printf("%.7g\n", f.x);
}

F4 f4_ret(float x) {
    return (F4) {.x = x};
}

void f8_foo(F8 f) {
    printf("%.7g %.7g\n", f.x, f.y);
}

F8 f8_ret(float x, float y) {
    return (F8) {.x = x, .y = y};
}

void f12_foo(F12 f) {
    printf("%.7g %.7g %.7g\n", f.x, f.y, f.z);
}

F12 f12_ret(float x, float y, float z) {
    return (F12) {.x = x, .y = y, .z = z};
}

void f16_foo(F16 f) {
    printf("%.7g %.7g %.7g %.7g\n", f.x, f.y, f.z, f.w);
}

F16 f16_ret(float x, float y, float z, float w) {
    return (F16) {.x = x, .y = y, .z = z, .w = w};
}

void d8_foo(D8 d) {
    printf("%.14g\n", d.x);
}

D8 d8_ret(double x) {
    return (D8) {.x = x};
}

void d16_foo(D16 d) {
    printf("%.14g %.14g\n", d.x, d.y);
}

D16 d16_ret(double x, double y) {
    return (D16) {.x = x, .y = y};
}

void d24_foo(D24 d) {
    printf("%.14g %.14g %.14g\n", d.x, d.y, d.z);
}

D24 d24_ret(double x, double y, double z) {
    return (D24) {.x = x, .y = y, .z = z};
}

void d32_foo(D32 d) {
    printf("%.14g %.14g %.14g %.14g\n", d.x, d.y, d.z, d.w);
}

D32 d32_ret(double x, double y, double z, double w) {
    return (D32) {.x = x, .y = y, .z = z, .w = w};
}

void fd16_foo(FD16 f) {
    printf("%.7g %.14g\n", f.x, f.y);
}

FD16 fd16_ret(float x, double y) {
    return (FD16) {.x = x, .y = y};
}

void fs8_foo(FS8 f) {
    printf("%.7g %d\n", f.x, f.y);
}

FS8 fs8_ret(float x, i32 y) {
    return (FS8) {.x = x, .y = y};
}

void ds16_foo(DS16 f) {
    printf("%.7g %" PRId64 "\n", f.x, f.y);
}

DS16 ds16_ret(double x, i64 y) {
    return (DS16) {.x = x, .y = y};
}
