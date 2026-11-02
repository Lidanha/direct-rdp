project('direct-rdp', 'c',
  version: '0.6.3',
  license: 'GPL-3.0-or-later',
  meson_version: '>= 0.62',
  default_options: ['c_std=gnu11', 'warning_level=2', 'buildtype=debugoptimized'],
)

app_id = 'io.github.lidanha.DirectRDP'

conf = configuration_data()
conf.set_quoted('APP_ID', app_id)
conf.set_quoted('APP_VERSION', meson.project_version())
configure_file(output: 'config.h', configuration: conf)

add_project_arguments(
  '-Wno-unused-parameter',
  '-Wno-missing-field-initializers',
  '-DGDK_VERSION_MIN_REQUIRED=GDK_VERSION_4_14',
  '-DADW_VERSION_MIN_REQUIRED=ADW_VERSION_1_5',
  language: 'c',
)

subdir('src')
subdir('data')
subdir('tests')

gnome = import('gnome')
gnome.post_install(gtk_update_icon_cache: true, update_desktop_database: true)
