// fixed_point.c - Implementation of Fixed-Point Arithmetic
#include "fixed_point.h"

// Convert integer to fixed-point
fixed_t int_to_fixed(int n) {
    return n * FIXED_SCALE;
}

// Convert fixed-point to integer (rounding down)
int fixed_to_int(fixed_t x) {
    return x / FIXED_SCALE;
}

// Convert fixed-point to integer (rounding to nearest)
int fixed_to_int_round(fixed_t x) {
    return (x + (FIXED_SCALE / 2)) / FIXED_SCALE;
}

// Convert floating-point to fixed-point
fixed_t float_to_fixed(float f) {
    return (fixed_t)(f * FIXED_SCALE);
}

// Convert fixed-point to floating-point
float fixed_to_float(fixed_t x) {
    return (float)x / FIXED_SCALE;
}

// Addition
fixed_t fixed_add(fixed_t a, fixed_t b) {
    return a + b;
}

// Subtraction
fixed_t fixed_sub(fixed_t a, fixed_t b) {
    return a - b;
}

// Multiplication
fixed_t fixed_mul(fixed_t a, fixed_t b) {
    return (fixed_t)(((long long)a * b) >> FIXED_POINT_FRACTIONAL_BITS);
}

// Division
fixed_t fixed_div(fixed_t a, fixed_t b) {
    return (fixed_t)(((long long)a << FIXED_POINT_FRACTIONAL_BITS) / b);
}
