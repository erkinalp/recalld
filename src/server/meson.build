# SPDX-License-Identifier: LGPL-2.1-or-later

server_deps = [
        dep_common,
        dep_threads,
        dep_openssl,
        dep_systemd,
]

if dep_pam.found()
        server_deps += dep_pam
endif

server_sources = files(
        'pam-auth.c',
        'jwt.c',
        'query-service.c',
        'http-server.c',
        'stream-receiver.c',
)

lib_server = static_library(
        'recalld-server',
        server_sources,
        include_directories : [inc_root, inc_include],
        dependencies : server_deps,
)

dep_server = declare_dependency(
        link_with : lib_server,
        include_directories : [inc_root, inc_include],
)

# systemd-recall-query-service
executable(
        'systemd-recall-query-service',
        'main.c',
        include_directories : [inc_root, inc_include],
        dependencies : [dep_server, dep_common] + server_deps,
        install : true,
        install_dir : get_option('libdir') / 'systemd',
)
