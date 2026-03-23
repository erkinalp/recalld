/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

#include "client/recalld-daemon.h"
#include "common/recalld-log.h"

static RecalldDaemon *daemon_instance = NULL;

static void signal_handler(int sig) {
        if (daemon_instance)
                daemon_instance->should_exit = true;
}

static void usage(void) {
        printf("Usage: systemd-recalld [OPTIONS]\n"
               "\n"
               "Desktop audio and video capture daemon.\n"
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
                        printf("systemd-recalld %s\n", "0.1.0");
                        return EXIT_SUCCESS;
                default:
                        return EXIT_FAILURE;
                }
        }

        log_info("systemd-recalld starting...");

        /* Set up signal handlers */
        signal(SIGTERM, signal_handler);
        signal(SIGINT, signal_handler);

        r = daemon_new(&daemon_instance, config_path);
        if (r < 0) {
                log_error_errno(-r, "Failed to initialize daemon: %m");
                return EXIT_FAILURE;
        }

        r = daemon_run(daemon_instance);

        daemon_free(daemon_instance);
        daemon_instance = NULL;

        log_info("systemd-recalld stopped.");
        return r < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
