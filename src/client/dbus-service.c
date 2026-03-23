/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <systemd/sd-bus.h>

#include "client/dbus-service.h"
#include "client/recalld-daemon.h"
#include "common/recalld-log.h"
#include "common/recalld-storage.h"

int dbus_method_start_capture(sd_bus_message *m, void *userdata, sd_bus_error *error) {
        RecalldDaemon *daemon = userdata;
        int r;

        r = daemon_start_capture(daemon);
        if (r < 0)
                return sd_bus_error_set_errnof(error, -r,
                        "Failed to start capture: %s", strerror(-r));

        return sd_bus_reply_method_return(m, "");
}

int dbus_method_stop_capture(sd_bus_message *m, void *userdata, sd_bus_error *error) {
        RecalldDaemon *daemon = userdata;
        int r;

        r = daemon_stop_capture(daemon);
        if (r < 0)
                return sd_bus_error_set_errnof(error, -r,
                        "Failed to stop capture: %s", strerror(-r));

        return sd_bus_reply_method_return(m, "");
}

int dbus_method_pause(sd_bus_message *m, void *userdata, sd_bus_error *error) {
        RecalldDaemon *daemon = userdata;
        int r;

        r = daemon_pause(daemon);
        if (r < 0)
                return sd_bus_error_set_errnof(error, -r,
                        "Failed to pause capture: %s", strerror(-r));

        return sd_bus_reply_method_return(m, "");
}

int dbus_method_resume(sd_bus_message *m, void *userdata, sd_bus_error *error) {
        RecalldDaemon *daemon = userdata;
        int r;

        r = daemon_resume(daemon);
        if (r < 0)
                return sd_bus_error_set_errnof(error, -r,
                        "Failed to resume capture: %s", strerror(-r));

        return sd_bus_reply_method_return(m, "");
}

int dbus_method_get_status(sd_bus_message *m, void *userdata, sd_bus_error *error) {
        RecalldDaemon *daemon = userdata;
        const char *status;
        int64_t storage_used;
        int capture_count;

        if (!daemon->capturing)
                status = "stopped";
        else if (daemon->paused)
                status = "paused";
        else
                status = "capturing";

        storage_used = storage_get_total_size(daemon->storage);
        capture_count = storage_get_capture_count(daemon->storage);

        return sd_bus_reply_method_return(m, "sxi",
                        status,
                        storage_used > 0 ? storage_used : (int64_t) 0,
                        capture_count > 0 ? capture_count : 0);
}

int dbus_method_list_captures(sd_bus_message *m, void *userdata, sd_bus_error *error) {
        RecalldDaemon *daemon = userdata;
        sd_bus_message *reply = NULL;
        const char *type_str;
        CaptureType type;
        int64_t from, to;
        CaptureMetadata *entries = NULL;
        int count = 0;
        int r;

        r = sd_bus_message_read(m, "sxx", &type_str, &from, &to);
        if (r < 0)
                return sd_bus_error_set_errnof(error, -r, "Failed to parse arguments");

        type = capture_type_from_string(type_str);
        if (type == _CAPTURE_TYPE_INVALID)
                return sd_bus_error_set_errnof(error, EINVAL, "Invalid capture type: %s", type_str);

        r = storage_list(daemon->storage, type, (time_t) from, (time_t) to, &entries, &count);
        if (r < 0)
                return sd_bus_error_set_errnof(error, -r, "Failed to list captures");

        r = sd_bus_message_new_method_return(m, &reply);
        if (r < 0)
                goto finish;

        r = sd_bus_message_open_container(reply, 'a', "(xsxix)");
        if (r < 0)
                goto finish;

        for (int i = 0; i < count; i++) {
                r = sd_bus_message_append(reply, "(xsxix)",
                                entries[i].id,
                                capture_type_to_string(entries[i].type),
                                (int64_t) entries[i].timestamp,
                                entries[i].duration_ms,
                                (int64_t) entries[i].data_size);
                if (r < 0)
                        goto finish;
        }

        r = sd_bus_message_close_container(reply);
        if (r < 0)
                goto finish;

        r = sd_bus_send(/* bus= */ NULL, reply, /* cookie= */ NULL);

finish:
        storage_metadata_free(entries, count);
        sd_bus_message_unref(reply);
        return r < 0 ? r : 1; /* positive = reply already sent */
}

int dbus_property_get_capturing(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        RecalldDaemon *daemon = userdata;
        return sd_bus_message_append(reply, "b", daemon->capturing);
}

int dbus_property_get_paused(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        RecalldDaemon *daemon = userdata;
        return sd_bus_message_append(reply, "b", daemon->paused);
}

int dbus_property_get_capture_count(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        RecalldDaemon *daemon = userdata;
        int count = storage_get_capture_count(daemon->storage);
        return sd_bus_message_append(reply, "i", count > 0 ? count : 0);
}

int dbus_property_get_storage_used(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        RecalldDaemon *daemon = userdata;
        int64_t used = storage_get_total_size(daemon->storage);
        return sd_bus_message_append(reply, "x", used > 0 ? used : (int64_t) 0);
}

static const sd_bus_vtable recalld_vtable[] = {
        SD_BUS_VTABLE_START(0),
        SD_BUS_METHOD("StartCapture", "", "", dbus_method_start_capture, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("StopCapture", "", "", dbus_method_stop_capture, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("Pause", "", "", dbus_method_pause, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("Resume", "", "", dbus_method_resume, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("GetStatus", "", "sxi", dbus_method_get_status, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("ListCaptures", "sxx", "a(xsxix)", dbus_method_list_captures, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_PROPERTY("Capturing", "b", dbus_property_get_capturing, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
        SD_BUS_PROPERTY("Paused", "b", dbus_property_get_paused, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
        SD_BUS_PROPERTY("CaptureCount", "i", dbus_property_get_capture_count, 0, 0),
        SD_BUS_PROPERTY("StorageUsed", "x", dbus_property_get_storage_used, 0, 0),
        SD_BUS_VTABLE_END,
};

int dbus_service_init(sd_bus **ret_bus) {
        sd_bus *bus = NULL;
        int r;

        r = sd_bus_open_system(&bus);
        if (r < 0)
                return log_error_errno(-r, "Failed to connect to system bus: %m");

        *ret_bus = bus;
        return 0;
}

int dbus_service_register(sd_bus *bus, RecalldDaemon *daemon) {
        int r;

        r = sd_bus_add_object_vtable(bus, /* slot= */ NULL,
                        RECALLD_DBUS_PATH,
                        RECALLD_DBUS_INTERFACE,
                        recalld_vtable,
                        daemon);
        if (r < 0)
                return log_error_errno(-r, "Failed to register D-Bus object: %m");

        r = sd_bus_request_name(bus, RECALLD_DBUS_NAME, 0);
        if (r < 0)
                return log_error_errno(-r, "Failed to acquire bus name %s: %m",
                                       RECALLD_DBUS_NAME);

        log_info("D-Bus service registered: %s", RECALLD_DBUS_NAME);
        return 0;
}

void dbus_service_cleanup(sd_bus *bus) {
        sd_bus_release_name(bus, RECALLD_DBUS_NAME);
        sd_bus_flush_close_unref(bus);
}
