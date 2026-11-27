/* deflateBound_test.c -- test for deflateBound() */

#include "zlib.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond, msg) { \
    if (!(cond)) { \
        fprintf(stderr, "FAILED: %s (line %d)\n", msg, __LINE__); \
        exit(1); \
    } \
}

#define CHECK_ERR(err, msg) { \
    if (err != Z_OK) { \
        fprintf(stderr, "%s error: %d\n", msg, err); \
        exit(1); \
    } \
}

/* Deflate len bytes of pseudo-random-ish data with the given stream and
 * verify the output fits within the bound computed before deflating. */
static void fill(Bytef *buf, uLong len) {
    uLong i;
    unsigned x = 12345;
    for (i = 0; i < len; i++) {
        x = x * 1103515245u + 12345u;
        buf[i] = (Bytef)(x >> 16);
    }
}

static void run_and_check(z_stream *strm, uLong bound, const Bytef *in,
                          uLong len) {
    int err;
    Bytef *out = (Bytef *)malloc(bound ? bound : 1);
    CHECK(out != NULL, "malloc");
    strm->next_in = (Bytef *)in;
    strm->avail_in = (uInt)len;
    strm->next_out = out;
    strm->avail_out = (uInt)bound;
    err = deflate(strm, Z_FINISH);
    CHECK(err == Z_STREAM_END, "deflate Z_FINISH within bound");
    CHECK(strm->total_out <= bound, "output within bound");
    free(out);
}

static void test_null_stream(void) {
    uLong b0, b1, b2;
    b0 = deflateBound(Z_NULL, 0);
    b1 = deflateBound(Z_NULL, 1000);
    b2 = deflateBound(Z_NULL, 1000000);
    CHECK(b0 >= 18, "null stream bound includes wrapper");
    CHECK(b1 >= 1000, "bound >= source");
    CHECK(b2 > b1, "bound grows");

    /* uninitialised stream (state is NULL) */
    {
        z_stream strm;
        memset(&strm, 0, sizeof(strm));
        CHECK(deflateBound(&strm, 1000) == b1, "stream without state");
    }
    /* overflow */
    CHECK(deflateBound(Z_NULL, (uLong)-1) == (uLong)-1, "overflow null");
    CHECK(deflateBound(Z_NULL, (uLong)-1 - 10) == (uLong)-1,
          "overflow null 2");
}

static void test_default(void) {
    z_stream strm;
    uLong len = 20000, bound, bound2;
    Bytef *in = (Bytef *)malloc(len);
    int err;
    fill(in, len);
    memset(&strm, 0, sizeof(strm));
    err = deflateInit(&strm, Z_DEFAULT_COMPRESSION);
    CHECK_ERR(err, "deflateInit");
    bound = deflateBound(&strm, len);
    CHECK(bound >= len, "bound >= len");
    CHECK(bound < deflateBound(Z_NULL, len), "default bound is tighter");
    CHECK(deflateBound(&strm, 0) >= 6, "zero length bound");
    run_and_check(&strm, bound, in, len);
    /* after data has been processed, strstart != 0 -> wrapper +4 */
    bound2 = deflateBound(&strm, len);
    CHECK(bound2 >= bound, "bound after deflate");
    deflateEnd(&strm);
    free(in);
}

static void test_default_after_partial(void) {
    z_stream strm;
    uLong len = 1000, b_before, b_after;
    Bytef *in = (Bytef *)malloc(len);
    Bytef out[2000];
    int err;
    fill(in, len);
    memset(&strm, 0, sizeof(strm));
    err = deflateInit(&strm, 6);
    CHECK_ERR(err, "deflateInit");
    b_before = deflateBound(&strm, len);
    strm.next_in = in;
    strm.avail_in = (uInt)len;
    strm.next_out = out;
    strm.avail_out = sizeof(out);
    err = deflate(&strm, Z_SYNC_FLUSH);
    CHECK_ERR(err, "deflate sync flush");
    b_after = deflateBound(&strm, len);
    CHECK(b_after == b_before + 4, "zlib wrapper adds 4 when strstart != 0");
    deflateEnd(&strm);
    free(in);
}

static void test_raw(void) {
    z_stream strm;
    uLong len = 5000, bound, zbound;
    Bytef *in = (Bytef *)malloc(len);
    int err;
    fill(in, len);
    memset(&strm, 0, sizeof(strm));
    err = deflateInit2(&strm, 6, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY);
    CHECK_ERR(err, "deflateInit2 raw");
    bound = deflateBound(&strm, len);
    run_and_check(&strm, bound, in, len);
    deflateEnd(&strm);

    memset(&strm, 0, sizeof(strm));
    err = deflateInit(&strm, 6);
    CHECK_ERR(err, "deflateInit");
    zbound = deflateBound(&strm, len);
    deflateEnd(&strm);
    CHECK(zbound == bound + 6, "zlib wrapper is 6 bytes larger than raw");
    free(in);
}

static void test_gzip(void) {
    z_stream strm;
    uLong len = 5000, bound, bound_raw;
    Bytef *in = (Bytef *)malloc(len);
    int err;
    fill(in, len);

    memset(&strm, 0, sizeof(strm));
    err = deflateInit2(&strm, 6, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY);
    CHECK_ERR(err, "deflateInit2 raw");
    bound_raw = deflateBound(&strm, len);
    deflateEnd(&strm);

    /* gzip without header */
    memset(&strm, 0, sizeof(strm));
    err = deflateInit2(&strm, 6, Z_DEFLATED, 31, 8, Z_DEFAULT_STRATEGY);
    CHECK_ERR(err, "deflateInit2 gzip");
    bound = deflateBound(&strm, len);
    CHECK(bound == bound_raw + 18, "gzip wrapper is 18 bytes");
    run_and_check(&strm, bound, in, len);
    deflateEnd(&strm);

    /* gzip with header: extra, name, comment, hcrc */
    {
        gz_header head;
        Bytef extra[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
        Bytef name[] = "name.txt";
        Bytef comment[] = "a comment";
        uLong expect;

        memset(&head, 0, sizeof(head));
        head.extra = extra;
        head.extra_len = sizeof(extra);
        head.name = name;
        head.comment = comment;
        head.hcrc = 1;

        memset(&strm, 0, sizeof(strm));
        err = deflateInit2(&strm, 6, Z_DEFLATED, 31, 8, Z_DEFAULT_STRATEGY);
        CHECK_ERR(err, "deflateInit2 gzip");
        err = deflateSetHeader(&strm, &head);
        CHECK_ERR(err, "deflateSetHeader");
        bound = deflateBound(&strm, len);
        expect = bound_raw + 18 + 2 + sizeof(extra) + sizeof(name) +
                 sizeof(comment) + 2;
        CHECK(bound == expect, "gzip header bound");
        run_and_check(&strm, bound, in, len);
        deflateEnd(&strm);
    }

    /* gzip with header with only some fields set */
    {
        gz_header head;
        memset(&head, 0, sizeof(head));
        memset(&strm, 0, sizeof(strm));
        err = deflateInit2(&strm, 6, Z_DEFLATED, 31, 8, Z_DEFAULT_STRATEGY);
        CHECK_ERR(err, "deflateInit2 gzip");
        err = deflateSetHeader(&strm, &head);
        CHECK_ERR(err, "deflateSetHeader");
        bound = deflateBound(&strm, len);
        CHECK(bound == bound_raw + 18, "empty gzip header bound");
        deflateEnd(&strm);
    }
    free(in);
}

static void test_nondefault(void) {
    static const struct { int level, wbits, mem; } cfg[] = {
        {6, 15, 8},   /* default */
        {6, 9, 8},    /* small window: w_bits <= hash_bits, fixed bound */
        {0, 9, 8},    /* level 0 -> stored bound */
        {6, 15, 9},   /* hash_bits 16: fixed bound */
        {6, 15, 1},   /* hash_bits 8: stored bound */
        {1, 12, 4},
        {9, 15, 2},
        {0, 15, 8},
        {3, -12, 5},
        {9, 31, 9},
    };
    size_t i;
    uLong len = 30000;
    Bytef *in = (Bytef *)malloc(len);
    fill(in, len);
    for (i = 0; i < sizeof(cfg) / sizeof(cfg[0]); i++) {
        z_stream strm;
        uLong bound;
        int err;
        memset(&strm, 0, sizeof(strm));
        err = deflateInit2(&strm, cfg[i].level, Z_DEFLATED, cfg[i].wbits,
                           cfg[i].mem, Z_DEFAULT_STRATEGY);
        CHECK_ERR(err, "deflateInit2");
        bound = deflateBound(&strm, len);
        CHECK(bound >= len, "bound >= len");
        run_and_check(&strm, bound, in, len);
        deflateEnd(&strm);
    }
    free(in);
}

static void test_compressible(void) {
    z_stream strm;
    uLong len = 100000, bound;
    Bytef *in = (Bytef *)malloc(len);
    int err;
    memset(in, 'a', len);
    memset(&strm, 0, sizeof(strm));
    err = deflateInit(&strm, Z_BEST_COMPRESSION);
    CHECK_ERR(err, "deflateInit");
    bound = deflateBound(&strm, len);
    run_and_check(&strm, bound, in, len);
    deflateEnd(&strm);
    free(in);
}

static void test_huge(void) {
    z_stream strm;
    int err;
    uLong b;
    memset(&strm, 0, sizeof(strm));
    err = deflateInit(&strm, 6);
    CHECK_ERR(err, "deflateInit");
    b = deflateBound(&strm, (uLong)-1);
    CHECK(b == (uLong)-1, "overflow default");
    deflateEnd(&strm);

    memset(&strm, 0, sizeof(strm));
    err = deflateInit2(&strm, 6, Z_DEFLATED, 9, 8, Z_DEFAULT_STRATEGY);
    CHECK_ERR(err, "deflateInit2");
    CHECK(deflateBound(&strm, (uLong)-1) == (uLong)-1, "overflow fixed");
    CHECK(deflateBound(&strm, (uLong)-1 - 5) == (uLong)-1,
          "overflow fixed + wrapper");
    deflateEnd(&strm);

    memset(&strm, 0, sizeof(strm));
    err = deflateInit2(&strm, 0, Z_DEFLATED, 9, 8, Z_DEFAULT_STRATEGY);
    CHECK_ERR(err, "deflateInit2");
    CHECK(deflateBound(&strm, (uLong)-1) == (uLong)-1, "overflow stored");
    deflateEnd(&strm);
}

int main(void) {
    test_null_stream();
    test_default();
    test_default_after_partial();
    test_raw();
    test_gzip();
    test_nondefault();
    test_compressible();
    test_huge();
    return 0;
}
