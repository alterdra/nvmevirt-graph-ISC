// fixed_point.h - Fixed-Point Arithmetic Library
#ifndef FIXED_POINT_H
#define FIXED_POINT_H

#include <linux/types.h>

// Fixed-point format: Q16.16 (16 integer bits, 16 fractional bits)
typedef int fixed_t;
#define FIXED_POINT_FRACTIONAL_BITS 16
#define FIXED_SCALE (1 << FIXED_POINT_FRACTIONAL_BITS)  // 2^16 = 65536

// Conversion functions
fixed_t int_to_fixed(int n);
int fixed_to_int(fixed_t x);
int fixed_to_int_round(fixed_t x);
fixed_t float_to_fixed(float f);
float fixed_to_float(fixed_t x);

// Arithmetic operations
fixed_t fixed_add(fixed_t a, fixed_t b);
fixed_t fixed_sub(fixed_t a, fixed_t b);
fixed_t fixed_mul(fixed_t a, fixed_t b);
fixed_t fixed_div(fixed_t a, fixed_t b);

#endif // FIXED_POINT_H