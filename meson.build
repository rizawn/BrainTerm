project(
  'brainterm',
  'c',
  version: '0.1.0',
  meson_version: '>= 1.1.0',
  default_options: [
    'c_std=c17',
    'warning_level=3',
    'werror=false',
  ],
)

cc = meson.get_compiler('c')
is_windows = host_machine.system() == 'windows'

add_project_arguments(
  cc.get_supported_arguments([
    '-Wpedantic',
    '-Wshadow',
    '-Wformat=2',
    '-Wno-overlength-strings',
  ]),
  language: 'c',
)

gnome = import('gnome')

glib_dep = dependency('glib-2.0', version: '>= 2.74', include_type: 'system')
gio_dep = dependency('gio-2.0', version: '>= 2.74', include_type: 'system')
gtk4_dep = dependency('gtk4', version: '>= 4.10', include_type: 'system')
adw_dep = dependency('libadwaita-1', version: '>= 1.4', include_type: 'system')
gtksource_dep = dependency('gtksourceview-5', version: '>= 5.4', include_type: 'system')
vterm_dep = dependency('vterm', version: '>= 0.3', include_type: 'system')
git2_dep = dependency('libgit2', include_type: 'system')
cjson_dep = dependency('libcjson', include_type: 'system')
yaml_dep = dependency('yaml-0.1', include_type: 'system')
sqlite_dep = dependency('sqlite3', include_type: 'system')
md4c_dep = dependency('md4c', include_type: 'system')
m_dep = cc.find_library('m', required: false)
util_dep = is_windows ? [] : [cc.find_library('util', required: false)]
cmocka_dep = dependency('cmocka', required: get_option('tests'), include_type: 'system')

langspec_install_dir = get_option('datadir') / 'brainterm' / 'language-specs'
styles_install_dir = get_option('datadir') / 'brainterm' / 'styles'
fonts_install_dir = get_option('datadir') / 'brainterm' / 'fonts'
install_data('data/language-specs/bt-markdown.lang', install_dir: langspec_install_dir)
install_data('data/styles/bt-ink-dark.xml', 'data/styles/bt-ink-light.xml', install_dir: styles_install_dir)
install_data(
  'data/fonts/InterVariable.ttf',
  'data/fonts/JetBrainsMono-Regular.ttf',
  'data/fonts/JetBrainsMono-Bold.ttf',
  install_dir: fonts_install_dir,
)

bt_resources = gnome.compile_resources(
  'bt-resources',
  'data/brainterm.gresource.xml',
  source_dir: 'data',
)

subdir('src')
if get_option('tests')
  subdir('tests')
endif
