/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <openssl/evp.h>
#include <openssl/hmac.h>

#include "server/jwt.h"
#include "common/recalld-log.h"

struct JwtContext {
        uint8_t *secret;
        size_t secret_len;
};

/* Simple base64url encoding */
static int base64url_encode(const uint8_t *in, size_t in_len, char *out, size_t out_cap) {
        static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
        size_t i, j;

        if (out_cap < (in_len + 2) / 3 * 4 + 1)
                return -ENOBUFS;

        for (i = 0, j = 0; i < in_len; i += 3) {
                uint32_t n = ((uint32_t) in[i]) << 16;

                if (i + 1 < in_len)
                        n |= ((uint32_t) in[i + 1]) << 8;
                if (i + 2 < in_len)
                        n |= (uint32_t) in[i + 2];

                out[j++] = table[(n >> 18) & 0x3f];
                out[j++] = table[(n >> 12) & 0x3f];

                if (i + 1 < in_len)
                        out[j++] = table[(n >> 6) & 0x3f];
                if (i + 2 < in_len)
                        out[j++] = table[n & 0x3f];
        }
        out[j] = '\0';

        return (int) j;
}

/* Simple base64url decoding */
static int base64url_decode(const char *in, size_t in_len, uint8_t *out, size_t out_cap) {
        static const uint8_t table[256] = {
                ['A'] = 0,  ['B'] = 1,  ['C'] = 2,  ['D'] = 3,
                ['E'] = 4,  ['F'] = 5,  ['G'] = 6,  ['H'] = 7,
                ['I'] = 8,  ['J'] = 9,  ['K'] = 10, ['L'] = 11,
                ['M'] = 12, ['N'] = 13, ['O'] = 14, ['P'] = 15,
                ['Q'] = 16, ['R'] = 17, ['S'] = 18, ['T'] = 19,
                ['U'] = 20, ['V'] = 21, ['W'] = 22, ['X'] = 23,
                ['Y'] = 24, ['Z'] = 25,
                ['a'] = 26, ['b'] = 27, ['c'] = 28, ['d'] = 29,
                ['e'] = 30, ['f'] = 31, ['g'] = 32, ['h'] = 33,
                ['i'] = 34, ['j'] = 35, ['k'] = 36, ['l'] = 37,
                ['m'] = 38, ['n'] = 39, ['o'] = 40, ['p'] = 41,
                ['q'] = 42, ['r'] = 43, ['s'] = 44, ['t'] = 45,
                ['u'] = 46, ['v'] = 47, ['w'] = 48, ['x'] = 49,
                ['y'] = 50, ['z'] = 51,
                ['0'] = 52, ['1'] = 53, ['2'] = 54, ['3'] = 55,
                ['4'] = 56, ['5'] = 57, ['6'] = 58, ['7'] = 59,
                ['8'] = 60, ['9'] = 61,
                ['-'] = 62, ['_'] = 63,
        };
        size_t i, j;

        if (out_cap < in_len * 3 / 4)
                return -ENOBUFS;

        for (i = 0, j = 0; i < in_len; i += 4) {
                uint32_t n = 0;
                int pad = 0;

                n = (uint32_t) table[(unsigned char) in[i]] << 18;
                if (i + 1 < in_len)
                        n |= (uint32_t) table[(unsigned char) in[i + 1]] << 12;
                else
                        pad++;
                if (i + 2 < in_len)
                        n |= (uint32_t) table[(unsigned char) in[i + 2]] << 6;
                else
                        pad++;
                if (i + 3 < in_len)
                        n |= (uint32_t) table[(unsigned char) in[i + 3]];
                else
                        pad++;

                out[j++] = (uint8_t)((n >> 16) & 0xff);
                if (pad < 2)
                        out[j++] = (uint8_t)((n >> 8) & 0xff);
                if (pad < 1)
                        out[j++] = (uint8_t)(n & 0xff);
        }

        return (int) j;
}

int jwt_new(JwtContext **ret, const char *secret_file) {
        JwtContext *ctx;
        FILE *f;
        uint8_t buf[4096];
        size_t n;

        if (!ret || !secret_file)
                return -EINVAL;

        f = fopen(secret_file, "rbe");
        if (!f)
                return log_error_errno(errno, "Failed to open JWT secret file %s: %m", secret_file);

        n = fread(buf, 1, sizeof(buf), f);
        fclose(f);

        if (n == 0)
                return log_error_errno(EIO, "JWT secret file is empty.");

        /* Strip trailing newline */
        while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r'))
                n--;

        ctx = calloc(1, sizeof(JwtContext));
        if (!ctx)
                return log_oom(), -ENOMEM;

        ctx->secret = malloc(n);
        if (!ctx->secret) {
                free(ctx);
                return log_oom(), -ENOMEM;
        }

        memcpy(ctx->secret, buf, n);
        ctx->secret_len = n;
        explicit_bzero(buf, sizeof(buf));

        *ret = ctx;
        return 0;
}

JwtContext* jwt_free(JwtContext *ctx) {
        if (!ctx)
                return NULL;

        if (ctx->secret) {
                explicit_bzero(ctx->secret, ctx->secret_len);
                free(ctx->secret);
        }

        free(ctx);
        return NULL;
}

int jwt_generate(JwtContext *ctx, const char *subject, int validity_seconds, char **ret_token) {
        char header_json[128];
        char payload_json[512];
        char header_b64[256], payload_b64[768];
        char signing_input[1024];
        unsigned char sig[EVP_MAX_MD_SIZE];
        unsigned int sig_len;
        char sig_b64[256];
        char *token;
        time_t now;
        int r;

        if (!ctx || !subject || !ret_token)
                return -EINVAL;

        now = time(NULL);

        snprintf(header_json, sizeof(header_json),
                 "{\"alg\":\"HS256\",\"typ\":\"JWT\"}");

        snprintf(payload_json, sizeof(payload_json),
                 "{\"sub\":\"%s\",\"iat\":%ld,\"exp\":%ld}",
                 subject, (long) now, (long)(now + validity_seconds));

        r = base64url_encode((const uint8_t*) header_json, strlen(header_json),
                             header_b64, sizeof(header_b64));
        if (r < 0)
                return r;

        r = base64url_encode((const uint8_t*) payload_json, strlen(payload_json),
                             payload_b64, sizeof(payload_b64));
        if (r < 0)
                return r;

        snprintf(signing_input, sizeof(signing_input), "%s.%s", header_b64, payload_b64);

        if (!HMAC(EVP_sha256(), ctx->secret, (int) ctx->secret_len,
                  (const unsigned char*) signing_input, strlen(signing_input),
                  sig, &sig_len))
                return log_error_errno(EIO, "Failed to compute HMAC signature.");

        r = base64url_encode(sig, sig_len, sig_b64, sizeof(sig_b64));
        if (r < 0)
                return r;

        r = asprintf(&token, "%s.%s", signing_input, sig_b64);
        if (r < 0)
                return log_oom(), -ENOMEM;

        *ret_token = token;
        return 0;
}

int jwt_validate(JwtContext *ctx, const char *token, JwtClaims *ret_claims) {
        char *header_end, *payload_end;
        char signing_input[1024];
        unsigned char expected_sig[EVP_MAX_MD_SIZE];
        unsigned int expected_sig_len;
        uint8_t actual_sig[256];
        char payload_json[512];
        int sig_len;
        size_t payload_b64_len;
        int r;

        if (!ctx || !token || !ret_claims)
                return -EINVAL;

        /* Split token into header.payload.signature */
        header_end = strchr(token, '.');
        if (!header_end)
                return -EINVAL;

        payload_end = strchr(header_end + 1, '.');
        if (!payload_end)
                return -EINVAL;

        /* Verify signature */
        snprintf(signing_input, sizeof(signing_input), "%.*s",
                 (int)(payload_end - token), token);

        if (!HMAC(EVP_sha256(), ctx->secret, (int) ctx->secret_len,
                  (const unsigned char*) signing_input, strlen(signing_input),
                  expected_sig, &expected_sig_len))
                return -EIO;

        sig_len = base64url_decode(payload_end + 1, strlen(payload_end + 1),
                                   actual_sig, sizeof(actual_sig));
        if (sig_len < 0)
                return sig_len;

        if ((unsigned int) sig_len != expected_sig_len ||
            CRYPTO_memcmp(expected_sig, actual_sig, expected_sig_len) != 0)
                return -EACCES;

        /* Decode payload */
        payload_b64_len = (size_t)(payload_end - header_end - 1);
        r = base64url_decode(header_end + 1, payload_b64_len,
                             (uint8_t*) payload_json, sizeof(payload_json) - 1);
        if (r < 0)
                return r;
        payload_json[r] = '\0';

        /* Minimal JSON parsing for sub, iat, exp */
        memset(ret_claims, 0, sizeof(*ret_claims));

        {
                char *sub_start = strstr(payload_json, "\"sub\":\"");
                if (sub_start) {
                        sub_start += 7;
                        char *sub_end = strchr(sub_start, '"');
                        if (sub_end) {
                                ret_claims->subject = strndup(sub_start, (size_t)(sub_end - sub_start));
                        }
                }
        }

        {
                char *iat_start = strstr(payload_json, "\"iat\":");
                if (iat_start)
                        ret_claims->issued_at = (time_t) atol(iat_start + 6);
        }

        {
                char *exp_start = strstr(payload_json, "\"exp\":");
                if (exp_start)
                        ret_claims->expires_at = (time_t) atol(exp_start + 6);
        }

        /* Check expiration */
        if (ret_claims->expires_at > 0 && ret_claims->expires_at < time(NULL)) {
                jwt_claims_free(ret_claims);
                return -ETIMEDOUT;
        }

        return 0;
}

void jwt_claims_free(JwtClaims *claims) {
        if (!claims)
                return;

        free(claims->subject);
        claims->subject = NULL;
}
