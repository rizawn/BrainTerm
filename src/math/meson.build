math_sources = files(
  'mathtok.c',
)

bt_math_lib = static_library(
  'bt_math',
  math_sources,
  dependencies: [glib_dep, harfbuzz_dep],
  include_directories: include_directories('..', '.'),
)

bt_math_dep = declare_dependency(
  link_with: bt_math_lib,
  include_directories: include_directories('..', '.'),
  dependencies: [glib_dep, harfbuzz_dep],
)
