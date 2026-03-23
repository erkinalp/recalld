/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ENCRYPTION_KEY_SIZE 32   /* AES-256 */
#define ENCRYPTION_IV_SIZE  12   /* GCM nonce */
#define ENCRYPTION_TAG_SIZE 16   /* GCM auth tag */

typedef struct EncryptionContext {
        uint8_t key[ENCRYPTION_KEY_SIZE];
        bool initialized;
} EncryptionContext;

int encryption_init(EncryptionContext *ctx, const uint8_t *key, size_t key_len);
void encryption_cleanup(EncryptionContext *ctx);

int encryption_derive_key(
                const char *passphrase,
                const uint8_t *salt,
                size_t salt_len,
                int iterations,
                uint8_t *ret_key,
                size_t key_len);

/* Encrypt data using AES-256-GCM.
 * ret_out must have capacity for in_len + ENCRYPTION_IV_SIZE + ENCRYPTION_TAG_SIZE.
 * Returns total output length on success, negative errno on failure. */
int encryption_encrypt(
                EncryptionContext *ctx,
                const uint8_t *in,
                size_t in_len,
                uint8_t *ret_out,
                size_t out_cap);

/* Decrypt data encrypted with encryption_encrypt.
 * ret_out must have capacity for in_len - ENCRYPTION_IV_SIZE - ENCRYPTION_TAG_SIZE.
 * Returns plaintext length on success, negative errno on failure. */
int encryption_decrypt(
                EncryptionContext *ctx,
                const uint8_t *in,
                size_t in_len,
                uint8_t *ret_out,
                size_t out_cap);

int encryption_random_bytes(uint8_t *buf, size_t len);
