/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <stdbool.h>
#include <time.h>

typedef struct JwtContext JwtContext;

typedef struct JwtClaims {
        char *subject;
        time_t issued_at;
        time_t expires_at;
} JwtClaims;

int jwt_new(JwtContext **ret, const char *secret_file);
JwtContext* jwt_free(JwtContext *ctx);

/* Generate a signed JWT token. Caller must free the returned string. */
int jwt_generate(JwtContext *ctx, const char *subject, int validity_seconds, char **ret_token);

/* Validate and decode a JWT token. Returns 0 on success. */
int jwt_validate(JwtContext *ctx, const char *token, JwtClaims *ret_claims);

void jwt_claims_free(JwtClaims *claims);
