#include "int128.h"

Int128 int128_from_i64(i64 n) {
    Int128 result;
    result.low = (u64) n;
    result.high = (n < 0) ? UINT64_MAX : 0;
    return result;
}

Int128 int128_from_u64(u64 n) {
    Int128 result;
    result.low = n;
    result.high = 0;
    return result;
}

Int128 int128_add(Int128 a, Int128 b, bool is_signed) {
    unused(is_signed);
    Int128 result;
    result.low = a.low + b.low;
    result.high = a.high + b.high + (u64) (result.low < a.low);
    return result;
}

Int128 int128_sub(Int128 a, Int128 b, bool is_signed) {
    unused(is_signed);
    Int128 result;
    result.low = a.low - b.low;
    result.high = a.high - b.high - (u64) (a.low < b.low);
    return result;
}

// 64x64 -> 128 multiplication
static void mul64wide(u64 a, u64 b, u64 *hi, u64 *lo) {
    u64 a0 = (uint32_t) a;
    u64 a1 = a >> 32;
    u64 b0 = (uint32_t) b;
    u64 b1 = b >> 32;

    u64 p00 = a0 * b0;
    u64 p01 = a0 * b1;
    u64 p10 = a1 * b0;
    u64 p11 = a1 * b1;

    u64 middle = (p00 >> 32) + (uint32_t) p01 + (uint32_t) p10;
    *lo = (p00 & 0xffffffffULL) | (middle << 32);
    *hi = p11 + (p01 >> 32) + (p10 >> 32) + (middle >> 32);
}

// Unsigned comparison. Returns -1,0,+1
static int int128_cmp_u(Int128 a, Int128 b) {
    if (a.high < b.high) return -1;
    if (a.high > b.high) return 1;
    if (a.low < b.low) return -1;
    if (a.low > b.low) return 1;
    return 0;
}

// Shift left by one bit
static Int128 int128_shl1(Int128 x) {
    Int128 r;
    r.high = (x.high << 1) | (x.low >> 63);
    r.low = x.low << 1;
    return r;
}

// Get bit [0,127]
static bool int128_getbit(Int128 x, unsigned bit) {
    if (bit < 64) return (x.low >> bit) & 1;
    return (x.high >> (bit - 64)) & 1;
}

// Set bit [0,127]
static void int128_setbit(Int128 *x, unsigned bit) {
    if (bit < 64) x->low |= 1ULL << bit;
    else x->high |= 1ULL << (bit - 64);
}

// Negation (two's complement)
Int128 int128_neg(Int128 x) {
    Int128 r;
    r.low = ~x.low + 1;
    r.high = ~x.high;
    if (r.low == 0) r.high++;
    return r;
}

Int128 int128_mul(Int128 a, Int128 b, bool is_signed) {
    bool negate = false;
    if (is_signed) {
        if (int128_is_negative(a)) {
            a = int128_neg(a);
            negate = !negate;
        }

        if (int128_is_negative(b)) {
            b = int128_neg(b);
            negate = !negate;
        }
    }

    u64 lo_lo_hi, lo_lo_lo;
    u64 hi_lo_hi, hi_lo_lo;
    u64 lo_hi_hi, lo_hi_lo;

    // al * bl
    mul64wide(a.low, b.low, &lo_lo_hi, &lo_lo_lo);

    // ah * bl
    mul64wide(a.high, b.low, &hi_lo_hi, &hi_lo_lo);

    // al * bh
    mul64wide(a.low, b.high, &lo_hi_hi, &lo_hi_lo);

    Int128 r;
    r.low = lo_lo_lo;

    //
    //  Low 128 bits are:
    //
    //      lo_lo_lo
    //      lo_lo_hi + hi_lo_lo + lo_hi_lo   (mod 2^64)
    //
    //  Products involving hi_lo_hi, lo_hi_hi, and ah*bh land at
    //  bit 128 or above and are discarded.
    //
    r.high = lo_lo_hi;
    r.high += hi_lo_lo;
    r.high += lo_hi_lo;

    if (negate) r = int128_neg(r);
    return r;
}

Int128 int128_div(Int128 a, Int128 b, bool is_signed) {
    Int128 quotient = {0};
    int128_divmod(a, b, is_signed, &quotient, NULL);
    return quotient;
}

Int128 int128_mod(Int128 a, Int128 b, bool is_signed) {
    Int128 remainder = {0};
    int128_divmod(a, b, is_signed, NULL, &remainder);
    return remainder;
}

void int128_divmod(Int128 a, Int128 b, bool is_signed, Int128 *out_quotient, Int128 *out_remainder) {
    Int128 quotient = {0, 0};
    Int128 remainder = {0, 0};

    bool qneg = false;
    bool rneg = false;

    if (is_signed) {
        if (int128_is_negative(a)) {
            a = int128_neg(a);
            qneg = !qneg;
            rneg = true;
        }

        if (int128_is_negative(b)) {
            b = int128_neg(b);
            qneg = !qneg;
        }
    }

    // Division by zero. Leave behavior to caller if desired
    if (b.high == 0 && b.low == 0) {
        if (out_quotient) *out_quotient = quotient;
        if (out_remainder) *out_remainder = remainder;
        return;
    }

    for (int bit = 127; bit >= 0; --bit) {
        remainder = int128_shl1(remainder);
        if (int128_getbit(a, (unsigned) bit)) {
            remainder.low |= 1;
        }

        if (int128_cmp_u(remainder, b) >= 0) {
            remainder = int128_sub(remainder, b, is_signed);
            int128_setbit(&quotient, (unsigned) bit);
        }
    }

    if (is_signed) {
        if (qneg) quotient = int128_neg(quotient);
        if (rneg) remainder = int128_neg(remainder);
    }

    if (out_quotient) *out_quotient = quotient;
    if (out_remainder) *out_remainder = remainder;
}

Int128 int128_shl(Int128 a, Int128 b, bool is_signed) {
    assert(b.high == 0);
    assert(b.low <= 127);
    const unsigned shift = b.low;

    unused(is_signed);
    Int128 r = {0, 0};
    if (shift >= 128) return r;
    if (shift == 0) return a;
    if (shift >= 64) {
        r.high = a.low << (shift - 64);
        return r;
    }

    r.high = (a.high << shift) | (a.low >> (64 - shift));
    r.low = a.low << shift;
    return r;
}

Int128 int128_shr(Int128 x, Int128 b, bool is_signed) {
    assert(b.high == 0);
    assert(b.low <= 127);
    const unsigned shift = b.low;

    Int128 r;
    if (shift >= 128) {
        if (is_signed && (x.high & 0x8000000000000000ULL)) {
            r.high = UINT64_MAX;
            r.low = UINT64_MAX;
        } else {
            r.high = 0;
            r.low = 0;
        }
        return r;
    }

    if (shift == 0) return x;
    if (shift >= 64) {
        const unsigned s = shift - 64;
        if (is_signed && (x.high & 0x8000000000000000ULL)) {
            r.high = UINT64_MAX;
            if (s == 0) r.low = x.high;
            else r.low = (x.high >> s) | (UINT64_MAX << (64 - s));
        } else {
            r.high = 0;
            if (s == 0) r.low = x.high;
            else r.low = x.high >> s;
        }
        return r;
    }

    r.low = (x.low >> shift) | (x.high << (64 - shift));
    if (is_signed && (x.high & 0x8000000000000000ULL)) r.high = (x.high >> shift) | (UINT64_MAX << (64 - shift));
    else r.high = x.high >> shift;
    return r;
}

Int128 int128_and(Int128 a, Int128 b, bool is_signed) {
    unused(is_signed);
    Int128 r;
    r.low = a.low & b.low;
    r.high = a.high & b.high;
    return r;
}

Int128 int128_or(Int128 a, Int128 b, bool is_signed) {
    unused(is_signed);
    Int128 r;
    r.low = a.low | b.low;
    r.high = a.high | b.high;
    return r;
}

Int128 int128_not(Int128 x) {
    Int128 r;
    r.low = ~x.low;
    r.high = ~x.high;
    return r;
}

static int int128_cmp(Int128 a, Int128 b, bool is_signed) {
    if (is_signed) {
        bool a_neg = (a.high >> 63) != 0;
        bool b_neg = (b.high >> 63) != 0;

        // Different signs
        if (a_neg != b_neg) return a_neg ? -1 : 1;
    }

    // Same sign (or unsigned): compare lexicographically
    if (a.high < b.high) return -1;
    if (a.high > b.high) return 1;
    if (a.low < b.low) return -1;
    if (a.low > b.low) return 1;
    return 0;
}

bool int128_gt(Int128 a, Int128 b, bool is_signed) {
    return int128_cmp(a, b, is_signed) > 0;
}

bool int128_ge(Int128 a, Int128 b, bool is_signed) {
    return int128_cmp(a, b, is_signed) >= 0;
}

bool int128_lt(Int128 a, Int128 b, bool is_signed) {
    return int128_cmp(a, b, is_signed) < 0;
}

bool int128_le(Int128 a, Int128 b, bool is_signed) {
    return int128_cmp(a, b, is_signed) <= 0;
}

bool int128_eq(Int128 a, Int128 b) {
    return a.high == b.high && a.low == b.low;
}

bool int128_ne(Int128 a, Int128 b) {
    return !int128_eq(a, b);
}

bool int128_is_zero(Int128 x) {
    return x.low == 0 && x.high == 0;
}

bool int128_is_negative(Int128 x) {
    return (x.high >> 63) != 0;
}

const char *int128_to_cstr(Int128 n) {
    char   buffer[128];
    size_t head = len(buffer);

    const Int128 zero = int128_from_i64(0);
    const Int128 ten = int128_from_i64(10);

    bool negative = false;
    if (int128_lt(n, zero, true)) {
        negative = true;
        n = int128_neg(n);
    }

    buffer[--head] = '\0';
    if (int128_eq(n, zero)) {
        buffer[--head] = '0';
    } else {
        while (!int128_eq(n, zero)) {
            Int128 quotient = {0};
            Int128 remainder = {0};
            int128_divmod(n, ten, true, &quotient, &remainder);

            assert(head);
            buffer[--head] = '0' + remainder.low;
            n = quotient;
        }
    }

    if (negative) {
        assert(head);
        buffer[--head] = '-';
    }
    return arena_sv_to_cstr(&temp_arena, (SV) {.data = buffer + head, .count = len(buffer) - head});
}

#define TWO_TO_64 (4294967296.0 * 4294967296.0)

double int128_to_double(Int128 n) {
    double low_d = (double) n.low;
    double high_d = (double) (int64_t) n.high;
    return (high_d * TWO_TO_64) + low_d;
}

// This file was vibecoded. Since this is a very common file across multiple codebases, and is just a case of copy
// pasting the implementation, therefore the risk of AI hallucinating is quite low. Nevertheless I tested this against
// the '__int128' type of GCC/Clang, and it seems to work.
