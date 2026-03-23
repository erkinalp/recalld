# SPDX-License-Identifier: LGPL-2.1-or-later

common_sources = files(
        'recalld-log.c',
        'recalld-config.c',
        'recalld-encryption.c',
        'recalld-storage.c',
)

lib_common = static_library(
        'recalld-common',
        common_sources,
        include_directories : [inc_root, inc_include],
        dependencies : [
                dep_threads,
                dep_sqlite3,
                dep_openssl,
        ],
)

dep_common = declare_dependency(
        link_with : lib_common,
        include_directories : [inc_root, inc_include],
)
