#pragma once
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define CLAMP(v, lo, hi) MIN(MAX(v, lo), hi)
#define BIT64(n) (1ULL << (n))
/* Zephyr's IS_ENABLED trick: 1 iff the macro is defined to 1. */
#define _XXXX1 _YYYY,
#define IS_ENABLED(m) Z_IS_EN1(m)
#define Z_IS_EN1(m) Z_IS_EN2(_XXXX##m)
#define Z_IS_EN2(x) Z_IS_EN3(x 1, 0)
#define Z_IS_EN3(ignore, val, ...) val
