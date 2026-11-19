#ifndef TRESPASSER_STATIC_ASSERT_H
#define TRESPASSER_STATIC_ASSERT_H

/* A compile-time check that every C compiler of the project accepts (MSVC, GCC, Clang, with or without C11). */
#define JPT_STATIC_JOIN_(a, b) a##b
#define JPT_STATIC_JOIN(a, b) JPT_STATIC_JOIN_(a, b)
#define JPT_STATIC_ASSERT(condition, name) typedef char JPT_STATIC_JOIN(jpt_static_assert_##name##_, __LINE__)[(condition) ? 1 : -1]

#endif
