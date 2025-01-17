/*
 * Copyright 2018 The OpenSSL Project Authors. All Rights Reserved.
 *
 * Licensed under the Apache License 2.0 (the "License").  You may not use
 * this file except in compliance with the License.  You can obtain a copy
 * in the file LICENSE in the source distribution or at
 * https://www.openssl.org/source/license.html
 */

/*
 * See SP800-185 "Appendix A - KMAC, .... in Terms of Keccak[c]"
 *
 * Inputs are:
 *    K = Key                  (len(K) < 2^2040 bits)
 *    X = Input
 *    L = Output length        (0 <= L < 2^2040 bits)
 *    S = Customization String Default="" (len(S) < 2^2040 bits)
 *
 * KMAC128(K, X, L, S)
 * {
 *     newX = bytepad(encode_string(K), 168) ||  X || right_encode(L).
 *     T = bytepad(encode_string("KMAC") || encode_string(S), 168).
 *     return KECCAK[256](T || newX || 00, L).
 * }
 *
 * KMAC256(K, X, L, S)
 * {
 *     newX = bytepad(encode_string(K), 136) ||  X || right_encode(L).
 *     T = bytepad(encode_string("KMAC") || encode_string(S), 136).
 *     return KECCAK[512](T || newX || 00, L).
 * }
 *
 * KMAC128XOF(K, X, L, S)
 * {
 *     newX = bytepad(encode_string(K), 168) ||  X || right_encode(0).
 *     T = bytepad(encode_string("KMAC") || encode_string(S), 168).
 *     return KECCAK[256](T || newX || 00, L).
 * }
 *
 * KMAC256XOF(K, X, L, S)
 * {
 *     newX = bytepad(encode_string(K), 136) ||  X || right_encode(0).
 *     T = bytepad(encode_string("KMAC") || encode_string(S), 136).
 *     return KECCAK[512](T || newX || 00, L).
 * }
 *
 */

#include <stdlib.h>
#include <string.h>
#include <openssl/core_numbers.h>
#include <openssl/core_names.h>
#include <openssl/params.h>
#include <openssl/evp.h>
#include <openssl/err.h>

#include "internal/providercommonerr.h"
#include "internal/provider_algs.h"
#include "internal/provider_ctx.h"

/*
 * Forward declaration of everything implemented here.  This is not strictly
 * necessary for the compiler, but provides an assurance that the signatures
 * of the functions in the dispatch table are correct.
 */
static OSSL_OP_mac_newctx_fn kmac128_new;
static OSSL_OP_mac_newctx_fn kmac256_new;
static OSSL_OP_mac_dupctx_fn kmac_dup;
static OSSL_OP_mac_freectx_fn kmac_free;
static OSSL_OP_mac_gettable_ctx_params_fn kmac_gettable_ctx_params;
static OSSL_OP_mac_get_ctx_params_fn kmac_get_ctx_params;
static OSSL_OP_mac_settable_ctx_params_fn kmac_settable_ctx_params;
static OSSL_OP_mac_set_ctx_params_fn kmac_set_ctx_params;
static OSSL_OP_mac_size_fn kmac_size;
static OSSL_OP_mac_init_fn kmac_init;
static OSSL_OP_mac_update_fn kmac_update;
static OSSL_OP_mac_final_fn kmac_final;

#define KMAC_MAX_BLOCKSIZE ((1600 - 128*2) / 8) /* 168 */
#define KMAC_MIN_BLOCKSIZE ((1600 - 256*2) / 8) /* 136 */

/* Length encoding will be  a 1 byte size + length in bits (2 bytes max) */
#define KMAC_MAX_ENCODED_HEADER_LEN 3

/*
 * Custom string max size is chosen such that:
 *   len(encoded_string(custom) + len(kmac_encoded_string) <= KMAC_MIN_BLOCKSIZE
 *   i.e: (KMAC_MAX_CUSTOM + KMAC_MAX_ENCODED_LEN) + 6 <= 136
 */
#define KMAC_MAX_CUSTOM 127

/* Maximum size of encoded custom string */
#define KMAC_MAX_CUSTOM_ENCODED (KMAC_MAX_CUSTOM + KMAC_MAX_ENCODED_HEADER_LEN)

/* Maximum key size in bytes = 2040 / 8 */
#define KMAC_MAX_KEY 255

/*
 * Maximum Encoded Key size will be padded to a multiple of the blocksize
 * i.e KMAC_MAX_KEY + KMAC_MAX_ENCODED_LEN = 258
 * Padded to a multiple of KMAC_MAX_BLOCKSIZE
 */
#define KMAC_MAX_KEY_ENCODED (KMAC_MAX_BLOCKSIZE * 2)

/* Fixed value of encode_string("KMAC") */
static const unsigned char kmac_string[] = {
    0x01, 0x20, 0x4B, 0x4D, 0x41, 0x43
};


#define KMAC_FLAG_XOF_MODE          1

struct kmac_data_st {
    void  *provctx;
    EVP_MD_CTX *ctx;

    /*
     * References to the underlying keccak_kmac implementation.  |md|
     * caches the digest, always.  |alloc_md| only holds a reference to an
     * explicitly fetched digest.
     * |md| is cleared after a EVP_DigestInit call.
     */
    const EVP_MD *md;            /* Cache KMAC digest */
    EVP_MD *alloc_md;            /* Fetched digest */

    size_t out_len;
    int key_len;
    int custom_len;
    /* If xof_mode = 1 then we use right_encode(0) */
    int xof_mode;
    /* key and custom are stored in encoded form */
    unsigned char key[KMAC_MAX_KEY_ENCODED];
    unsigned char custom[KMAC_MAX_CUSTOM_ENCODED];
};

static int encode_string(unsigned char *out, int *out_len,
                         const unsigned char *in, int in_len);
static int right_encode(unsigned char *out, int *out_len, size_t bits);
static int bytepad(unsigned char *out, int *out_len,
                   const unsigned char *in1, int in1_len,
                   const unsigned char *in2, int in2_len,
                   int w);
static int kmac_bytepad_encode_key(unsigned char *out, int *out_len,
                                   const unsigned char *in, int in_len,
                                   int w);

static void kmac_free(void *vmacctx)
{
    struct kmac_data_st *kctx = vmacctx;

    if (kctx != NULL) {
        EVP_MD_CTX_free(kctx->ctx);
        EVP_MD_meth_free(kctx->alloc_md);
        OPENSSL_cleanse(kctx->key, kctx->key_len);
        OPENSSL_cleanse(kctx->custom, kctx->custom_len);
        OPENSSL_free(kctx);
    }
}

/*
 * We have KMAC implemented as a hash, which we can use instead of
 * reimplementing the EVP functionality with direct use of
 * keccak_mac_init() and friends.
 */
static void *kmac_new(void *provctx, EVP_MD *fetched_md, const EVP_MD *md)
{
    struct kmac_data_st *kctx = NULL;

    if (md == NULL)
        return NULL;

    if ((kctx = OPENSSL_zalloc(sizeof(*kctx))) == NULL
            || (kctx->ctx = EVP_MD_CTX_new()) == NULL) {
        kmac_free(kctx);
        return NULL;
    }
    kctx->provctx = provctx;
    kctx->md = md;
    kctx->alloc_md = fetched_md;
    kctx->out_len = EVP_MD_size(md);
    return kctx;
}

static void *kmac_fetch_new(void *provctx, const char *mdname)
{
    EVP_MD *fetched_md = EVP_MD_fetch(PROV_LIBRARY_CONTEXT_OF(provctx),
                                      mdname, NULL);
    const EVP_MD *md = fetched_md;
    void *ret = NULL;

#ifndef FIPS_MODE /* Inside the FIPS module, we don't support legacy digests */
    /* TODO(3.0) BEGIN legacy stuff, to be removed */
    if (md == NULL)
        md = EVP_get_digestbyname(mdname);
    /* TODO(3.0) END of legacy stuff */
#endif

    ret = kmac_new(provctx, fetched_md, md);
    if (ret == NULL)
        EVP_MD_meth_free(fetched_md);
    return ret;
}

static void *kmac128_new(void *provctx)
{
    return kmac_fetch_new(provctx, "KECCAK_KMAC128");
}

static void *kmac256_new(void *provctx)
{
    return kmac_fetch_new(provctx, "KECCAK_KMAC256");
}

static void *kmac_dup(void *vsrc)
{
    struct kmac_data_st *src = vsrc;
    struct kmac_data_st *dst = kmac_new(src->provctx, src->alloc_md, src->md);

    if (dst == NULL)
        return NULL;

    if (!EVP_MD_CTX_copy(dst->ctx, src->ctx)
        || (src->alloc_md != NULL && !EVP_MD_up_ref(src->alloc_md))) {
        kmac_free(dst);
        return NULL;
    }

    dst->md = src->md;
    dst->alloc_md = src->alloc_md;
    dst->out_len = src->out_len;
    dst->key_len = src->key_len;
    dst->custom_len = src->custom_len;
    dst->xof_mode = src->xof_mode;
    memcpy(dst->key, src->key, src->key_len);
    memcpy(dst->custom, src->custom, dst->custom_len);

    return dst;
}

/*
 * The init() assumes that any ctrl methods are set beforehand for
 * md, key and custom. Setting the fields afterwards will have no
 * effect on the output mac.
 */
static int kmac_init(void *vmacctx)
{
    struct kmac_data_st *kctx = vmacctx;
    EVP_MD_CTX *ctx = kctx->ctx;
    unsigned char out[KMAC_MAX_BLOCKSIZE];
    int out_len, block_len;


    /* Check key has been set */
    if (kctx->key_len == 0) {
        EVPerr(EVP_F_KMAC_INIT, EVP_R_NO_KEY_SET);
        return 0;
    }
    if (!EVP_DigestInit_ex(kctx->ctx, kctx->md, NULL))
        return 0;

    block_len = EVP_MD_block_size(kctx->md);

    /* Set default custom string if it is not already set */
    if (kctx->custom_len == 0) {
        const OSSL_PARAM params[] = {
            OSSL_PARAM_octet_string(OSSL_MAC_PARAM_CUSTOM, "", 0),
            OSSL_PARAM_END
        };
        (void)kmac_set_ctx_params(kctx, params);
    }

    return bytepad(out, &out_len, kmac_string, sizeof(kmac_string),
                   kctx->custom, kctx->custom_len, block_len)
           && EVP_DigestUpdate(ctx, out, out_len)
           && EVP_DigestUpdate(ctx, kctx->key, kctx->key_len);
}

static size_t kmac_size(void *vmacctx)
{
    struct kmac_data_st *kctx = vmacctx;

    return kctx->out_len;
}

static int kmac_update(void *vmacctx, const unsigned char *data,
                       size_t datalen)
{
    struct kmac_data_st *kctx = vmacctx;

    return EVP_DigestUpdate(kctx->ctx, data, datalen);
}

static int kmac_final(void *vmacctx, unsigned char *out, size_t *outl,
                      size_t outsize)
{
    struct kmac_data_st *kctx = vmacctx;
    EVP_MD_CTX *ctx = kctx->ctx;
    int lbits, len;
    unsigned char encoded_outlen[KMAC_MAX_ENCODED_HEADER_LEN];
    int ok;

    /* KMAC XOF mode sets the encoded length to 0 */
    lbits = (kctx->xof_mode ? 0 : (kctx->out_len * 8));

    ok = right_encode(encoded_outlen, &len, lbits)
        && EVP_DigestUpdate(ctx, encoded_outlen, len)
        && EVP_DigestFinalXOF(ctx, out, kctx->out_len);
    if (ok && outl != NULL)
        *outl = kctx->out_len;
    return ok;
}

static const OSSL_PARAM known_gettable_ctx_params[] = {
    OSSL_PARAM_size_t(OSSL_MAC_PARAM_OUTLEN, NULL),
    OSSL_PARAM_size_t(OSSL_MAC_PARAM_SIZE, NULL), /* Same as "outlen" */
    OSSL_PARAM_size_t(OSSL_MAC_PARAM_DIGESTSIZE, NULL), /* Same as "outlen" */
    OSSL_PARAM_END
};
static const OSSL_PARAM *kmac_gettable_ctx_params(void)
{
    return known_gettable_ctx_params;
}

static int kmac_get_ctx_params(void *vmacctx, OSSL_PARAM params[])
{
    OSSL_PARAM *p;

    if ((p = OSSL_PARAM_locate(params, OSSL_MAC_PARAM_OUTLEN)) != NULL
        || (p = OSSL_PARAM_locate(params, OSSL_MAC_PARAM_SIZE)) != NULL
        || (p = OSSL_PARAM_locate(params, OSSL_MAC_PARAM_DIGESTSIZE)) != NULL)
        return OSSL_PARAM_set_size_t(p, kmac_size(vmacctx));

    return 1;
}

static const OSSL_PARAM known_settable_ctx_params[] = {
    OSSL_PARAM_int(OSSL_MAC_PARAM_XOF, NULL),
    OSSL_PARAM_size_t(OSSL_MAC_PARAM_OUTLEN, NULL),
    OSSL_PARAM_size_t(OSSL_MAC_PARAM_SIZE, NULL),
    OSSL_PARAM_octet_string(OSSL_MAC_PARAM_KEY, NULL, 0),
    OSSL_PARAM_octet_string(OSSL_MAC_PARAM_CUSTOM, NULL, 0),
    OSSL_PARAM_END
};
static const OSSL_PARAM *kmac_settable_ctx_params(void)
{
    return known_settable_ctx_params;
}

/*
 * The following params can be set any time before final():
 *     - "outlen" or "size":    The requested output length.
 *     - "xof":                 If set, this indicates that right_encoded(0)
 *                              is part of the digested data, otherwise it
 *                              uses right_encoded(requested output length).
 *
 * All other params should be set before init().
 */
static int kmac_set_ctx_params(void *vmacctx, const OSSL_PARAM *params)
{
    struct kmac_data_st *kctx = vmacctx;
    const OSSL_PARAM *p;

    if ((p = OSSL_PARAM_locate_const(params, OSSL_MAC_PARAM_XOF)) != NULL
        && !OSSL_PARAM_get_int(p, &kctx->xof_mode))
        return 0;
    if (((p = OSSL_PARAM_locate_const(params, OSSL_MAC_PARAM_OUTLEN)) != NULL
         ||
         (p = OSSL_PARAM_locate_const(params, OSSL_MAC_PARAM_SIZE)) != NULL)
        && !OSSL_PARAM_get_size_t(p, &kctx->out_len))
        return 0;
    if ((p = OSSL_PARAM_locate_const(params, OSSL_MAC_PARAM_KEY)) != NULL) {
        if (p->data_size < 4 || p->data_size > KMAC_MAX_KEY) {
            ERR_raise(ERR_LIB_PROV, PROV_R_INVALID_KEY_LENGTH);
            return 0;
        }
        if (!kmac_bytepad_encode_key(kctx->key, &kctx->key_len,
                                     p->data, p->data_size,
                                     EVP_MD_block_size(kctx->md)))
            return 0;
    }
    if ((p = OSSL_PARAM_locate_const(params, OSSL_MAC_PARAM_CUSTOM))
        != NULL) {
        if (p->data_size > KMAC_MAX_CUSTOM) {
            ERR_raise(ERR_LIB_PROV, PROV_R_INVALID_CUSTOM_LENGTH);
            return 0;
        }
        if (!encode_string(kctx->custom, &kctx->custom_len,
                           p->data, p->data_size))
            return 0;
    }
    return 1;
}

/*
 * Encoding/Padding Methods.
 */

/* Returns the number of bytes required to store 'bits' into a byte array */
static unsigned int get_encode_size(size_t bits)
{
    unsigned int cnt = 0, sz = sizeof(size_t);

    while (bits && (cnt < sz)) {
        ++cnt;
        bits >>= 8;
    }
    /* If bits is zero 1 byte is required */
    if (cnt == 0)
        cnt = 1;
    return cnt;
}

/*
 * Convert an integer into bytes . The number of bytes is appended
 * to the end of the buffer. Returns an array of bytes 'out' of size
 * *out_len.
 *
 * e.g if bits = 32, out[2] = { 0x20, 0x01 }
 *
 */
static int right_encode(unsigned char *out, int *out_len, size_t bits)
{
    unsigned int len = get_encode_size(bits);
    int i;

    /* The length is constrained to a single byte: 2040/8 = 255 */
    if (len > 0xFF)
        return 0;

    /* MSB's are at the start of the bytes array */
    for (i = len - 1; i >= 0; --i) {
        out[i] = (unsigned char)(bits & 0xFF);
        bits >>= 8;
    }
    /* Tack the length onto the end */
    out[len] = (unsigned char)len;

    /* The Returned length includes the tacked on byte */
    *out_len = len + 1;
    return 1;
}

/*
 * Encodes a string with a left encoded length added. Note that the
 * in_len is converted to bits (*8).
 *
 * e.g- in="KMAC" gives out[6] = { 0x01, 0x20, 0x4B, 0x4D, 0x41, 0x43 }
 *                                 len   bits    K     M     A     C
 */
static int encode_string(unsigned char *out, int *out_len,
                         const unsigned char *in, int in_len)
{
    if (in == NULL) {
        *out_len = 0;
    } else {
        int i, bits, len;

        bits = 8 * in_len;
        len = get_encode_size(bits);
        if (len > 0xFF)
            return 0;

        out[0] = len;
        for (i = len; i > 0; --i) {
            out[i] = (bits & 0xFF);
            bits >>= 8;
        }
        memcpy(out + len + 1, in, in_len);
        *out_len = (1 + len + in_len);
    }
    return 1;
}

/*
 * Returns a zero padded encoding of the inputs in1 and an optional
 * in2 (can be NULL). The padded output must be a multiple of the blocksize 'w'.
 * The value of w is in bytes (< 256).
 *
 * The returned output is:
 *    zero_padded(multiple of w, (left_encode(w) || in1 [|| in2])
 */
static int bytepad(unsigned char *out, int *out_len,
                   const unsigned char *in1, int in1_len,
                   const unsigned char *in2, int in2_len, int w)
{
    int len;
    unsigned char *p = out;
    int sz = w;

    /* Left encoded w */
    *p++ = 1;
    *p++ = w;
    /* || in1 */
    memcpy(p, in1, in1_len);
    p += in1_len;
    /* [ || in2 ] */
    if (in2 != NULL && in2_len > 0) {
        memcpy(p, in2, in2_len);
        p += in2_len;
    }
    /* Figure out the pad size (divisible by w) */
    len = p - out;
    while (len > sz) {
        sz += w;
    }
    /* zero pad the end of the buffer */
    memset(p, 0, sz - len);
    *out_len = sz;
    return 1;
}

/*
 * Returns out = bytepad(encode_string(in), w)
 */
static int kmac_bytepad_encode_key(unsigned char *out, int *out_len,
                                   const unsigned char *in, int in_len,
                                   int w)
{
    unsigned char tmp[KMAC_MAX_KEY + KMAC_MAX_ENCODED_HEADER_LEN];
    int tmp_len;

    if (!encode_string(tmp, &tmp_len, in, in_len))
        return 0;

    return bytepad(out, out_len, tmp, tmp_len, NULL, 0, w);
}

const OSSL_DISPATCH kmac128_functions[] = {
    { OSSL_FUNC_MAC_NEWCTX, (void (*)(void))kmac128_new },
    { OSSL_FUNC_MAC_DUPCTX, (void (*)(void))kmac_dup },
    { OSSL_FUNC_MAC_FREECTX, (void (*)(void))kmac_free },
    { OSSL_FUNC_MAC_INIT, (void (*)(void))kmac_init },
    { OSSL_FUNC_MAC_UPDATE, (void (*)(void))kmac_update },
    { OSSL_FUNC_MAC_FINAL, (void (*)(void))kmac_final },
    { OSSL_FUNC_MAC_GETTABLE_CTX_PARAMS,
      (void (*)(void))kmac_gettable_ctx_params },
    { OSSL_FUNC_MAC_GET_CTX_PARAMS, (void (*)(void))kmac_get_ctx_params },
    { OSSL_FUNC_MAC_SETTABLE_CTX_PARAMS,
      (void (*)(void))kmac_settable_ctx_params },
    { OSSL_FUNC_MAC_SET_CTX_PARAMS, (void (*)(void))kmac_set_ctx_params },
    { 0, NULL }
};

const OSSL_DISPATCH kmac256_functions[] = {
    { OSSL_FUNC_MAC_NEWCTX, (void (*)(void))kmac256_new },
    { OSSL_FUNC_MAC_DUPCTX, (void (*)(void))kmac_dup },
    { OSSL_FUNC_MAC_FREECTX, (void (*)(void))kmac_free },
    { OSSL_FUNC_MAC_INIT, (void (*)(void))kmac_init },
    { OSSL_FUNC_MAC_UPDATE, (void (*)(void))kmac_update },
    { OSSL_FUNC_MAC_FINAL, (void (*)(void))kmac_final },
    { OSSL_FUNC_MAC_GETTABLE_CTX_PARAMS,
      (void (*)(void))kmac_gettable_ctx_params },
    { OSSL_FUNC_MAC_GET_CTX_PARAMS, (void (*)(void))kmac_get_ctx_params },
    { OSSL_FUNC_MAC_SETTABLE_CTX_PARAMS,
      (void (*)(void))kmac_settable_ctx_params },
    { OSSL_FUNC_MAC_SET_CTX_PARAMS, (void (*)(void))kmac_set_ctx_params },
    { 0, NULL }
};
