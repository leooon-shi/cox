#pragma once

#if defined(__GNUC__) || defined(__clang__)
// !!(x) ensure the expression is evaluated as a boolean
#define LIKELY(x) __builtin_expect(!!(x), 1) 
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define LIKELY(x) (x)
#define UNLIKELY(x) (x) 
#endif