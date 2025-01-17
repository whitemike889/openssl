/*
 * Copyright 2018 The OpenSSL Project Authors. All Rights Reserved.
 *
 * Licensed under the OpenSSL license (the "License").  You may not use
 * this file except in compliance with the License.  You can obtain a copy
 * in the file LICENSE in the source distribution or at
 * https://www.openssl.org/source/license.html
 */

#include <string.h>
#include <stdlib.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/core_names.h>
#include <openssl/params.h>
#include "internal/modes_int.h"
#include "internal/siv_int.h"

#ifndef OPENSSL_NO_SIV

__owur static ossl_inline uint32_t rotl8(uint32_t x)
{
    return (x << 8) | (x >> 24);
}

__owur static ossl_inline uint32_t rotr8(uint32_t x)
{
    return (x >> 8) | (x << 24);
}

__owur static ossl_inline uint64_t byteswap8(uint64_t x)
{
    uint32_t high = (uint32_t)(x >> 32);
    uint32_t low = (uint32_t)x;

    high = (rotl8(high) & 0x00ff00ff) | (rotr8(high) & 0xff00ff00);
    low = (rotl8(low) & 0x00ff00ff) | (rotr8(low) & 0xff00ff00);
    return ((uint64_t)low) << 32 | (uint64_t)high;
}

__owur static ossl_inline uint64_t siv128_getword(SIV_BLOCK const *b, size_t i)
{
    const union {
        long one;
        char little;
    } is_endian = { 1 };

    if (is_endian.little)
        return byteswap8(b->word[i]);
    return b->word[i];
}

static ossl_inline void siv128_putword(SIV_BLOCK *b, size_t i, uint64_t x)
{
    const union {
        long one;
        char little;
    } is_endian = { 1 };

    if (is_endian.little)
        b->word[i] = byteswap8(x);
    else
        b->word[i] = x;
}

static ossl_inline void siv128_xorblock(SIV_BLOCK *x,
                                        SIV_BLOCK const *y)
{
    x->word[0] ^= y->word[0];
    x->word[1] ^= y->word[1];
}

/*
 * Doubles |b|, which is 16 bytes representing an element
 * of GF(2**128) modulo the irreducible polynomial
 * x**128 + x**7 + x**2 + x + 1.
 * Assumes two's-complement arithmetic
 */
static ossl_inline void siv128_dbl(SIV_BLOCK *b)
{
    uint64_t high = siv128_getword(b, 0);
    uint64_t low = siv128_getword(b, 1);
    uint64_t high_carry = high & (((uint64_t)1) << 63);
    uint64_t low_carry = low & (((uint64_t)1) << 63);
    int64_t low_mask = -((int64_t)(high_carry >> 63)) & 0x87;
    uint64_t high_mask = low_carry >> 63;

    high = (high << 1) | high_mask;
    low = (low << 1) ^ (uint64_t)low_mask;
    siv128_putword(b, 0, high);
    siv128_putword(b, 1, low);
}

__owur static ossl_inline int siv128_do_s2v_p(SIV128_CONTEXT *ctx, SIV_BLOCK *out,
                                              unsigned char const* in, size_t len)
{
    SIV_BLOCK t;
    size_t out_len = sizeof(out->byte);
    EVP_MAC_CTX *mac_ctx;
    int ret = 0;

    mac_ctx = EVP_MAC_CTX_dup(ctx->mac_ctx_init);
    if (mac_ctx == NULL)
        return 0;

    if (len >= SIV_LEN) {
        if (!EVP_MAC_update(mac_ctx, in, len - SIV_LEN))
            goto err;
        memcpy(&t, in + (len-SIV_LEN), SIV_LEN);
        siv128_xorblock(&t, &ctx->d);
        if (!EVP_MAC_update(mac_ctx, t.byte, SIV_LEN))
            goto err;
    } else {
        memset(&t, 0, sizeof(t));
        memcpy(&t, in, len);
        t.byte[len] = 0x80;
        siv128_dbl(&ctx->d);
        siv128_xorblock(&t, &ctx->d);
        if (!EVP_MAC_update(mac_ctx, t.byte, SIV_LEN))
            goto err;
    }
    if (!EVP_MAC_final(mac_ctx, out->byte, &out_len, sizeof(out->byte))
        || out_len != SIV_LEN)
        goto err;

    ret = 1;

err:
    EVP_MAC_CTX_free(mac_ctx);
    return ret;
}


__owur static ossl_inline int siv128_do_encrypt(EVP_CIPHER_CTX *ctx, unsigned char *out,
                                             unsigned char const *in, size_t len,
                                             SIV_BLOCK *icv)
{
    int out_len = (int)len;

    if (!EVP_CipherInit_ex(ctx, NULL, NULL, NULL, icv->byte, 1))
        return 0;
    return EVP_EncryptUpdate(ctx, out, &out_len, in, out_len);
}

/*
 * Create a new SIV128_CONTEXT
 */
SIV128_CONTEXT *CRYPTO_siv128_new(const unsigned char *key, int klen, EVP_CIPHER* cbc, EVP_CIPHER* ctr)
{
    SIV128_CONTEXT *ctx;
    int ret;

    if ((ctx = OPENSSL_malloc(sizeof(*ctx))) != NULL) {
        ret = CRYPTO_siv128_init(ctx, key, klen, cbc, ctr);
        if (ret)
            return ctx;
        OPENSSL_free(ctx);
    }

    return NULL;
}

/*
 * Initialise an existing SIV128_CONTEXT
 */
int CRYPTO_siv128_init(SIV128_CONTEXT *ctx, const unsigned char *key, int klen,
                       const EVP_CIPHER* cbc, const EVP_CIPHER* ctr)
{
    static const unsigned char zero[SIV_LEN] = { 0 };
    size_t out_len = SIV_LEN;
    EVP_MAC_CTX *mac_ctx = NULL;
    OSSL_PARAM params[3];
    const char *cbc_name = EVP_CIPHER_name(cbc);

    params[0] = OSSL_PARAM_construct_utf8_string(OSSL_MAC_PARAM_ALGORITHM,
                                                 (char *)cbc_name,
                                                 strlen(cbc_name) + 1);
    params[1] = OSSL_PARAM_construct_octet_string(OSSL_MAC_PARAM_KEY,
                                                  (void *)key, klen);
    params[2] = OSSL_PARAM_construct_end();

    memset(&ctx->d, 0, sizeof(ctx->d));
    ctx->cipher_ctx = NULL;
    ctx->mac_ctx_init = NULL;

    if (key == NULL || cbc == NULL || ctr == NULL
            || (ctx->cipher_ctx = EVP_CIPHER_CTX_new()) == NULL
            /* TODO(3.0) library context */
            || (ctx->mac = EVP_MAC_fetch(NULL, "CMAC", NULL)) == NULL
            || (ctx->mac_ctx_init = EVP_MAC_CTX_new(ctx->mac)) == NULL
            || !EVP_MAC_CTX_set_params(ctx->mac_ctx_init, params)
            || !EVP_EncryptInit_ex(ctx->cipher_ctx, ctr, NULL, key + klen, NULL)
            || (mac_ctx = EVP_MAC_CTX_dup(ctx->mac_ctx_init)) == NULL
            || !EVP_MAC_update(mac_ctx, zero, sizeof(zero))
            || !EVP_MAC_final(mac_ctx, ctx->d.byte, &out_len,
                              sizeof(ctx->d.byte))) {
        EVP_CIPHER_CTX_free(ctx->cipher_ctx);
        EVP_MAC_CTX_free(ctx->mac_ctx_init);
        EVP_MAC_CTX_free(mac_ctx);
        EVP_MAC_free(ctx->mac);
        return 0;
    }
    EVP_MAC_CTX_free(mac_ctx);

    ctx->final_ret = -1;
    ctx->crypto_ok = 1;

    return 1;
}

/*
 * Copy an SIV128_CONTEXT object
 */
int CRYPTO_siv128_copy_ctx(SIV128_CONTEXT *dest, SIV128_CONTEXT *src)
{
    memcpy(&dest->d, &src->d, sizeof(src->d));
    if (!EVP_CIPHER_CTX_copy(dest->cipher_ctx, src->cipher_ctx))
        return 0;
    EVP_MAC_CTX_free(dest->mac_ctx_init);
    dest->mac_ctx_init = EVP_MAC_CTX_dup(src->mac_ctx_init);
    if (dest->mac_ctx_init == NULL)
        return 0;
    return 1;
}

/*
 * Provide any AAD. This can be called multiple times.
 * Per RFC5297, the last piece of associated data
 * is the nonce, but it's not treated special
 */
int CRYPTO_siv128_aad(SIV128_CONTEXT *ctx, const unsigned char *aad,
                      size_t len)
{
    SIV_BLOCK mac_out;
    size_t out_len = SIV_LEN;
    EVP_MAC_CTX *mac_ctx;

    siv128_dbl(&ctx->d);

    if ((mac_ctx = EVP_MAC_CTX_dup(ctx->mac_ctx_init)) == NULL
        || !EVP_MAC_update(mac_ctx, aad, len)
        || !EVP_MAC_final(mac_ctx, mac_out.byte, &out_len,
                          sizeof(mac_out.byte))
        || out_len != SIV_LEN) {
        EVP_MAC_CTX_free(mac_ctx);
        return 0;
    }
    EVP_MAC_CTX_free(mac_ctx);

    siv128_xorblock(&ctx->d, &mac_out);

    return 1;
}

/*
 * Provide any data to be encrypted. This can be called once.
 */
int CRYPTO_siv128_encrypt(SIV128_CONTEXT *ctx,
                          const unsigned char *in, unsigned char *out,
                          size_t len)
{
    SIV_BLOCK q;

    /* can only do one crypto operation */
    if (ctx->crypto_ok == 0)
        return 0;
    ctx->crypto_ok--;

    if (!siv128_do_s2v_p(ctx, &q, in, len))
        return 0;

    memcpy(ctx->tag.byte, &q, SIV_LEN);
    q.byte[8] &= 0x7f;
    q.byte[12] &= 0x7f;

    if (!siv128_do_encrypt(ctx->cipher_ctx, out, in, len, &q))
        return 0;
    ctx->final_ret = 0;
    return len;
}

/*
 * Provide any data to be decrypted. This can be called once.
 */
int CRYPTO_siv128_decrypt(SIV128_CONTEXT *ctx,
                          const unsigned char *in, unsigned char *out,
                          size_t len)
{
    unsigned char* p;
    SIV_BLOCK t, q;
    int i;

    /* can only do one crypto operation */
    if (ctx->crypto_ok == 0)
        return 0;
    ctx->crypto_ok--;

    memcpy(&q, ctx->tag.byte, SIV_LEN);
    q.byte[8] &= 0x7f;
    q.byte[12] &= 0x7f;

    if (!siv128_do_encrypt(ctx->cipher_ctx, out, in, len, &q)
        || !siv128_do_s2v_p(ctx, &t, out, len))
        return 0;

    p = ctx->tag.byte;
    for (i = 0; i < SIV_LEN; i++)
        t.byte[i] ^= p[i];

    if ((t.word[0] | t.word[1]) != 0) {
        OPENSSL_cleanse(out, len);
        return 0;
    }
    ctx->final_ret = 0;
    return len;
}

/*
 * Return the already calculated final result.
 */
int CRYPTO_siv128_finish(SIV128_CONTEXT *ctx)
{
    return ctx->final_ret;
}

/*
 * Set the tag
 */
int CRYPTO_siv128_set_tag(SIV128_CONTEXT *ctx, const unsigned char *tag, size_t len)
{
    if (len != SIV_LEN)
        return 0;

    /* Copy the tag from the supplied buffer */
    memcpy(ctx->tag.byte, tag, len);
    return 1;
}

/*
 * Retrieve the calculated tag
 */
int CRYPTO_siv128_get_tag(SIV128_CONTEXT *ctx, unsigned char *tag, size_t len)
{
    if (len != SIV_LEN)
        return 0;

    /* Copy the tag into the supplied buffer */
    memcpy(tag, ctx->tag.byte, len);
    return 1;
}

/*
 * Release all resources
 */
int CRYPTO_siv128_cleanup(SIV128_CONTEXT *ctx)
{
    if (ctx != NULL) {
        EVP_CIPHER_CTX_free(ctx->cipher_ctx);
        ctx->cipher_ctx = NULL;
        EVP_MAC_CTX_free(ctx->mac_ctx_init);
        ctx->mac_ctx_init = NULL;
        EVP_MAC_free(ctx->mac);
        ctx->mac = NULL;
        OPENSSL_cleanse(&ctx->d, sizeof(ctx->d));
        OPENSSL_cleanse(&ctx->tag, sizeof(ctx->tag));
        ctx->final_ret = -1;
        ctx->crypto_ok = 1;
    }
    return 1;
}

int CRYPTO_siv128_speed(SIV128_CONTEXT *ctx, int arg)
{
    ctx->crypto_ok = (arg == 1) ? -1 : 1;
    return 1;
}

#endif                          /* OPENSSL_NO_SIV */
