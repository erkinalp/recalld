/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "common/recalld-log.h"

static RecalldLogLevel current_log_level = RECALLD_LOG_INFO;

void recalld_log_set_level(RecalldLogLevel level) {
        if (level >= 0 && level < _RECALLD_LOG_LEVEL_MAX)
                current_log_level = level;
}

RecalldLogLevel recalld_log_get_level(void) {
        return current_log_level;
}

RecalldLogLevel recalld_log_level_from_string(const char *s) {
        if (!s)
                return _RECALLD_LOG_LEVEL_INVALID;

        if (strcmp(s, "emerg") == 0)
                return RECALLD_LOG_EMERG;
        if (strcmp(s, "alert") == 0)
                return RECALLD_LOG_ALERT;
        if (strcmp(s, "crit") == 0)
                return RECALLD_LOG_CRIT;
        if (strcmp(s, "err") == 0 || strcmp(s, "error") == 0)
                return RECALLD_LOG_ERR;
        if (strcmp(s, "warning") == 0 || strcmp(s, "warn") == 0)
                return RECALLD_LOG_WARNING;
        if (strcmp(s, "notice") == 0)
                return RECALLD_LOG_NOTICE;
        if (strcmp(s, "info") == 0)
                return RECALLD_LOG_INFO;
        if (strcmp(s, "debug") == 0)
                return RECALLD_LOG_DEBUG;

        return _RECALLD_LOG_LEVEL_INVALID;
}

const char* recalld_log_level_to_string(RecalldLogLevel level) {
        switch (level) {
        case RECALLD_LOG_EMERG:
                return "emerg";
        case RECALLD_LOG_ALERT:
                return "alert";
        case RECALLD_LOG_CRIT:
                return "crit";
        case RECALLD_LOG_ERR:
                return "error";
        case RECALLD_LOG_WARNING:
                return "warning";
        case RECALLD_LOG_NOTICE:
                return "notice";
        case RECALLD_LOG_INFO:
                return "info";
        case RECALLD_LOG_DEBUG:
                return "debug";
        default:
                return "unknown";
        }
}

int recalld_log_internal(
                RecalldLogLevel level,
                int error,
                const char *file,
                int line,
                const char *func,
                const char *format, ...) {

        va_list ap;
        char buf[4096];
        time_t now;
        struct tm tm;
        char timebuf[64];

        if (level > current_log_level)
                return -error;

        now = time(NULL);
        localtime_r(&now, &tm);
        strftime(timebuf, sizeof(timebuf), "%b %d %H:%M:%S", &tm);

        va_start(ap, format);
        vsnprintf(buf, sizeof(buf), format, ap);
        va_end(ap);

        fprintf(stderr, "%s systemd-recalld[%d]: %s\n",
                timebuf, (int) getpid(), buf);

        return -error;
}
