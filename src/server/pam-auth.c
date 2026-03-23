/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "server/pam-auth.h"
#include "common/recalld-log.h"

#if HAVE_PAM
#include <security/pam_appl.h>

struct PamAuthContext {
        char *service_name;
};

static int pam_conversation(
                int num_msg,
                const struct pam_message **msg,
                struct pam_response **resp,
                void *appdata_ptr) {

        struct pam_response *reply;
        const char *password = appdata_ptr;

        if (num_msg <= 0 || num_msg > PAM_MAX_NUM_MSG)
                return PAM_CONV_ERR;

        reply = calloc((size_t) num_msg, sizeof(struct pam_response));
        if (!reply)
                return PAM_BUF_ERR;

        for (int i = 0; i < num_msg; i++) {
                switch (msg[i]->msg_style) {
                case PAM_PROMPT_ECHO_OFF:
                case PAM_PROMPT_ECHO_ON:
                        reply[i].resp = strdup(password ? password : "");
                        if (!reply[i].resp) {
                                free(reply);
                                return PAM_BUF_ERR;
                        }
                        break;
                case PAM_ERROR_MSG:
                        log_error("PAM error: %s", msg[i]->msg);
                        break;
                case PAM_TEXT_INFO:
                        log_info("PAM info: %s", msg[i]->msg);
                        break;
                default:
                        free(reply);
                        return PAM_CONV_ERR;
                }
        }

        *resp = reply;
        return PAM_SUCCESS;
}

int pam_auth_new(PamAuthContext **ret, const char *service_name) {
        PamAuthContext *ctx;

        if (!ret || !service_name)
                return -EINVAL;

        ctx = calloc(1, sizeof(PamAuthContext));
        if (!ctx)
                return log_oom(), -ENOMEM;

        ctx->service_name = strdup(service_name);
        if (!ctx->service_name) {
                free(ctx);
                return log_oom(), -ENOMEM;
        }

        log_info("PAM authentication initialized with service: %s", service_name);
        *ret = ctx;
        return 0;
}

PamAuthContext* pam_auth_free(PamAuthContext *ctx) {
        if (!ctx)
                return NULL;

        free(ctx->service_name);
        free(ctx);
        return NULL;
}

bool pam_auth_authenticate(PamAuthContext *ctx, const char *username, const char *password) {
        pam_handle_t *pamh = NULL;
        struct pam_conv conv = {
                .conv = pam_conversation,
                .appdata_ptr = (void*) password,
        };
        int r;

        if (!ctx || !username || !password)
                return false;

        r = pam_start(ctx->service_name, username, &conv, &pamh);
        if (r != PAM_SUCCESS) {
                log_error("PAM start failed: %s", pam_strerror(pamh, r));
                return false;
        }

        r = pam_authenticate(pamh, 0);
        if (r != PAM_SUCCESS) {
                log_warning("PAM authentication failed for user %s: %s",
                            username, pam_strerror(pamh, r));
                pam_end(pamh, r);
                return false;
        }

        r = pam_acct_mgmt(pamh, 0);
        if (r != PAM_SUCCESS) {
                log_warning("PAM account check failed for user %s: %s",
                            username, pam_strerror(pamh, r));
                pam_end(pamh, r);
                return false;
        }

        pam_end(pamh, PAM_SUCCESS);
        log_info("User %s authenticated successfully.", username);
        return true;
}

bool pam_auth_check_account(PamAuthContext *ctx, const char *username) {
        pam_handle_t *pamh = NULL;
        struct pam_conv conv = {
                .conv = pam_conversation,
                .appdata_ptr = NULL,
        };
        int r;

        if (!ctx || !username)
                return false;

        r = pam_start(ctx->service_name, username, &conv, &pamh);
        if (r != PAM_SUCCESS)
                return false;

        r = pam_acct_mgmt(pamh, 0);
        pam_end(pamh, r);

        return r == PAM_SUCCESS;
}

#else /* !HAVE_PAM */

struct PamAuthContext {
        char *service_name;
};

int pam_auth_new(PamAuthContext **ret, const char *service_name) {
        log_warning("PAM support not compiled in.");
        return -ENOSYS;
}

PamAuthContext* pam_auth_free(PamAuthContext *ctx) {
        return NULL;
}

bool pam_auth_authenticate(PamAuthContext *ctx, const char *username, const char *password) {
        return false;
}

bool pam_auth_check_account(PamAuthContext *ctx, const char *username) {
        return false;
}

#endif /* HAVE_PAM */
