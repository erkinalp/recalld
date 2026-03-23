/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <syslog.h>

typedef enum RecalldLogLevel {
        RECALLD_LOG_EMERG   = LOG_EMERG,
        RECALLD_LOG_ALERT   = LOG_ALERT,
        RECALLD_LOG_CRIT    = LOG_CRIT,
        RECALLD_LOG_ERR     = LOG_ERR,
        RECALLD_LOG_WARNING = LOG_WARNING,
        RECALLD_LOG_NOTICE  = LOG_NOTICE,
        RECALLD_LOG_INFO    = LOG_INFO,
        RECALLD_LOG_DEBUG   = LOG_DEBUG,
        _RECALLD_LOG_LEVEL_MAX,
        _RECALLD_LOG_LEVEL_INVALID = -1,
} RecalldLogLevel;

void recalld_log_set_level(RecalldLogLevel level);
RecalldLogLevel recalld_log_get_level(void);
RecalldLogLevel recalld_log_level_from_string(const char *s);
const char* recalld_log_level_to_string(RecalldLogLevel level);

int recalld_log_internal(
                RecalldLogLevel level,
                int error,
                const char *file,
                int line,
                const char *func,
                const char *format, ...) __attribute__((format(printf, 6, 7)));

#define log_debug(...)   recalld_log_internal(RECALLD_LOG_DEBUG,   0, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define log_info(...)    recalld_log_internal(RECALLD_LOG_INFO,    0, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define log_notice(...)  recalld_log_internal(RECALLD_LOG_NOTICE,  0, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define log_warning(...) recalld_log_internal(RECALLD_LOG_WARNING, 0, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define log_error(...)   recalld_log_internal(RECALLD_LOG_ERR,     0, __FILE__, __LINE__, __func__, __VA_ARGS__)

#define log_debug_errno(error, ...)   recalld_log_internal(RECALLD_LOG_DEBUG,   error, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define log_info_errno(error, ...)    recalld_log_internal(RECALLD_LOG_INFO,    error, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define log_warning_errno(error, ...) recalld_log_internal(RECALLD_LOG_WARNING, error, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define log_error_errno(error, ...)   recalld_log_internal(RECALLD_LOG_ERR,     error, __FILE__, __LINE__, __func__, __VA_ARGS__)

#define log_oom() log_error("Out of memory.")
