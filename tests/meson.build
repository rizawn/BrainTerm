test_inc = include_directories('.')

t_pathguard = executable(
  'test_pathguard',
  'unit/test_pathguard.c',
  include_directories: test_inc,
  dependencies: [bt_core_dep, cmocka_dep],
)
test('pathguard', t_pathguard, suite: 'unit')

t_atomicwrite = executable(
  'test_atomicwrite',
  'unit/test_atomicwrite.c',
  include_directories: test_inc,
  dependencies: [bt_core_dep, cmocka_dep],
)
test('atomicwrite', t_atomicwrite, suite: 'unit')

t_scan = executable(
  'test_scan',
  'unit/test_scan.c',
  include_directories: test_inc,
  dependencies: [bt_core_dep, cmocka_dep],
)
test('scan', t_scan, suite: 'unit')

t_noteutil = executable(
  'test_noteutil',
  'unit/test_noteutil.c',
  include_directories: test_inc,
  dependencies: [bt_core_dep, cmocka_dep],
)
test('noteutil', t_noteutil, suite: 'unit')

crashrig_child = executable(
  'crashrig_child',
  'crash-rig/crashrig_child.c',
  include_directories: test_inc,
  dependencies: [bt_core_dep],
)

t_crashrig = executable(
  'test_crashrig',
  'crash-rig/test_crashrig.c',
  include_directories: test_inc,
  dependencies: [bt_core_dep, cmocka_dep],
)
test(
  'crashrig_r1',
  t_crashrig,
  suite: 'crash-rig',
  env: {'BT_CRASHRIG_CHILD': crashrig_child.full_path()},
  depends: [crashrig_child],
  timeout: 60,
)

bench_scan = executable(
  'bench_scan',
  'bench/bench_scan.c',
  include_directories: test_inc,
  dependencies: [bt_core_dep],
)
test('bench_scan_1k', bench_scan, args: ['1000'], suite: 'bench', timeout: 60)

t_vtcorpus = executable(
  'test_vtcorpus',
  'vtcorpus/test_vtcorpus.c',
  include_directories: test_inc,
  dependencies: [bt_core_dep, cmocka_dep],
)
test('vtcorpus', t_vtcorpus, suite: 'vtcorpus')

t_pty_cleanup = executable(
  'test_pty_cleanup',
  'unit/test_pty_cleanup.c',
  include_directories: test_inc,
  dependencies: [bt_core_dep, cmocka_dep],
)
test('pty_cleanup', t_pty_cleanup, suite: 'unit', timeout: 30)

bench_vterm = executable(
  'bench_vterm',
  'bench/bench_vterm.c',
  include_directories: test_inc,
  dependencies: [bt_core_dep],
)
test('bench_vterm', bench_vterm, suite: 'bench', timeout: 30)

t_checkpoint = executable(
  'test_checkpoint',
  'unit/test_checkpoint.c',
  include_directories: test_inc,
  dependencies: [bt_core_dep, cmocka_dep],
)
test('checkpoint', t_checkpoint, suite: 'unit')

t_sessions = executable(
  'test_sessions',
  'unit/test_sessions.c',
  include_directories: test_inc,
  dependencies: [bt_core_dep, cmocka_dep],
)
test('sessions', t_sessions, suite: 'unit')

t_agents_cmd = executable(
  'test_agents_cmd',
  'unit/test_agents_cmd.c',
  include_directories: test_inc,
  dependencies: [bt_core_dep, cmocka_dep],
)
test('agents_cmd', t_agents_cmd, suite: 'unit')

t_tabs = executable(
  'test_tabs',
  'unit/test_tabs.c',
  include_directories: test_inc,
  dependencies: [bt_core_dep, cmocka_dep],
)
test('tabs', t_tabs, suite: 'unit', timeout: 45)

t_frontmatter = executable(
  'test_frontmatter',
  'unit/test_frontmatter.c',
  include_directories: test_inc,
  dependencies: [bt_core_dep, cmocka_dep],
)
test('frontmatter', t_frontmatter, suite: 'unit')

t_mdparse = executable(
  'test_mdparse',
  'unit/test_mdparse.c',
  include_directories: test_inc,
  dependencies: [bt_core_dep, cmocka_dep],
)
test('mdparse', t_mdparse, suite: 'unit')

t_index = executable(
  'test_index',
  'unit/test_index.c',
  include_directories: test_inc,
  dependencies: [bt_core_dep, cmocka_dep],
)
test('index', t_index, suite: 'unit')

bench_search = executable(
  'bench_search',
  'bench/bench_search.c',
  include_directories: test_inc,
  dependencies: [bt_core_dep],
)
test('bench_search_1k', bench_search, suite: 'bench', timeout: 60)

t_watcher = executable(
  'test_watcher_conflict',
  'unit/test_watcher_conflict.c',
  include_directories: test_inc,
  dependencies: [bt_core_dep, cmocka_dep],
)
test('watcher_conflict', t_watcher, suite: 'unit')

t_organizer = executable(
  'test_organizer',
  'unit/test_organizer.c',
  include_directories: test_inc,
  dependencies: [bt_core_dep, cmocka_dep],
)
test('organizer', t_organizer, suite: 'unit')

t_graph = executable(
  'test_graph',
  'unit/test_graph.c',
  include_directories: test_inc,
  dependencies: [bt_core_dep, cmocka_dep],
)
test('graph', t_graph, suite: 'unit')

t_graph_v2 = executable(
  'test_graph_v2',
  'unit/test_graph_v2.c',
  include_directories: test_inc,
  dependencies: [bt_core_dep, cmocka_dep],
)
test('graph_v2', t_graph_v2, suite: 'unit')

t_graphlayout = executable(
  'test_graphlayout',
  'unit/test_graphlayout.c',
  include_directories: test_inc,
  dependencies: [bt_core_dep, cmocka_dep],
)
test('graphlayout', t_graphlayout, suite: 'unit')

bench_graphlayout = executable(
  'bench_graphlayout',
  'bench/bench_graphlayout.c',
  include_directories: test_inc,
  dependencies: [bt_core_dep],
)
test('bench_graphlayout', bench_graphlayout, suite: 'bench', timeout: 60)

t_linkresolve = executable(
  'test_linkresolve',
  'unit/test_linkresolve.c',
  include_directories: test_inc,
  dependencies: [bt_core_dep, cmocka_dep],
)
test('linkresolve', t_linkresolve, suite: 'unit')

t_createnote = executable(
  'test_createnote',
  'unit/test_createnote.c',
  include_directories: test_inc,
  dependencies: [bt_core_dep, cmocka_dep],
)
test('createnote', t_createnote, suite: 'unit')

t_chooser = executable(
  'test_chooser',
  'unit/test_chooser.c',
  include_directories: test_inc,
  dependencies: [bt_core_dep, cmocka_dep, glib_dep],
)
test('chooser', t_chooser, suite: 'unit')

bench_createnote = executable(
  'bench_createnote',
  'bench/bench_createnote.c',
  include_directories: test_inc,
  dependencies: [bt_core_dep],
)
test('bench_createnote', bench_createnote, suite: 'bench', timeout: 30)

t_math_tok = executable(
  'test_math_tok',
  'unit/test_math_tok.c',
  include_directories: [test_inc, include_directories('../src')],
  dependencies: [bt_math_dep, glib_dep, cmocka_dep],
)
test('math_tok', t_math_tok, suite: 'unit')

t_math_highlight = executable(
  'test_math_highlight',
  ['unit/test_math_highlight.c', '../src/ui/editorpane.c', '../src/ui/mathrender.c', '../src/ui/themeload.c', bt_resources],
  include_directories: [test_inc, include_directories('../src')],
  dependencies: [bt_core_dep, bt_math_dep, gtk4_dep, gtksource_dep, freetype_dep, cairo_dep, cmocka_dep, m_dep],
)
test(
  'math_highlight',
  t_math_highlight,
  suite: 'unit',
  is_parallel: false,
  env: {'BT_SOURCE_ROOT': meson.project_source_root()},
)

t_editor_links = executable(
  'test_editor_links',
  [
    'unit/test_editor_links.c',
    '../src/app.c',
    '../src/terminal.c',
    '../src/graphview.c',
    '../src/ui/graphgl.c',
    '../src/ui/graphcapture.c',
    '../src/ui/filetree.c',
    '../src/ui/editorpane.c',
    '../src/ui/mathrender.c',
    '../src/ui/createpopover.c',
    '../src/ui/chooserpopover.c',
    '../src/ui/quickopen.c',
    '../src/ui/termtabs.c',
    '../src/ui/sessionspanel.c',
    '../src/ui/askbar.c',
    '../src/ui/themeload.c',
    bt_resources,
    bt_build_stamp_h,
  ],
  include_directories: [test_inc, include_directories('../src')],
  dependencies: [bt_core_dep, bt_math_dep, gtk4_dep, adw_dep, epoxy_dep, gtksource_dep, freetype_dep, cairo_dep, cmocka_dep, m_dep],
)
test(
  'editor_links',
  t_editor_links,
  suite: 'unit',
  is_parallel: false,
  env: {'BT_SOURCE_ROOT': meson.project_source_root()},
)

bench_graph = executable(
  'bench_graph',
  ['bench/bench_graph.c', '../src/graphview.c', '../src/ui/graphgl.c', '../src/ui/graphcapture.c', bt_resources],
  include_directories: [test_inc, include_directories('../src')],
  dependencies: [bt_core_dep, gtk4_dep, epoxy_dep],
)
test('bench_graph_1k', bench_graph, suite: 'bench', is_parallel: false, timeout: 30)

bench_graph_gl = executable(
  'bench_graph_gl',
  ['bench/bench_graph_gl.c', '../src/ui/graphgl.c', bt_resources, bt_build_stamp_h],
  include_directories: [test_inc, include_directories('../src')],
  dependencies: [bt_core_dep, gtk4_dep, epoxy_dep],
)
test('bench_graph_gl', bench_graph_gl, suite: 'bench', is_parallel: false, timeout: 60)

t_importers = executable(
  'test_importers',
  'unit/test_importers.c',
  include_directories: test_inc,
  dependencies: [bt_core_dep, cmocka_dep],
)
test('importers', t_importers, suite: 'unit')

t_embeddings = executable(
  'test_embeddings',
  'unit/test_embeddings.c',
  include_directories: test_inc,
  dependencies: [bt_core_dep, cmocka_dep],
)
test('embeddings', t_embeddings, suite: 'unit')

t_mcp = executable(
  'test_mcp',
  'unit/test_mcp.c',
  include_directories: test_inc,
  dependencies: [bt_core_dep, cmocka_dep],
)
test(
  'mcp',
  t_mcp,
  suite: 'unit',
  env: {'BT_MCP_BIN': bt_mcp_exe.full_path()},
  depends: [bt_mcp_exe],
  timeout: 30,
)

t_theme = executable(
  'test_theme',
  ['unit/test_theme.c', '../src/graphview.c', '../src/ui/graphgl.c', '../src/ui/graphcapture.c', bt_resources],
  include_directories: [test_inc, include_directories('../src')],
  dependencies: [bt_core_dep, gtk4_dep, epoxy_dep, cmocka_dep],
)
test(
  'theme',
  t_theme,
  suite: 'unit',
  is_parallel: false,
  env: {'BT_SOURCE_ROOT': meson.project_source_root()},
)

t_fix_ui = executable(
  'test_fix_ui',
  [
    'unit/test_fix_ui.c',
    '../src/app.c',
    '../src/terminal.c',
    '../src/graphview.c',
    '../src/ui/graphgl.c',
    '../src/ui/graphcapture.c',
    '../src/ui/filetree.c',
    '../src/ui/editorpane.c',
    '../src/ui/mathrender.c',
    '../src/ui/createpopover.c',
    '../src/ui/chooserpopover.c',
    '../src/ui/quickopen.c',
    '../src/ui/termtabs.c',
    '../src/ui/sessionspanel.c',
    '../src/ui/askbar.c',
    '../src/ui/themeload.c',
    bt_resources,
    bt_build_stamp_h,
  ],
  include_directories: [test_inc, include_directories('../src')],
  dependencies: [bt_core_dep, gtk4_dep, adw_dep, epoxy_dep, gtksource_dep, freetype_dep, cairo_dep, cmocka_dep, m_dep],
)
test(
  'fix_ui',
  t_fix_ui,
  suite: 'unit',
  is_parallel: false,
  env: {'BT_SOURCE_ROOT': meson.project_source_root()},
  timeout: 30,
)
