/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <errno.h>
#include <string.h>

#include <openssl/evp.h>
#include <openssl/rand.h>

#include "common/recalld-encryption.h"
#include "common/recalld-log.h"

int encryption_init(EncryptionContext *ctx, const uint8_t *key, size_t key_len) {
        if (!ctx || !key)
                return -EINVAL;
        if (key_len != ENCRYPTION_KEY_SIZE)
                return -EINVAL;

        memcpy(ctx->key, key, ENCRYPTION_KEY_SIZE);
        ctx->initialized = true;

        return 0;
}

void encryption_cleanup(EncryptionContext *ctx) {
        if (!ctx)
                return;

        explicit_bzero(ctx->key, ENCRYPTION_KEY_SIZE);
        ctx->initialized = false;
}

int encryption_derive_key(
                const char *passphrase,
                const uint8_t *salt,
                size_t salt_len,
                int iterations,
                uint8_t *ret_key,
                size_t key_len) {

        if (!passphrase || !salt || !ret_key)
                return -EINVAL;

        if (!PKCS5_PBKDF2_HMAC(
                        passphrase, -1,
                        salt, (int) salt_len,
                        iterations,
                        EVP_sha256(),
                        (int) key_len, ret_key))
                return log_error_errno(EIO, "Failed to derive encryption key: %m");

        return 0;
}

int encryption_encrypt(
                EncryptionContext *ctx,
                const uint8_t *in,
                size_t in_len,
                uint8_t *ret_out,
                size_t out_cap) {

        EVP_CIPHER_CTX *evp_ctx = NULL;
        uint8_t iv[ENCRYPTION_IV_SIZE];
        uint8_t tag[ENCRYPTION_TAG_SIZE];
        int len, ciphertext_len;
        size_t total;
        int r;

        if (!ctx || !ctx->initialized)
                return -EINVAL;
        if (!in || !ret_out)
                return -EINVAL;

        total = in_len + ENCRYPTION_IV_SIZE + ENCRYPTION_TAG_SIZE;
        if (out_cap < total)
                return -ENOBUFS;

        /* Generate random IV */
        if (RAND_bytes(iv, ENCRYPTION_IV_SIZE) != 1)
                return log_error_errno(EIO, "Failed to generate random IV: %m");

        evp_ctx = EVP_CIPHER_CTX_new();
        if (!evp_ctx)
                return log_oom(), -ENOMEM;

        r = 0;

        if (EVP_EncryptInit_ex(evp_ctx, EVP_aes_256_gcm(), /* impl= */ NULL, ctx->key, iv) != 1) {
                r = -EIO;
                goto finish;
        }

        if (EVP_EncryptUpdate(evp_ctx, ret_out + ENCRYPTION_IV_SIZE, &len, in, (int) in_len) != 1) {
                r = -EIO;
                goto finish;
        }
        ciphertext_len = len;

        if (EVP_EncryptFinal_ex(evp_ctx, ret_out + ENCRYPTION_IV_SIZE + ciphertext_len, &len) != 1) {
                r = -EIO;
                goto finish;
        }
        ciphertext_len += len;

        if (EVP_CIPHER_CTX_ctrl(evp_ctx, EVP_CTRL_GCM_GET_TAG, ENCRYPTION_TAG_SIZE, tag) != 1) {
                r = -EIO;
                goto finish;
        }

        /* Output format: IV || ciphertext || tag */
        memcpy(ret_out, iv, ENCRYPTION_IV_SIZE);
        memcpy(ret_out + ENCRYPTION_IV_SIZE + ciphertext_len, tag, ENCRYPTION_TAG_SIZE);
        r = (int)(ENCRYPTION_IV_SIZE + ciphertext_len + ENCRYPTION_TAG_SIZE);

finish:
        EVP_CIPHER_CTX_free(evp_ctx);
        return r;
}

int encryption_decrypt(
                EncryptionContext *ctx,
                const uint8_t *in,
                size_t in_len,
                uint8_t *ret_out,
                size_t out_cap) {

        EVP_CIPHER_CTX *evp_ctx = NULL;
        const uint8_t *iv, *ciphertext, *tag;
        size_t ciphertext_len;
        int len, plaintext_len;
        int r;

        if (!ctx || !ctx->initialized)
                return -EINVAL;
        if (!in || !ret_out)
                return -EINVAL;
        if (in_len < ENCRYPTION_IV_SIZE + ENCRYPTION_TAG_SIZE)
                return -EINVAL;

        iv = in;
        ciphertext = in + ENCRYPTION_IV_SIZE;
        ciphertext_len = in_len - ENCRYPTION_IV_SIZE - ENCRYPTION_TAG_SIZE;
        tag = in + in_len - ENCRYPTION_TAG_SIZE;

        if (out_cap < ciphertext_len)
                return -ENOBUFS;

        evp_ctx = EVP_CIPHER_CTX_new();
        if (!evp_ctx)
                return log_oom(), -ENOMEM;

        r = 0;

        if (EVP_DecryptInit_ex(evp_ctx, EVP_aes_256_gcm(), /* impl= */ NULL, ctx->key, iv) != 1) {
                r = -EIO;
                goto finish;
        }

        if (EVP_DecryptUpdate(evp_ctx, ret_out, &len, ciphertext, (int) ciphertext_len) != 1) {
                r = -EIO;
                goto finish;
        }
        plaintext_len = len;

        if (EVP_CIPHER_CTX_ctrl(evp_ctx, EVP_CTRL_GCM_SET_TAG, ENCRYPTION_TAG_SIZE, (void*) tag) != 1) {
                r = -EIO;
                goto finish;
        }

        if (EVP_DecryptFinal_ex(evp_ctx, ret_out + plaintext_len, &len) != 1) {
                r = -EACCES; /* authentication failed */
                goto finish;
        }
        plaintext_len += len;
        r = plaintext_len;

finish:
        EVP_CIPHER_CTX_free(evp_ctx);
        return r;
}

int encryption_random_bytes(uint8_t *buf, size_t len) {
        if (!buf)
                return -EINVAL;

        if (RAND_bytes(buf, (int) len) != 1)
                return -EIO;

        return 0;
}
