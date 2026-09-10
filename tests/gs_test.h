/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Mark Liversedge */
/*
 * gs_test.h — the conformance harness, vendored on purpose.
 *
 * libgspro links nothing at all, and the suite inherits that: anyone should be
 * able to clone the repo and run every conformance case on a machine with no
 * package manager, no launch monitor and no network.  A test dependency is
 * still a dependency.
 *
 *   GS_TEST(CT_F01_something)   declare a case; the name IS the CT id
 *   GS_ASSERT(cond)             fail and continue to the next case
 *   GS_ASSERT_EQ(a, b)          integer equality, prints both sides
 *   GS_ASSERT_NEAR(a, b, tol)   floating point within tolerance
 *   GS_ASSERT_STR(a, b)         string equality
 *   GS_ASSERT_MEM(a, b, n)      byte-for-byte
 *   GS_BYTES("...")             expands to (const uint8_t *)s, strlen(s)
 *   gs_fixture(name, &len)      load tests/fixtures/<name>; never returns NULL
 *   GS_TEST_MAIN()              run everything registered in this file
 *
 * ⚠ EVERY CASE IS NAMED FOR ITS ROW IN docs/conformance.md §3.  That is the
 * whole point of the naming rule: a failure prints CT_D13 and the reader goes
 * to one table row that says what the case is and WHICH CLIENT demands it.  A
 * case with no row, or a row with no case, is a gap in the suite rather than a
 * matter of taste.
 */
#ifndef GS_TEST_H
#define GS_TEST_H

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct gs_test_case {
    const char *name;
    void (*fn)(void);
} gs_test_case;

#ifndef GS_TEST_MAX
#define GS_TEST_MAX 256
#endif

static gs_test_case gs_test_cases[GS_TEST_MAX];
static int gs_test_count = 0;
static int gs_test_failures = 0;
static int gs_test_current_failed = 0;
static int gs_test_assertions = 0;

static inline void gs_test_register(const char *name, void (*fn)(void))
{
    /* ⚠ ABORT RATHER THAN DROP.  Silently ignoring anything past the cap would
     * retire conformance cases one at a time with the suite still green — a
     * check that has stopped running, which is the exact failure this project
     * is built to avoid. */
    if (gs_test_count >= GS_TEST_MAX) {
        fprintf(stderr, "gs_test: more than %d cases; '%s' would be DROPPED\n", GS_TEST_MAX,
                name);
        abort();
    }
    gs_test_cases[gs_test_count].name = name;
    gs_test_cases[gs_test_count].fn = fn;
    gs_test_count++;
}

/*
 * ⚠ AUTO-REGISTRATION IS THE ONE PART OF THIS HARNESS THAT IS NOT PORTABLE C.
 * A case registers itself before main() so that adding one needs no edit to a
 * list somewhere else — a list is exactly the thing that silently stops
 * covering what it forgot.  GCC and clang have __attribute__((constructor));
 * MSVC has no equivalent, so the pointer goes in the CRT's own pre-main
 * initialiser section, which is the mechanism the C runtime itself uses.
 *
 * ⚠ THE FAILURE MODE IS SILENT, WHICH IS WHY GS_TEST_MAIN() COUNTS.  A suite
 * that registered nothing prints no failures and exits 0, which reads exactly
 * like a suite that passed.  The runner refuses to do that.
 */
#if defined(_MSC_VER)
#pragma section(".CRT$XCU", read)
#define GS_TEST_CTOR_(name)                                                                  \
    static void gs_register_##name(void);                                                    \
    __declspec(allocate(".CRT$XCU")) void (*gs_ctor_##name)(void) = gs_register_##name;      \
    static void gs_register_##name(void)
#else
#define GS_TEST_CTOR_(name)                                                                  \
    __attribute__((constructor)) static void gs_register_##name(void)
#endif

#define GS_TEST(name)                                                                        \
    static void name(void);                                                                  \
    GS_TEST_CTOR_(name)                                                                      \
    {                                                                                        \
        gs_test_register(#name, name);                                                       \
    }                                                                                        \
    static void name(void)

#define GS_FAIL_(fmt, ...)                                                                   \
    do {                                                                                     \
        gs_test_current_failed = 1;                                                          \
        fprintf(stderr, "    FAIL %s:%d: " fmt "\n", __FILE__, __LINE__, __VA_ARGS__);       \
    } while (0)

#define GS_ASSERT(cond)                                                                      \
    do {                                                                                     \
        gs_test_assertions++;                                                                \
        if (!(cond)) {                                                                       \
            GS_FAIL_("%s", #cond);                                                           \
        }                                                                                    \
    } while (0)

#define GS_ASSERT_MSG(cond, msg)                                                             \
    do {                                                                                     \
        gs_test_assertions++;                                                                \
        if (!(cond)) {                                                                       \
            GS_FAIL_("%s  (%s)", #cond, (msg));                                              \
        }                                                                                    \
    } while (0)

#define GS_ASSERT_EQ(a, b)                                                                   \
    do {                                                                                     \
        long long va_ = (long long)(a);                                                      \
        long long vb_ = (long long)(b);                                                      \
        gs_test_assertions++;                                                                \
        if (va_ != vb_) {                                                                    \
            GS_FAIL_("%s == %s  (%lld != %lld)", #a, #b, va_, vb_);                          \
        }                                                                                    \
    } while (0)

#define GS_ASSERT_NEAR(a, b, tol)                                                            \
    do {                                                                                     \
        double va_ = (double)(a);                                                            \
        double vb_ = (double)(b);                                                            \
        double t_ = (double)(tol);                                                           \
        gs_test_assertions++;                                                                \
        if (!(fabs(va_ - vb_) <= t_)) {                                                      \
            GS_FAIL_("%s ~= %s  (%.9g vs %.9g, tol %.9g, diff %.9g)", #a, #b, va_, vb_, t_,  \
                     fabs(va_ - vb_));                                                       \
        }                                                                                    \
    } while (0)

#define GS_ASSERT_STR(a, b)                                                                  \
    do {                                                                                     \
        const char *sa_ = (a);                                                               \
        const char *sb_ = (b);                                                               \
        gs_test_assertions++;                                                                \
        if (sa_ == NULL || sb_ == NULL || strcmp(sa_, sb_) != 0) {                           \
            GS_FAIL_("%s == %s  (\"%s\" != \"%s\")", #a, #b, sa_ ? sa_ : "(null)",           \
                     sb_ ? sb_ : "(null)");                                                  \
        }                                                                                    \
    } while (0)

/* Substring, for a formatted line whose exact layout is not the contract. */
#define GS_ASSERT_CONTAINS(hay, needle)                                                      \
    do {                                                                                     \
        const char *h_ = (hay);                                                              \
        const char *n_ = (needle);                                                           \
        gs_test_assertions++;                                                                \
        if (h_ == NULL || n_ == NULL || strstr(h_, n_) == NULL) {                            \
            GS_FAIL_("%s contains %s  (in \"%s\")", #hay, #needle, h_ ? h_ : "(null)");      \
        }                                                                                    \
    } while (0)

#define GS_ASSERT_NOT_CONTAINS(hay, needle)                                                  \
    do {                                                                                     \
        const char *h_ = (hay);                                                              \
        const char *n_ = (needle);                                                           \
        gs_test_assertions++;                                                                \
        if (h_ != NULL && n_ != NULL && strstr(h_, n_) != NULL) {                            \
            GS_FAIL_("%s must NOT contain %s  (in \"%s\")", #hay, #needle, h_);              \
        }                                                                                    \
    } while (0)

#define GS_ASSERT_MEM(a, b, n)                                                               \
    do {                                                                                     \
        gs_test_assertions++;                                                                \
        if (memcmp((a), (b), (size_t)(n)) != 0) {                                            \
            GS_FAIL_("%s == %s over %zu bytes", #a, #b, (size_t)(n));                        \
        }                                                                                    \
    } while (0)

/* Substring search over bytes that are not NUL-terminated — a write request's
 * payload, say.  memmem() is a GNU extension and this suite is portable C11. */
static inline const void *gs_memfind(const void *hay, size_t hay_len, const char *needle)
{
    size_t n = strlen(needle);
    const unsigned char *h = (const unsigned char *)hay;
    size_t i;
    if (n == 0u || hay_len < n) {
        return NULL;
    }
    for (i = 0; i + n <= hay_len; ++i) {
        if (memcmp(h + i, needle, n) == 0) {
            return h + i;
        }
    }
    return NULL;
}

/* A string literal as the (pointer, length) pair every decode entry point takes. */
#define GS_BYTES(s) ((const uint8_t *)(s)), (strlen(s))

/* ------------------------------------------------------------------------ */
/* Fixtures                                                                  */
/* ------------------------------------------------------------------------ */
/*
 * Loads tests/fixtures/<name> whole.  ⚠ ABORTS rather than returning NULL: a
 * missing fixture is a broken checkout or a broken build definition, and a case
 * that quietly skipped would be indistinguishable from one that passed.
 *
 * The caller owns the buffer; it is NUL-terminated one past `*len` so it can
 * also be used where a C string is wanted.
 */
#ifdef GS_FIXTURE_DIR
static inline unsigned char *gs_fixture(const char *name, size_t *len)
{
    char path[1024];
    FILE *f;
    long size;
    unsigned char *buf;
    size_t got;

    snprintf(path, sizeof(path), "%s/%s", GS_FIXTURE_DIR, name);
    f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "gs_test: fixture '%s' not found at %s\n", name, path);
        abort();
    }
    if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fprintf(stderr, "gs_test: cannot size fixture '%s'\n", name);
        abort();
    }
    buf = (unsigned char *)malloc((size_t)size + 1u);
    if (buf == NULL) {
        abort();
    }
    got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (got != (size_t)size) {
        fprintf(stderr, "gs_test: short read on fixture '%s'\n", name);
        abort();
    }
    buf[got] = '\0';
    *len = got;
    return buf;
}
#endif /* GS_FIXTURE_DIR */

/* ------------------------------------------------------------------------ */
/* Runner                                                                    */
/* ------------------------------------------------------------------------ */
#define GS_TEST_MAIN()                                                                       \
    int main(int argc, char **argv)                                                          \
    {                                                                                        \
        const char *filter = (argc > 1) ? argv[1] : NULL;                                    \
        int ran = 0;                                                                         \
        int i;                                                                               \
        GS_SCAFFOLD_BANNER_();                                                               \
        for (i = 0; i < gs_test_count; ++i) {                                                \
            if (filter && strstr(gs_test_cases[i].name, filter) == NULL) {                   \
                continue;                                                                    \
            }                                                                                \
            gs_test_current_failed = 0;                                                      \
            gs_test_cases[i].fn();                                                           \
            ran++;                                                                           \
            if (gs_test_current_failed) {                                                    \
                gs_test_failures++;                                                          \
                printf("[FAIL] %s\n", gs_test_cases[i].name);                                \
            } else {                                                                         \
                printf("[ ok ] %s\n", gs_test_cases[i].name);                                \
            }                                                                                \
        }                                                                                    \
        printf("%s: %d case(s), %d assertion(s), %d failure(s)\n", argv[0], ran,             \
               gs_test_assertions, gs_test_failures);                                        \
        /* ⚠ A suite that registered nothing must not report success — see the               \
         * note on GS_TEST_CTOR_. */                                                         \
        if (gs_test_count == 0) {                                                            \
            fprintf(stderr, "gs_test: NO CASES REGISTERED — the registration "               \
                            "mechanism did not fire; this is a failure, not a pass\n");      \
            return 2;                                                                        \
        }                                                                                    \
        return gs_test_failures == 0 ? 0 : 1;                                                \
    }

/*
 * ⚠ SAYS OUT LOUD WHEN THE FAILURES ARE EXPECTED.  Kept, and inert: every
 * source group has landed, so nothing defines GS_SCAFFOLDED_GROUPS and no
 * banner prints.  While the library was being written a group's symbols came
 * from a scaffold and every case touching them failed, and naming those groups
 * turned a wall of red into a progress bar — without it a genuine regression is
 * invisible among the not-yet-written.  Whoever splits the sources again gets
 * that back by defining the macro; a green suite prints nothing.
 */
#ifdef GS_SCAFFOLDED_GROUPS
#define GS_SCAFFOLD_BANNER_()                                                                \
    fprintf(stderr,                                                                          \
            "\n⚠ NOT IMPLEMENTED YET: %s.  Cases touching these are EXPECTED to fail;\n"     \
            "  src/gs_unimplemented.c is standing in for them.  docs/design.md §11.\n\n",    \
            GS_SCAFFOLDED_GROUPS)
#else
#define GS_SCAFFOLD_BANNER_() ((void)0)
#endif

#endif /* GS_TEST_H */
