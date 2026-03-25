# SPDX-License-Identifier: LGPL-2.1-or-later

client_deps = [
        dep_common,
        dep_threads,
        dep_systemd,
        dep_dbus,
        dep_openssl,
]

if dep_alsa.found()
        client_deps += dep_alsa
endif

if dep_pulse.found()
        client_deps += dep_pulse
endif

if dep_x11.found()
        client_deps += dep_x11
endif

if dep_xext.found()
        client_deps += dep_xext
endif

client_sources = files(
        'audio-capture.c',
        'video-capture.c',
        'network-transmit.c',
        'dbus-service.c',
        'recalld-daemon.c',
)

lib_client = static_library(
        'recalld-client',
        client_sources,
        include_directories : [inc_root, inc_include],
        dependencies : client_deps,
)

dep_client = declare_dependency(
        link_with : lib_client,
        include_directories : [inc_root, inc_include],
)

# systemd-recalld daemon
executable(
        'systemd-recalld',
        'main.c',
        include_directories : [inc_root, inc_include],
        dependencies : [dep_client, dep_common] + client_deps,
        install : true,
        install_dir : get_option('libdir') / 'systemd',
)

# recallctl command-line tool
executable(
        'recallctl',
        'recallctl.c',
        include_directories : [inc_root, inc_include],
        dependencies : [dep_systemd, dep_dbus],
        install : true,
        install_dir : get_option('bindir'),
)
