deps = [
  dependency('gtk4', version: '>= 4.14'),
  dependency('libadwaita-1', version: '>= 1.5'),
  dependency('freerdp3', version: '>= 3.5'),
  dependency('freerdp-client3', version: '>= 3.5'),
  dependency('winpr3', version: '>= 3.5'),
  dependency('libvncclient', version: '>= 0.9.14'),
  dependency('libsecret-1'),
]

executable('direct-rdp',
  'main.c',
  'dr-connection.c',
  'dr-display.c',
  'dr-rdp-session.c',
  'dr-session.c',
  'dr-vnc-session.c',
  'dr-window.c',
  'dr-wol.c',
  dependencies: deps,
  include_directories: include_directories('..'),
  install: true,
)
