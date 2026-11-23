subdir('math')

platform_src = is_windows ? files('platform_win.c', 'pty_win.c') : files('platform_unix.c', 'pty_unix.c')

core_sources = files(
  'bterror.c',
  'vault.c',
  'noteutil.c',
  'linkresolve.c',
  'createnote.c',
  'choosermodel.c',
  'navhistory.c',
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
  'graph_state.c',
  'importer.c',
  'embed.c',
  'theme.c',
) + platform_src

bt_core_lib = static_library(
  'bt_core',
  core_sources,
  dependencies: [glib_dep, gio_dep, vterm_dep, git2_dep, cjson_dep, yaml_dep, sqlite_dep, md4c_dep, m_dep, bt_math_dep] + util_dep,
  include_directories: include_directories('.'),
)

bt_core_dep = declare_dependency(
  link_with: bt_core_lib,
  include_directories: include_directories('.'),
  dependencies: [glib_dep, gio_dep, vterm_dep, git2_dep, cjson_dep, yaml_dep, sqlite_dep, md4c_dep, m_dep, bt_math_dep] + util_dep,
)

app_sources = files(
  'main.c',
  'app.c',
  'terminal.c',
  'graphview.c',
  'ui/graphgl.c',
  'ui/graphcapture.c',
  'ui/filetree.c',
  'ui/editorpane.c',
  'ui/mathrender.c',
  'ui/createpopover.c',
  'ui/chooserpopover.c',
  'ui/quickopen.c',
  'ui/termtabs.c',
  'ui/sessionspanel.c',
  'ui/askbar.c',
  'ui/themeload.c',
)

# Bundled fonts: Windows registers them as GDI private fonts (Pango uses its win32
# font map there); elsewhere fontconfig + PangoFc (FcConfigAppFontAddFile).
fontconfig_dep = dependency('fontconfig', required: false, include_type: 'system')
pangoft2_dep = dependency('pangoft2', required: false, include_type: 'system')
use_fc = not is_windows and fontconfig_dep.found() and pangoft2_dep.found()
font_args = use_fc ? ['-DBT_HAVE_FONTCONFIG=1'] : []
font_deps = use_fc ? [fontconfig_dep, pangoft2_dep] : []

langspec_abs = get_option('prefix') / langspec_install_dir
styles_abs = get_option('prefix') / styles_install_dir

bt_exe = executable(
  'bt',
  app_sources + [bt_resources, bt_build_stamp_h],
  dependencies: [bt_core_dep, gtk4_dep, adw_dep, epoxy_dep, gtksource_dep, freetype_dep, cairo_dep, m_dep] + font_deps,
  c_args: font_args + [
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
