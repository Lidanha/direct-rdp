deps = [
  dependency('gtk4', version: '>= 4.14'),
  dependency('libadwaita-1', version: '>= 1.5'),
  dependency('freerdp3', version: '>= 3.5'),
  dependency('freerdp-client3', version: '>= 3.5'),
  dependency('winpr3', version: '>= 3.5'),
  dependency('libvncclient', version: '>= 0.9.14'),
  dependency('libsecret-1'),
  dependency('zlib'),
]

# UTF-8 clipboard arrived in libvncclient 0.9.15; older ones only do Latin-1.
c_args = []
if meson.get_compiler('c').has_member('rfbClient', 'GotXCutTextUTF8',
    prefix: '#include <rfb/rfbclient.h>', dependencies: deps[5])
  c_args += '-DHAVE_VNC_UTF8_CLIPBOARD'
endif

executable('direct-rdp',
  'main.c',
  'dr-apple-adaptive.c',
  'dr-connection.c',
  'dr-display.c',
  'dr-rdp-session.c',
  'dr-session.c',
  'dr-vnc-session.c',
  'dr-window.c',
  'dr-wol.c',
  dependencies: deps,
  c_args: c_args,
  include_directories: include_directories('..'),
  install: true,
)
