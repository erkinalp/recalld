/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <systemd/sd-daemon.h>

#include "server/http-server.h"
#include "server/jwt.h"
#include "server/query-service.h"
#include "common/recalld-config.h"
#include "common/recalld-log.h"

static volatile sig_atomic_t should_exit = 0;

static void signal_handler(int sig) {
        should_exit = 1;
}

static void usage(void) {
        printf("Usage: systemd-recall-query-service [OPTIONS]\n"
               "\n"
               "AI-powered query service for systemd-recalld captured data.\n"
               "\n"
               "  -c, --config=PATH    Configuration file path\n"
               "  -h, --help           Show this help\n"
               "      --version        Show version\n");
}

int main(int argc, char *argv[]) {
        static const struct option long_options[] = {
                { "config",  required_argument, NULL, 'c' },
                { "help",    no_argument,       NULL, 'h' },
                { "version", no_argument,       NULL, 'V' },
                {}
        };

        QueryServiceConfig config = {};
        QueryService *query_svc = NULL;
        JwtContext *jwt = NULL;
        HttpServer *http = NULL;
        const char *config_path = NULL;
        int c, r;

        while ((c = getopt_long(argc, argv, "c:h", long_options, /* longindex= */ NULL)) >= 0) {
                switch (c) {
                case 'c':
                        config_path = optarg;
                        break;
                case 'h':
                        usage();
                        return EXIT_SUCCESS;
                case 'V':
                        printf("systemd-recall-query-service %s\n", "0.1.0");
                        return EXIT_SUCCESS;
                default:
                        return EXIT_FAILURE;
                }
        }

        log_info("systemd-recall-query-service starting...");

        signal(SIGTERM, signal_handler);
        signal(SIGINT, signal_handler);

        r = query_service_config_load(&config,
                        config_path ? config_path :
                        "/etc/systemd/recall-query-service/recall-query-service.conf");
        if (r < 0) {
                log_error_errno(-r, "Failed to load configuration: %m");
                return EXIT_FAILURE;
        }

        if (config.log_level) {
                RecalldLogLevel lvl = recalld_log_level_from_string(config.log_level);
                if (lvl != _RECALLD_LOG_LEVEL_INVALID)
                        recalld_log_set_level(lvl);
        }

        r = jwt_new(&jwt, config.jwt_secret_file);
        if (r < 0) {
                log_error_errno(-r, "Failed to initialize JWT: %m");
                goto finish;
        }

        r = query_service_new(&query_svc, &config);
        if (r < 0) {
                log_error_errno(-r, "Failed to initialize query service: %m");
                goto finish;
        }

        r = query_service_start(query_svc);
        if (r < 0) {
                log_error_errno(-r, "Failed to start query service: %m");
                goto finish;
        }

        r = http_server_new(&http, &config, query_svc, jwt);
        if (r < 0) {
                log_error_errno(-r, "Failed to initialize HTTP server: %m");
                goto finish;
        }

        r = http_server_start(http);
        if (r < 0) {
                log_error_errno(-r, "Failed to start HTTP server: %m");
                goto finish;
        }

        sd_notify(/* unset= */ 0, "READY=1\nSTATUS=Running");
        log_info("Service ready, listening on port %d.", config.port);

        while (!should_exit) {
                sd_notify(/* unset= */ 0, "WATCHDOG=1");
                sleep(5);
        }

        sd_notify(/* unset= */ 0, "STOPPING=1");

finish:
        http_server_free(http);
        query_service_free(query_svc);
        jwt_free(jwt);
        query_service_config_free(&config);

        log_info("systemd-recall-query-service stopped.");
        return r < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
