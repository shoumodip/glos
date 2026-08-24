#ifndef ABI_H
#define ABI_H

#include <stdint.h>

typedef int32_t i32;
typedef int64_t i64;

// Basics
typedef struct {
    i32 x;
    i32 y;
} S8;

void s8_foo(S8 s);
S8   s8_ret(i32 x, i32 y);

typedef struct {
    i64 x;
    i64 y;
} S16;

void s16_foo(S16 s);
void s16_bar(S16 s0, S16 s1, S16 s2, S16 s3);
void s16_baz(S16 s0, S16 s1, S16 s2, S16 s3, S16 s4);
S16  s16_ret(i64 x, i64 y);

typedef struct {
    i64 x;
    i64 y;
    i64 z;
} S24;

void s24_foo(S24 s);
S24  s24_ret(i64 x, i64 y, i64 z);

// Odd sized values
typedef struct {
    char xs[3];
} S3;

void s3_foo(S3 s);
S3   s3_ret(char xs[3]);

typedef struct {
    char xs[11];
} S11;

void s11_foo(S11 s);
S11  s11_ret(char xs[11]);

#endif // ABI_H
