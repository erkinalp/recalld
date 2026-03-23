/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <systemd/sd-bus.h>

#include "client/dbus-service.h"

static sd_bus *bus = NULL;

static int bus_connect(void) {
        int r;

        r = sd_bus_open_system(&bus);
        if (r < 0) {
                fprintf(stderr, "Failed to connect to system bus: %s\n", strerror(-r));
                return r;
        }

        return 0;
}

static int call_method_simple(const char *method) {
        sd_bus_error error = SD_BUS_ERROR_NULL;
        sd_bus_message *reply = NULL;
        int r;

        r = sd_bus_call_method(bus,
                        RECALLD_DBUS_NAME,
                        RECALLD_DBUS_PATH,
                        RECALLD_DBUS_INTERFACE,
                        method,
                        &error, &reply, "");
        if (r < 0) {
                fprintf(stderr, "Failed to call %s: %s\n", method, error.message);
                sd_bus_error_free(&error);
                return r;
        }

        sd_bus_message_unref(reply);
        sd_bus_error_free(&error);
        return 0;
}

static int cmd_status(int argc, char *argv[]) {
        sd_bus_error error = SD_BUS_ERROR_NULL;
        sd_bus_message *reply = NULL;
        const char *status;
        int64_t storage_used;
        int capture_count;
        int r;

        r = sd_bus_call_method(bus,
                        RECALLD_DBUS_NAME,
                        RECALLD_DBUS_PATH,
                        RECALLD_DBUS_INTERFACE,
                        "GetStatus",
                        &error, &reply, "");
        if (r < 0) {
                fprintf(stderr, "Failed to get status: %s\n", error.message);
                sd_bus_error_free(&error);
                return r;
        }

        r = sd_bus_message_read(reply, "sxi", &status, &storage_used, &capture_count);
        if (r < 0) {
                fprintf(stderr, "Failed to parse status response.\n");
                sd_bus_message_unref(reply);
                return r;
        }

        printf("Status:        %s\n", status);
        printf("Captures:      %d\n", capture_count);

        if (storage_used < 1024)
                printf("Storage used:  %ld B\n", (long) storage_used);
        else if (storage_used < 1024 * 1024)
                printf("Storage used:  %.1f KiB\n", (double) storage_used / 1024);
        else if (storage_used < 1024 * 1024 * 1024)
                printf("Storage used:  %.1f MiB\n", (double) storage_used / (1024 * 1024));
        else
                printf("Storage used:  %.1f GiB\n", (double) storage_used / (1024 * 1024 * 1024));

        sd_bus_message_unref(reply);
        return 0;
}

static int cmd_start(int argc, char *argv[]) {
        int r;

        r = call_method_simple("StartCapture");
        if (r < 0)
                return r;

        printf("Capture started.\n");
        return 0;
}

static int cmd_stop(int argc, char *argv[]) {
        int r;

        r = call_method_simple("StopCapture");
        if (r < 0)
                return r;

        printf("Capture stopped.\n");
        return 0;
}

static int cmd_pause(int argc, char *argv[]) {
        int r;

        r = call_method_simple("Pause");
        if (r < 0)
                return r;

        printf("Capture paused.\n");
        return 0;
}

static int cmd_resume(int argc, char *argv[]) {
        int r;

        r = call_method_simple("Resume");
        if (r < 0)
                return r;

        printf("Capture resumed.\n");
        return 0;
}

static int cmd_list(int argc, char *argv[]) {
        sd_bus_error error = SD_BUS_ERROR_NULL;
        sd_bus_message *reply = NULL;
        const char *type_str = "audio";
        int64_t from = 0, to = 0;
        int r;

        /* Parse optional arguments */
        if (argc > 0)
                type_str = argv[0];
        if (argc > 1)
                from = (int64_t) atol(argv[1]);
        if (argc > 2)
                to = (int64_t) atol(argv[2]);

        if (to == 0)
                to = (int64_t) time(NULL);

        r = sd_bus_call_method(bus,
                        RECALLD_DBUS_NAME,
                        RECALLD_DBUS_PATH,
                        RECALLD_DBUS_INTERFACE,
                        "ListCaptures",
                        &error, &reply, "sxx", type_str, from, to);
        if (r < 0) {
                fprintf(stderr, "Failed to list captures: %s\n", error.message);
                sd_bus_error_free(&error);
                return r;
        }

        r = sd_bus_message_enter_container(reply, 'a', "(xsxix)");
        if (r < 0)
                goto finish;

        printf("%-8s %-12s %-24s %-12s %-12s\n",
               "ID", "Type", "Timestamp", "Duration", "Size");
        printf("%-8s %-12s %-24s %-12s %-12s\n",
               "--------", "------------", "------------------------",
               "------------", "------------");

        while ((r = sd_bus_message_read(reply, "(xsxix)",
                        NULL, NULL, NULL, NULL, NULL)) > 0) {

                int64_t id, timestamp, size;
                const char *type;
                int duration;
                char timebuf[64];
                struct tm tm;
                time_t t;

                /* Re-read with actual variables */
                sd_bus_message_rewind(reply, 0);
                r = sd_bus_message_read(reply, "(xsxix)",
                                &id, &type, &timestamp, &duration, &size);
                if (r < 0)
                        break;

                t = (time_t) timestamp;
                localtime_r(&t, &tm);
                strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", &tm);

                printf("%-8ld %-12s %-24s %-12d %-12ld\n",
                       (long) id, type, timebuf, duration, (long) size);
        }

finish:
        sd_bus_message_unref(reply);
        sd_bus_error_free(&error);
        return 0;
}

static void usage(void) {
        printf("Usage: recallctl COMMAND [OPTIONS]\n"
               "\n"
               "Control systemd-recalld capture daemon.\n"
               "\n"
               "Commands:\n"
               "  status             Show capture status\n"
               "  start              Start capturing\n"
               "  stop               Stop capturing\n"
               "  pause              Pause capturing\n"
               "  resume             Resume capturing\n"
               "  list [TYPE]        List captures (audio, video, screenshot)\n"
               "\n"
               "Options:\n"
               "  -h, --help         Show this help\n"
               "      --version      Show version\n");
}

int main(int argc, char *argv[]) {
        static const struct option long_options[] = {
                { "help",    no_argument, NULL, 'h' },
                { "version", no_argument, NULL, 'V' },
                {}
        };

        int c, r;

        while ((c = getopt_long(argc, argv, "h", long_options, /* longindex= */ NULL)) >= 0) {
                switch (c) {
                case 'h':
                        usage();
                        return EXIT_SUCCESS;
                case 'V':
                        printf("recallctl %s\n", "0.1.0");
                        return EXIT_SUCCESS;
                default:
                        return EXIT_FAILURE;
                }
        }

        if (optind >= argc) {
                usage();
                return EXIT_FAILURE;
        }

        r = bus_connect();
        if (r < 0)
                return EXIT_FAILURE;

        const char *cmd = argv[optind];
        argc -= optind + 1;
        argv += optind + 1;

        if (strcmp(cmd, "status") == 0)
                r = cmd_status(argc, argv);
        else if (strcmp(cmd, "start") == 0)
                r = cmd_start(argc, argv);
        else if (strcmp(cmd, "stop") == 0)
                r = cmd_stop(argc, argv);
        else if (strcmp(cmd, "pause") == 0)
                r = cmd_pause(argc, argv);
        else if (strcmp(cmd, "resume") == 0)
                r = cmd_resume(argc, argv);
        else if (strcmp(cmd, "list") == 0)
                r = cmd_list(argc, argv);
        else {
                fprintf(stderr, "Unknown command: %s\n", cmd);
                usage();
                r = -EINVAL;
        }

        sd_bus_flush_close_unref(bus);
        return r < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
