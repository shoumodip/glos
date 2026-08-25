#ifndef ABI_H
#define ABI_H

#include <stdint.h>

typedef int32_t s32;
typedef int64_t s64;

// Basics
typedef struct {
    s32 x;
    s32 y;
} S8;

void s8_foo(S8 s);
S8   s8_ret(s32 x, s32 y);

typedef struct {
    s64 x;
    s64 y;
} S16;

void s16_foo(S16 s);
void s16_bar(S16 s0, S16 s1, S16 s2, S16 s3);
void s16_baz(S16 s0, S16 s1, S16 s2, S16 s3, S16 s4);
S16  s16_ret(s64 x, s64 y);

typedef struct {
    s64 x;
    s64 y;
    s64 z;
} S24;

void s24_foo(S24 s);
S24  s24_ret(s64 x, s64 y, s64 z);

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

// Floats
typedef struct {
    float x;
} F4;

void f4_foo(F4 f);
F4   f4_ret(float x);

typedef struct {
    float x, y;
} F8;

void f8_foo(F8 f);
F8   f8_ret(float x, float y);

typedef struct {
    float x, y, z;
} F12;

void f12_foo(F12 f);
F12  f12_ret(float x, float y, float z);

typedef struct {
    float x, y, z, w;
} F16;

void f16_foo(F16 f);
F16  f16_ret(float x, float y, float z, float w);

typedef struct {
    double x;
} D8;

void d8_foo(D8 d);
D8   d8_ret(double x);

typedef struct {
    double x, y;
} D16;

void d16_foo(D16 d);
D16  d16_ret(double x, double y);

typedef struct {
    double x, y, z;
} D24;

void d24_foo(D24 d);
D24  d24_ret(double x, double y, double z);

typedef struct {
    double x, y, z, w;
} D32;

void d32_foo(D32 d);
D32  d32_ret(double x, double y, double z, double w);

typedef struct {
    float  x;
    double y;
} FD16;

void fd16_foo(FD16 f);
FD16 fd16_ret(float x, double y);

typedef struct {
    float x;
    s32   y;
} FS8;

void fs8_foo(FS8 f);
FS8  fs8_ret(float x, s32 y);

typedef struct {
    double x;
    s64    y;
} DS16;

void ds16_foo(DS16 f);
DS16 ds16_ret(double x, s64 y);

#endif // ABI_H
