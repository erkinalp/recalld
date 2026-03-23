project('systemd-recalld', 'c',
  version : '0.1.0',
  license : 'LGPL-2.1-or-later',
  default_options : [
    'c_std=gnu11',
    'warning_level=2',
    'b_ndebug=if-release',
  ],
  meson_version : '>= 0.60.0',
)

cc = meson.get_compiler('c')

# Required dependencies
dep_threads = dependency('threads')
dep_sqlite3 = dependency('sqlite3', version : '>= 3.35.0')
dep_openssl = dependency('openssl', version : '>= 1.1.0')
dep_systemd = dependency('libsystemd', version : '>= 249')
dep_dbus = dependency('dbus-1')

# Optional dependencies
dep_alsa = dependency('alsa', required : false)
dep_pulse = dependency('libpulse', required : false)
dep_x11 = dependency('x11', required : false)
dep_xext = dependency('xext', required : false)
dep_opus = dependency('opus', required : false)
dep_vpx = dependency('vpx', required : false)
dep_pam = cc.find_library('pam', required : false)

conf = configuration_data()
conf.set_quoted('PACKAGE_VERSION', meson.project_version())
conf.set_quoted('SYSCONFDIR', get_option('prefix') / get_option('sysconfdir'))
conf.set_quoted('LOCALSTATEDIR', get_option('prefix') / get_option('localstatedir'))
conf.set_quoted('LIBDIR', get_option('prefix') / get_option('libdir'))
conf.set10('HAVE_ALSA', dep_alsa.found())
conf.set10('HAVE_PULSE', dep_pulse.found())
conf.set10('HAVE_X11', dep_x11.found())
conf.set10('HAVE_OPUS', dep_opus.found())
conf.set10('HAVE_VPX', dep_vpx.found())
conf.set10('HAVE_PAM', dep_pam.found())

configure_file(output : 'config.h', configuration : conf)

inc_root = include_directories('.')
inc_include = include_directories('include')

subdir('src/common')
subdir('src/client')
subdir('src/server')

# Install systemd unit files
install_data('units/systemd-recalld.service',
  install_dir : get_option('prefix') / 'lib' / 'systemd' / 'system',
)
install_data('units/systemd-recall-query-service.service',
  install_dir : get_option('prefix') / 'lib' / 'systemd' / 'system',
)

# Install configuration files
install_data('config/recalld.conf',
  install_dir : get_option('sysconfdir') / 'systemd' / 'recalld',
)
install_data('config/recall-query-service.conf',
  install_dir : get_option('sysconfdir') / 'systemd' / 'recall-query-service',
)

# Install man pages
install_man('man/recallctl.1')
install_man('man/systemd-recalld.service.8')
install_man('man/systemd-recall-query-service.service.8')

# Install D-Bus policy
install_data('config/org.freedesktop.systemd.recalld.conf',
  install_dir : get_option('prefix') / 'share' / 'dbus-1' / 'system.d',
)

# Install PAM config
if dep_pam.found()
  install_data('config/systemd-recall',
    install_dir : get_option('sysconfdir') / 'pam.d',
  )
endif
