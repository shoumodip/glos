#ifndef INT128_H
#define INT128_H

#include "basic.h"

typedef struct {
    u64 low;
    u64 high;
} Int128;

#define INT128_FROM_U64(n) ((Int128) {.low = (u64) (n), .high = 0})
#define INT128_FROM_I64(n) ((Int128) {.low = (u64) (n), .high = ((n) < 0) ? UINT64_MAX : 0})

Int128 int128_from_i64(i64 n);
Int128 int128_from_u64(u64 n);

Int128 int128_add(Int128 a, Int128 b, bool is_signed);
Int128 int128_sub(Int128 a, Int128 b, bool is_signed);
Int128 int128_mul(Int128 a, Int128 b, bool is_signed);
Int128 int128_div(Int128 a, Int128 b, bool is_signed);
Int128 int128_mod(Int128 a, Int128 b, bool is_signed);
Int128 int128_neg(Int128 x);

void int128_divmod(Int128 a, Int128 b, bool is_signed, Int128 *out_quotient, Int128 *out_remainder);

#define INT128_SHIFT_MIN INT128_FROM_U64(0)
#define INT128_SHIFT_MAX INT128_FROM_U64(127)

Int128 int128_shl(Int128 a, Int128 b, bool is_signed);
Int128 int128_shr(Int128 a, Int128 b, bool is_signed);

Int128 int128_and(Int128 a, Int128 b, bool is_signed);
Int128 int128_or(Int128 a, Int128 b, bool is_signed);
Int128 int128_not(Int128 x);

bool int128_gt(Int128 a, Int128 b, bool is_signed);
bool int128_ge(Int128 a, Int128 b, bool is_signed);
bool int128_lt(Int128 a, Int128 b, bool is_signed);
bool int128_le(Int128 a, Int128 b, bool is_signed);
bool int128_eq(Int128 a, Int128 b);
bool int128_ne(Int128 a, Int128 b);

bool int128_is_zero(Int128 n);
bool int128_is_negative(Int128 n);

const char *int128_to_cstr(Int128 n);
double      int128_to_double(Int128 n);

#endif // INT128_H
