platform_src = is_windows ? files('platform_win.c', 'pty_win.c') : files('platform_unix.c', 'pty_unix.c')

core_sources = files(
  'bterror.c',
  'vault.c',
  'noteutil.c',
  'vtermbridge.c',
  'checkpoint.c',
  'agents.c',
  'sessions.c',
  'frontmatter.c',
  'mdparse.c',
  'index.c',
  'search.c',
  'watcher.c',
  'organizer.c',
  'graph.c',
  'importer.c',
  'embed.c',
  'theme.c',
) + platform_src

bt_core_lib = static_library(
  'bt_core',
  core_sources,
  dependencies: [glib_dep, gio_dep, vterm_dep, git2_dep, cjson_dep, yaml_dep, sqlite_dep, md4c_dep, m_dep] + util_dep,
  include_directories: include_directories('.'),
)

bt_core_dep = declare_dependency(
  link_with: bt_core_lib,
  include_directories: include_directories('.'),
  dependencies: [glib_dep, gio_dep, vterm_dep, git2_dep, cjson_dep, yaml_dep, sqlite_dep, md4c_dep, m_dep] + util_dep,
)

app_sources = files(
  'main.c',
  'app.c',
  'terminal.c',
  'graphview.c',
  'ui/filetree.c',
  'ui/editorpane.c',
  'ui/quickopen.c',
  'ui/termtabs.c',
  'ui/sessionspanel.c',
  'ui/askbar.c',
)

langspec_abs = get_option('prefix') / langspec_install_dir
styles_abs = get_option('prefix') / styles_install_dir

bt_exe = executable(
  'bt',
  app_sources + [bt_resources],
  dependencies: [bt_core_dep, gtk4_dep, adw_dep, gtksource_dep, m_dep],
  c_args: [
    '-DBT_LANGSPEC_DIR="@0@"'.format(langspec_abs),
    '-DBT_STYLES_DIR="@0@"'.format(styles_abs),
  ],
  install: true,
)

bt_mcp_exe = executable(
  'bt-mcp',
  files('mcp/main.c'),
  dependencies: [bt_core_dep],
  install: true,
)
