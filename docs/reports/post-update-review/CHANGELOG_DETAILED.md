# BrainTerm — Detailed Post-Update Changelog & Change Audit

This changelog documents BrainTerm's evolution from baseline commits on `origin/master`, through all **7 local commits ahead (`7861933`..`f0eef3c`)**, across every **uncommitted working-tree modification (`42` tracked files, `+3,185 / -198` lines, plus `5` newly created `docs/*.md` files)** verified on `2026-11-27`.

---

## 1. Review Scope & Historical Baseline

- **Pre-Update Baseline (`e8a6ca3` through `origin/master` `2086213` / `1b54694`):**
  - `e8a6ca3` (`2026-11-22`): Initial baseline implementing core M1–M8 architecture (vault safety, SQLite WAL/FTS5, ConPTY/`libvterm`, `libgit2` checkpoints, CLI launch hooks, chat importers, organizer heuristics, Ink styling).
  - `dfeec4b`–`7e92d98` (`2026-11-22`): Dusk design tokens, bundled OFL typography (`InterVariable`, `JetBrainsMono`), `16` symbolic SVG icons, build stamp injection, and lock-safe `scripts/package-win.ps1`.
  - `705dc22`–`89ed2de` (`2026-11-22`): Brain Graph v2 OpenGL 3.3 Core / GLES 3.0 instanced renderer, half-resolution Gaussian bloom FBOs, and automatic Cairo software fallback.
  - `d02c02b`–`1b54694` (`2026-11-23`): Clickable Links (`L1–L3`: single `linkresolve.c`, non-overwriting `createnote.c`, ambiguity `choosermodel.c`, `navhistory.c`), HarfBuzz OpenType `MATH` typography (`libbt_math` `M1–M5`), post-flight `PF-1`–`PF-8` hardening, and README overhaul (`2086213`).
- **Update Stage 1 — Seven Committed Graph Redesign Commits (`7861933`..`f0eef3c`):**
  - Rebuilt Brain Graph (`2026-11-23`..`2026-11-25`) with collapsible folder nodes, Z-sorted spatial picking, `12` Andromeda folder colors, **Galaxy** spiral projection, **Timeline** view (`Organic` / `Kept`), pure-C Obsidian-style 2D layout (`G1`), and `Clean | Glow` presets (`G2`).
- **Update Stage 2 — Uncommitted Pre-Beta Completion & Hardening (`2026-11-25`..`2026-11-27`):**
  - Added **G3** interactive pin/reheat/VBO fast-path/orphan highlighting, closed GUI workflows (`Ctrl+F/H`, Starter Vault menu, `>import chats`, `>review ai tags`, `>sync embeddings`, `Alt+B`, `F1` shortcuts), synced `BtNavHistory`, hardened input bounds and metachar filters, completed `bt-mcp` v2, and finalized CI, packaging, tests, and documentation.

---

## 2. Commit-by-Commit Audit (`origin/master..HEAD`)

### Commit 1 — `7861933` (`2026-11-23 20:00:51`)
- **Subject:** `graph: phase 1 interaction engine, folder nodes, spatial picking, anti-blob rendering`
- **Diff Summary:** `14` files changed (`+1,015 / -130` lines)
- **Problem Before:** 3D nodes blended additively into a white clump, hit-testing ignored Z-depth in overlaps, and folders were absent from graph topology.
- **Changes Applied:**
  - Added `BT_NODE_FOLDER`, `BT_EDGE_CONTAINS`, `top_folder`, `parent_idx`, `child_count`, `collapsed`, and `bt_graph_recompute_visibility()` in `src/graph.h`/`.c`.
  - Built uniform spatial-grid depth picking `bt_graph_pick_screen()`, fisheye hover spread `bt_graph_apply_hover_spread()`, and 4-way label placement `bt_graph_place_labels()` in `src/graph_state.h`/`.c`.
  - Updated `graph_node.vert.glsl`, `graph_node.frag.glsl`, `graph_edge.vert.glsl`, `src/ui/graphgl.c`, and `src/graphview.c` (`Tab` overlap cycling, `Space` collapse, context menu).
- **Verification Added:** `tests/unit/test_graph_v2.c` (`+25` lines).

### Commit 2 — `bd42b04` (`2026-11-23 20:05:12`)
- **Subject:** `graph: phase 2 galaxy folder color palette, scrollable legend, and .brainterm persistence`
- **Diff Summary:** `9` files changed (`+865 / -18` lines)
- **Problem Before:** Graph nodes lacked visual folder grouping.
- **Changes Applied:**
  - Created `src/graph_palette.h`/`.c` with `BT_GALAXY_PALETTE_COUNT = 12` Andromeda swatches, redmean perceptual distance `bt_color_perceptual_dist()`, synthesized fallback colors, `+/-8%` child note shading, and `<vault>/.brainterm/folder-colors.json` JSON persistence.
  - Excluded `.brainterm` in `src/vault.c`, registered `graph_palette.c` in `src/meson.build`, and added a scrollable legend in `src/graphview.c`.
- **Verification Added:** `tests/unit/test_graph_v2.c` (`+63` lines).

### Commit 3 — `9299559` (`2026-11-23 20:10:43`)
- **Subject:** `graph: phase 3 galaxy spiral disk view mode with core bulge and 700ms morph transition`
- **Diff Summary:** `6` files changed (`+463 / -54` lines)
- **Problem Before:** Brain Graph lacked a macro-topological clustered galaxy projection.
- **Changes Applied:**
  - Added `BT_GRAPH_MODE_GALAXY`, `BtGalaxyParams`, `bt_graph_compute_galaxy_layout()` (3–5 logarithmic arms, central bulge, thin disk, `3%` outer halo, rim warp), and differential rotation `bt_galaxy_apply_diff_rot()`.
  - Implemented `700 ms` animated transitions across `src/graphview.c`/`src/ui/graphgl.c` plus bulge shading in `data/shaders/graph_bg.frag.glsl`.
- **Verification Added:** `tests/unit/test_graph_v2.c` (`+50` lines).

### Commit 4 — `d617584` (`2026-11-23 20:16:57`)
- **Subject:** `graph: phase 4 timeline view mode, organic/kept toggle, brushing, ledger, and mode persistence`
- **Diff Summary:** `6` files changed (`+981 / -2` lines)
- **Problem Before:** Users could not view note history alongside git checkpoints, sessions, imports, and AI runs on one chronology axis.
- **Changes Applied:**
  - Created `src/graph_timeline.h`/`.c` (`bt_timeline_parse_date()`, collision-free **Organic** beeswarm / **Kept** grid layout `bt_timeline_build()`, `BT_TL_LANE_FOLDER`/`TYPE` lanes, ledger entries `BT_LEDGER_CHECKPOINT`/`SESSION`/`IMPORT`/`AI_EDIT`).
  - Wired timeline toggles, drag brushing, and `state.ini` persistence in `src/graphview.c` and `src/app.c`.
- **Verification Added:** `tests/unit/test_graph_v2.c` (`+44` lines).

### Commit 5 — `003e475` (`2026-11-25 08:37:33`)
- **Subject:** `graph: baseline prior galaxy/timeline updates and add obsidian 2d reference`
- **Diff Summary:** `15` files changed (`+643 / -112` lines)
- **Changes Applied:**
  - Tuned GL shaders (`graph_bg.frag.glsl`, `graph_edge.vert.glsl`, `graph_node.vert.glsl`), updated `data/theme.css`, and refined `scripts/gen_build_stamp.py`.
  - Updated `src/graph_state.h`/`.c`, `src/graphview.h`/`.c`, `src/ui/graphgl.c`, `src/app.c`, `test_fix_ui.c`, and `test_graph_v2.c`.
  - Committed Obsidian 2D target layout reference image `docs/reference/obsidian-graph-2d.png`.

### Commit 6 — `43205a7` (`2026-11-25 08:43:41`)
- **Subject:** `graphlayout: implement pure-C 2D clean force layout, halo ring, de-overlap, tests, and bench (G1)`
- **Diff Summary:** `8` files changed (`+1,629 / -101` lines)
- **Problem Before:** Legacy 2D relaxation allowed overlapping discs and irregular spacing across small/orphan components.
- **Changes Applied:**
  - Implemented pure-C `src/graphlayout.h`/`.c` (`BtGraphLayoutOpts`, `BtGraphLayoutState`, deterministic `0x5A17C0DEu` RNG, spatial-grid repulsion, component packing, halo ring, `12–40` pass disc de-overlap, warm start, pinning, NaN reset counter), wired into `src/graph.c` and `src/meson.build`.
- **Verification Added:** `tests/unit/test_graphlayout.c` (`290` lines) and `tests/bench/bench_graphlayout.c` (`85` lines).

### Commit 7 — `f0eef3c` (`2026-11-25 08:54:33`)
- **Subject:** `graph: implement Clean Obsidian-style 2D preset in Cairo/GL, Style toggle, and state.ini persistence (G2)`
- **Diff Summary:** `8` files changed (`+261 / -47` lines)
- **Changes Applied:** Added `BtGraphStyle` (`BT_GRAPH_STYLE_CLEAN`, `BT_GRAPH_STYLE_GLOW`), crisp flat-dot fragment shading (`data/shaders/graph_node.frag.glsl`, `src/ui/graphgl.c`), Cairo Clean mode + `Style: Clean | Glow` controls (`src/graphview.c`), and `[graph] style` persistence (`src/app.c`).

---

## 3. Complete Inventory of Uncommitted Changes (`42` Tracked + `5` New Docs)

Working-tree aggregate (`git diff --stat`): **`42` modified tracked files, `3,185` insertions (`+`), `198` deletions (`-`)**, concentrated most heavily in `src/app.c` (`945`), `src/graphview.c` (`428`), `src/ui/editorpane.c` (`356`), `src/index.c` (`218`), and `src/mcp/main.c` (`140`).

### 3.1 Brain Graph Interactive `G3` & GPU Fast-Path (`5` Tracked Files)
- `src/graphlayout.c` (`+8`) — Reset `iter_done` inside `reheat()` for continuous neighbor pull; mirrored `set_pos()` directly back to `BtGraphNode`.
- `src/ui/graphgl.h` (`+1`) — Declared position-only dirty marker `bt_graph_gl_mark_positions_dirty()`.
- `src/ui/graphgl.c` (`+75 / -32`) — Skipped costly Pango `2048x1024` texture atlas rebuilds during drag/settle frames by uploading only node/edge VBO buffers; rendered `highlight_orphans`.
- `src/ui/graphview_renderer.h` (`+1`) — Added `highlight_orphans` field to `BtGraphRenderState`.
- `src/graphview.c` (`+410 / -18`) — Added **G3** 2D drag-to-pin, `Pin`/`Unpin` context menus, zero idle CPU stop on equilibrium, `Re-layout` (`R`), `Orphans` (`O`), and `GTK_ACCESSIBLE_PROPERTY_LABEL` tags.

### 3.2 Core SQLite Index & Offline Vector Caching (`4` Tracked Files)
- `src/index.h` (`+22`) — Declared `BtEmbeddingRow`, `BtPendingAiTag`, and single-writer UI/MCP query APIs.
- `src/index.c` (`+203 / -15`) — Implemented synchronous `bt-db-writer` queries for recent notes (`ORDER BY updated DESC`), orphans, pending AI tags, backlinks, persisted embeddings, and counts.
- `src/embed.h` (`+5 / -1`) — Added `bt_embed_count_cached()` and documented `vault == NULL` safe cached hybrid lookup.
- `src/embed.c` (`+47 / -9`) — Loaded persisted SQLite BLOBs once per query and guarded `vec_is_finite()` / `isfinite()` against `NaN`/`Inf` entries.

### 3.3 Security, Bounds & Automation Hardening (`6` Tracked Files)
- `src/frontmatter.c` (`+5`) — Added `ylen > (256u * 1024u)` YAML header ceiling (`parse_error = TRUE`).
- `src/importer.c` (`+86 / -9`) — Added `read_import_file_bounded()` (`64 MiB` cap), YAML title escaping, FNV-1a `untitled-%.6s` non-ASCII slugs, and `BT_ERROR_INVALID` on 0-object JSONL inputs.
- `src/organizer.c` (`+30 / -3`) — Preserved corrupt `.bt/dismissed_tags.json` as `.bak` with `g_warning()`, enforced `R9` malformed YAML refusal across promote/dismiss, and logged write failures.
- `src/agents.c` (`+5 / -4`) — Stripped `'%'`, `'\''`, `'!'` alongside POSIX metachars in `sanitize_title_literal()`.
- `src/search.h` (`+4`) — Declared `bt_search_compose_ask_agent_cmd_for()` for user-selectable CLIs.
- `src/search.c` (`+25 / -3`) — Implemented configurable Ask prompt formatting (`claude`/`codex`/`gemini`) and aligned `append_sanitized()` with `'%'`, `'\''`, `'!'` masking.

### 3.4 Editor, Navigation History, Ask Bar, Tabs & Main App (`10` Tracked Files)
- `src/navhistory.h` (`+3`) — Declared `bt_nav_history_rename_path()` and `bt_nav_history_remove_path()`.
- `src/navhistory.c` (`+26`) — Synchronized stored `(relpath, caret_offset, scroll_y)` entries across note renames and deletions (`editor_links` PASS).
- `src/ui/editorpane.h` (`+6`) — Declared Find/Replace trigger (`bt_editorpane_show_find`) and onboarding action setters.
- `src/ui/editorpane.c` (`+350`) — Added collapsible `Ctrl+F` Find / `Ctrl+H` Replace (`#bt-find-bar`, `#bt-replace-row`), `GtkSourceSearchContext` counts, and Starter Vault onboarding buttons (`editor_links` PASS).
- `data/styles/bt-ink-dark.xml` (`+1`) — Added dark `<style name="search-match" .../>` editor highlight token.
- `data/styles/bt-ink-light.xml` (`+1`) — Added light `<style name="search-match" .../>` editor highlight token.
- `src/ui/askbar.h` (`+8`) — Declared extended `bt_askbar_show_ex()` with hybrid state, CLI badge, and toggle callback.
- `src/ui/askbar.c` (`+127`) — Added 10-entry query history (`Alt+Up`/`Down`), `Up`/`Down` list selection, `Hybrid` toggle, and active CLI badge (`--smoke-test` PASS).
- `src/ui/termtabs.c` (`+39`) — Restored VTE keyboard focus across tab switches/closes, added middle-click tab closure, and set accessible tooltips/labels (`tabs` PASS).
- `src/ui/filetree.c` (`+2`) — Added `"Vault notes"` `GTK_ACCESSIBLE_PROPERTY_LABEL` on the vault explorer tree (`fix_ui` PASS).
- `src/app.c` (`+919 / -26`) — Wired `>import chats`, `>review ai tags`, `>sync embeddings`, `Alt+B`, `New Starter Vault…`, searchable `F1` Keyboard Shortcuts modal (`show_shortcuts_dialog()`), Preferences controls (`show_line_numbers`, `hybrid_enabled`, `preferred_cli`), post-rollback UI/index sync, `navhistory` rename/delete hooks, and header/sidebar a11y labels.

### 3.5 MCP Bridge, Build, Packaging, Unit Tests (`10`) & Docs (`17` Modified + `5` New)
- `src/mcp/main.c` (`+123 / -17`) — Switched to `vault_open_readonly()`, raised JSON-RPC line buffer from `64 KB` to **`8 MiB`** (`read_json_line`), added `capabilities` and `top_k` + `hybrid` schemas, sorted `recent` descending, and exposed `backlinks` (`mcp` PASS + live E2E).
- `.github/workflows/ci.yml` (`+14`) — Added `master` push trigger, MSYS2/Ubuntu library dependencies, and `-j 1` test serialization.
- `meson.build` (`+2`) — Installed font license texts (`LICENSE-*.txt`, `OFL-*.txt`) into the bundle font directory.
- `scripts/package-win.ps1` (`+49`) — Resolved `bt-mcp.exe` DLLs, verified SHA-256 parity + isolated `PATH` smoke tests (`bt.exe` & `bt-mcp.exe`), and populated `dist/brainterm/licenses/`.
- **Expanded Unit Tests (`10` modified test files, `+508 / -6` lines):**
  - `tests/unit/test_agents_cmd.c` (`+16`) — Hostile `%`, `'`, `!` title/Ask sanitization cases.
  - `tests/unit/test_editor_links.c` (`+92`) — Find/Replace show/hide, shortcut audit, and `BtNavHistory` rename/delete tests.
  - `tests/unit/test_embeddings.c` (`+51`) — Persisted SQLite cache lookup, `vault == NULL` mode, and `NaN` vector safety.
  - `tests/unit/test_fix_ui.c` (`+74`) — Graph G3 `Orphans`, `Re-layout`, mode restore, and accessible control coverage.
  - `tests/unit/test_frontmatter.c` (`+16`) — `>256 KiB` YAML slice rejection test (`parse_error = TRUE`).
  - `tests/unit/test_graphlayout.c` (`+41`) — G3 `reheat()`, `set_pos()`, and zero-overlap stability assertions.
  - `tests/unit/test_importers.c` (`+88`) — `64 MiB` bound logic, hashed non-ASCII slugs, YAML title escaping, and 0-object JSONL error tests.
  - `tests/unit/test_index.c` (`+48`) — `recent`, `orphan`, `pending_ai_tags`, `backlinks`, and `embeddings` SQL query assertions.
  - `tests/unit/test_mcp.c` (`+69`) — `top_k`/`hybrid` schema, `backlinks`, read-only `.md.tmp` preservation, and `>64 KB` reader test.
  - `tests/unit/test_organizer.c` (`+46`) — Corrupt `.bt/dismissed_tags.json` `.bak` backup and `R9` malformed YAML refusal tests.
- **Updated Core Documentation (`2` files):**
  - `README.md` (`69` lines changed) — Updated verified capabilities, build/smoke commands, shortcuts, and read-only MCP setup.
  - `CHANGELOG.md` (`21` lines changed) — Documented Graph `G1`–`G3`, GUI workflow closures, hardening fixes, and test telemetry.
- **Newly Added Release Documentation (`5` files in `docs/`):**
  - [DEVELOPMENT_STATUS.md](../../DEVELOPMENT_STATUS.md) — Current subsystem readiness summary and verified toolchain matrix.
  - [KNOWN_LIMITATIONS.md](../../KNOWN_LIMITATIONS.md) — Accurate `128-dim` vector, UI split, sanitizer, and unverified platform disclosures.
  - [RELEASE_CHECKLIST.md](../../RELEASE_CHECKLIST.md) — Automated gates plus manual Windows/Linux/IME QA checklist.
  - [ROADMAP.md](../../ROADMAP.md) — Scoped post-pre-beta milestones (`v0.1` -> `v0.2` -> `v0.3`).
  - [THIRD_PARTY_LICENSES.md](../../THIRD_PARTY_LICENSES.md) — Runtime DLL, static library, and bundled OFL font license attribution index.

---

## 4. Before-and-After Subsystem Comparison

| Subsystem | Before Update (`origin/master`) | Implemented Changes | State After Update (`2026-11-27`) | Verification |
|---|---|---|---|---|
| **3D / Graph Modes** | Additive white point blob; no folders | `7861933`..`d617584`: folders, depth pick, Andromeda, Galaxy, Timeline | 4 view modes, depth picking, 12 folder hues, beeswarm/kept timeline | `graph_v2`, `bench_graph_gl` |
| **2D Graph & `G3`** | Spine relaxation; static canvas | `43205a7`/`f0eef3c` + G3 pin, reheat, VBO fast-path, `R`/`O` keys | Non-overlapping `Clean`/`Glow` 2D, drag-to-pin, zero idle CPU | `graphlayout`, `bench_graphlayout`, `fix_ui` |
| **Editor, Links & Nav** | No Find/Replace; minimal placeholder; stale nav | `bt-find-bar`, Starter Vault UI, `F1` modal, `navhistory` sync | Find/Replace, onboarding, searchable shortcuts, rename-safe history | `editor_links` (`27` cases, `0` warnings) |
| **Memory UI & Vectors** | Backend-only importers/organizer/vectors | `>import chats`, `>review ai tags`, `>sync embeddings`, `Alt+B`, cache | Complete GUI/CLI parity, `vault == NULL` hybrid query, `NaN` protection | `importers`, `organizer`, `embeddings`, `index` |
| **Security & Limits** | Unfiltered `%`/`'`/`!`; unbounded YAML/imports | `%`/`'`/`!` filter, `256 KiB`/`64 MiB` limits, slugs, `.bak` backup | Blocks `cmd.exe` `%VAR%`, avoids OOM/collisions, preserves tag locks | `agents_cmd`, `frontmatter`, `importers`, `organizer` |
| **MCP Bridge (`bt-mcp`)** | 64 KB buffer; no `backlinks`/`top_k` schema | Read-only vault open, `8 MiB` line limit, full schemas, `backlinks` | Verified stdio server packaged with DLLs & SHA-256 parity check | `mcp`, live E2E, `package-win.ps1` |
