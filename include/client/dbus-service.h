/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <systemd/sd-bus.h>

#define RECALLD_DBUS_NAME      "org.freedesktop.systemd.recalld"
#define RECALLD_DBUS_PATH      "/org/freedesktop/systemd/recalld"
#define RECALLD_DBUS_INTERFACE "org.freedesktop.systemd.recalld"

typedef struct RecalldDaemon RecalldDaemon;

int dbus_service_init(sd_bus **ret_bus);
int dbus_service_register(sd_bus *bus, RecalldDaemon *daemon);
void dbus_service_cleanup(sd_bus *bus);

/* D-Bus method handlers */
int dbus_method_start_capture(sd_bus_message *m, void *userdata, sd_bus_error *error);
int dbus_method_stop_capture(sd_bus_message *m, void *userdata, sd_bus_error *error);
int dbus_method_pause(sd_bus_message *m, void *userdata, sd_bus_error *error);
int dbus_method_resume(sd_bus_message *m, void *userdata, sd_bus_error *error);
int dbus_method_get_status(sd_bus_message *m, void *userdata, sd_bus_error *error);
int dbus_method_list_captures(sd_bus_message *m, void *userdata, sd_bus_error *error);

/* D-Bus property handlers */
int dbus_property_get_capturing(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error);

int dbus_property_get_paused(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error);

int dbus_property_get_capture_count(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error);

int dbus_property_get_storage_used(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error);
