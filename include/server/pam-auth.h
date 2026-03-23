/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <stdbool.h>

typedef struct PamAuthContext PamAuthContext;

int pam_auth_new(PamAuthContext **ret, const char *service_name);
PamAuthContext* pam_auth_free(PamAuthContext *ctx);

bool pam_auth_authenticate(PamAuthContext *ctx, const char *username, const char *password);
bool pam_auth_check_account(PamAuthContext *ctx, const char *username);
