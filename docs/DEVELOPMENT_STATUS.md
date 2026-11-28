# BrainTerm Development Status & Technical Audit

**Latest Audit & Engineering Update:** 2026-11-27  
**Repository Base Branch:** `master` @ `f0eef3c80855cdf9baa59a70e82bb32f0b448ae8` (`[origin/master: ahead 7]` + working-tree pre-beta completion)  
**Version / Stamp:** `BrainTerm 0.1.0 (build f0eef3c-dirty · 2026-11-27 17:48:43)`  
**Verified Build Profile:** MSYS2 UCRT64 C17 (`-Dwerror=true -Wall -Wextra -Wpedantic -Wshadow -Wformat=2`), Windows 11  

---

## 1. Executive Summary & Overall Maturity

**Estimated Maturity:** **Public Pre-Beta Candidate (Windows Portable Verified; Linux Desktop Interactive QA Pending)**

BrainTerm is a native C17/GTK4 desktop application (`99` C/H source files in `src/`, `39` C/H files in `tests/`). All major core subsystems — vault path containment (`vault_resolve`), crash-safe atomic writes (`.md.tmp` write + `fsync` + atomic replace + dir sync), SQLite WAL single-writer indexing (`BtIndex`), `libgit2` checkpoints and post-rollback UI/index sync, ConPTY/Job-Object terminal tabs, Clickable Links (`L1–L3`), in-place OpenType LaTeX math (`M1–M5`), Find & Replace, Starter Vault onboarding, Brain Graph (`2D Clean/Glow`, `3D`, `Galaxy`, `Timeline` + `G3` drag-to-pin/re-layout/orphan highlight), GUI importers/tag-review/hybrid-search, and read-only `bt-mcp` v2 — are wired end-to-end and verified by the automated C/CMocka suite and isolated Windows bundle smoke tests.

Remaining gaps before a general v1.0 release are explicitly tracked in [KNOWN_LIMITATIONS.md](KNOWN_LIMITATIONS.md) and [RELEASE_CHECKLIST.md](RELEASE_CHECKLIST.md):
1. **Top-level `LICENSE` file:** Author clarification required before publishing binaries/source under an OSI license ([README.md](../README.md)).
2. **Interactive Cross-Platform / IME QA:** Linux `pty_unix.c` / GUI runs in Ubuntu GitHub Actions CI under `xvfb-run`, and Windows CJK/dead-key IME + live TUI resize stress (`vim`, `htop`, `claude`) still require interactive human testing.
3. **Logseq Parity Spec (`docs/logseq-parity.md`):** Phases `P1–P9` (`tasks.c`, `query.c`, link-rewriting `rename.c`, static `exporter.c`, migrations `0003..0006`) remain specification-only.

---

## 2. Architecture Layering (Verified)

- **`libbt_math` (`src/math/`):** GTK-free, Cairo-free C17 LaTeX tokenizer, parser, HarfBuzz `MATH` font loader, and box layout engine (`mathtok.c`, `mathcmds.c`, `mathparse.c`, `mathfont.c`, `mathlayout.c`).
- **`libbt_core` (`src/`):** GTK-free C17 core logic: vault I/O (`vault.c`), frontmatter (`frontmatter.c`), MD4C extraction (`mdparse.c`), SQLite index & query helpers (`index.c`), link resolver & create-note planner (`linkresolve.c`, `choosermodel.c`, `createnote.c`), file watcher (`watcher.c`), git checkpointing (`checkpoint.c`), ConPTY/openpty & `libvterm` bridge (`pty_win.c`, `pty_unix.c`, `vtermbridge.c`), importers/organizer/embeddings (`importer.c`, `organizer.c`, `embed.c`), search (`search.c`), and pure-C graph layout/state/palette/timeline (`graph.c`, `graphlayout.c`, `graph_state.c`, `graph_palette.c`, `graph_timeline.c`).
- **GTK4 / `libadwaita` UI (`src/app.c`, `src/terminal.c`, `src/graphview.c`, `src/ui/`):** Window shell (`app.c`), `GtkSourceView 5` editor with Find & Replace and onboarding (`editorpane.c`, `mathrender.c`), terminal widget & middle-click/focus-preserving tabs (`terminal.c`, `termtabs.c`), file tree (`filetree.c`), popovers & search bar (`quickopen.c`, `askbar.c`, `chooserpopover.c`, `createpopover.c`), sessions panel (`sessionspanel.c`), and dual Cairo/OpenGL (`GtkGLArea`) graph viewports (`graphview.c`, `graphgl.c`).
- **External MCP Binary (`src/mcp/main.c` -> `bt-mcp`):** Standalone stdio JSON-RPC 2.0 binary linking only `libbt_core`, opening `vault/.bt/index.db` strictly read-only.

---

## 3. Feature Inventory vs Reality

| Area | Status | Notes (Source-Verified) |
| :--- | :--- | :--- |
| Vault scan, path guard, atomic write, crash recovery | **Complete** | `vault.c`; verified by `pathguard`, `atomicwrite`, `scan`, `crashrig_r1` |
| File watcher & conflict resolution | **Complete** | `watcher.c`, `app.c`, `editorpane.c`; toast reload / dirty banner / checkpoint diff |
| YAML frontmatter + MD4C parser | **Complete** | `frontmatter.c` (`libyaml`), `mdparse.c` (`md4c`) |
| SQLite FTS5 single-writer index + migrations `0001..0002` | **Complete** | `index.c`; plus `recent`, `backlinks`, `orphan_notes`, `pending_ai_tags`, `embeddings` queries |
| Editor Find & Replace, line numbers & Starter Vault | **Complete** | `Ctrl+F`/`Ctrl+H`, `search-match` schemes, `[editor] show_line_numbers`, `vault-template/` copy guard |
| Clickable links, create/chooser popovers, nav history, settings | **Complete** | `linkresolve.c`, `createnote.c`, `choosermodel.c`, `navhistory.c`, `editorpane.c`; `--selftest-links` |
| Live in-place LaTeX math (`$...$`, `$$...$$`) | **Complete** | `src/math/*`, `mathrender.c`, `editorpane.c` (`M1–M5`) |
| Embedded multi-tab ConPTY + `libvterm` terminal + agent launch | **Complete (Win)** / **CI-only (Linux)** | `pty_win.c` + CLI preflight check + tab focus/middle-click polish; `pty_unix.c` compiled/tested in Ubuntu CI only |
| Session persistence, resume & rollback sync | **Complete** | `sessions.c`, `sessionspanel.c`, `app.c` `sync_after_vault_mutation()` |
| Ask / Search / Command Bar (`Ctrl+E`, `Ctrl+P`) | **Complete** | Keyboard row/history navigation, `Hybrid` FTS5+vector toggle, configurable `[agent] preferred_cli` (`claude`/`codex`/`gemini`) |
| Brain Graph (`2D Clean/Glow`, `3D`, `Galaxy`, `Timeline` + `G3`) | **Complete** | Interactive 2D drag-to-pin, live spring settling (0 idle CPU), fast GL position VBO update, `Re-layout` (`R`), `Highlight Orphans` (`O`) |
| Auto-organizer (`tags_ai` + human lock + UI review) | **Complete (`NO_AI`/`CLI`)** | `organizer.c` (`R9` malformed-YAML refusal on promote/dismiss) + `>review ai tags` modal (`Promote`/`Dismiss`) |
| Chat / session importers (`>import chats`) | **Complete** | `importer.c` YAML-escaped writer + GTK file dialog for ChatGPT/Claude/Codex JSON & JSONL |
| Offline embeddings & hybrid RRF search | **Complete (lexical hash vectors)** | `embed.c` reads cached SQLite `embeddings` blobs (`vault == NULL` safe); `>sync embeddings` UI command |
| Read-only MCP stdio server (`bt-mcp` v2) | **Complete** | `capabilities`, `inputSchema`, `ORDER BY updated DESC` `recent`, `backlinks`, `hybrid` flag on `search_memory`, >64 KB reader |
| Logseq parity Phases `P1–P9` (tasks/Datalog query/rename/export) | **Unstarted** | Spec in `docs/logseq-parity.md`; deferred beyond pre-beta |

---

## 4. Key Improvements Completed

1. **Core DB, Embeddings, Fault Isolation & MCP Hardening:**
   - `src/index.c`: Added `bt_index_list_recent_notes()`, `bt_index_list_orphan_notes()`, `bt_index_list_backlinks()`, `bt_index_list_pending_ai_tags()`, `bt_index_list_embeddings()`, `bt_index_embedding_count()`.
   - `src/embed.c`: Loaded persisted SQLite vectors once per query inside `bt_embed_hybrid_search()` (`vault == NULL` safe) and added `vec_is_finite()` / `isfinite()` guards rejecting `NaN`/`Inf` vectors.
   - `src/organizer.c`: Logged `save_dismissed_json()` write failures, backed up corrupt `.bt/dismissed_tags.json` to `.bak` with `g_warning`, and enforced `R9` YAML parse refusal in `bt_organizer_promote_tag()` / `dismiss_tag()`.
   - `src/frontmatter.c`, `src/importer.c`, `src/agents.c`, `src/search.c`: Capped YAML frontmatter slice at 256 KiB, capped importer file reads at 64 MiB (`read_import_file_bounded`), hashed empty-ASCII non-ASCII titles (`untitled-<hash>`), returned `BT_ERROR_INVALID` on non-empty malformed JSONL with 0 parsed objects, added YAML header escaping, and stripped `'%'`, `'\''`, `'!'` alongside POSIX metacharacters.
   - `src/mcp/main.c`: Switched to `vault_open_readonly()`, replaced fixed 64 KB buffer with dynamic 8 MiB `read_json_line()`, added `capabilities`, tool `inputSchema` (including `top_k` and `hybrid` on `search_memory`), newest-first `recent`, `backlinks` tool, and read-only enforcement.
2. **GUI Feature Completion, Navigation Sync, A11y & Editor/Terminal UX:**
   - `src/app.c`, `src/navhistory.c`, `src/ui/askbar.c`, `src/search.c`: Wired `>import chats`, `>review ai tags`, `>sync embeddings`, `Alt+B` Backlinks & Orphans, `Hybrid` toggle + cached vector badge, Up/Down & `Alt+Up/Down` history, multi-CLI Ask-in-agent selector + preflight check, Starter Vault creator (`app.create-starter-vault` menu entry + empty state), in-app Keyboard Shortcuts dialog (`F1` / menu), Back/Forward history synchronization on note rename/delete (`bt_nav_history_rename_path` / `remove_path`), post-rollback sync, accessible labels (`GTK_ACCESSIBLE_PROPERTY_LABEL`) on icon-only header/sidebar/graph buttons, and `[editor] show_line_numbers`, `[search] hybrid_enabled`, `[agent] preferred_cli`.
   - `src/ui/editorpane.c`, `data/styles/bt-ink-*.xml`: Added collapsible in-document Find & Replace (`Ctrl+F`/`Ctrl+H`) with `search-match` style highlights, Starter Vault empty-state buttons, and clean Find-mode toggle behavior.
   - `src/ui/termtabs.c`, `src/ui/filetree.c`: Preserved active terminal focus on page switch/close, added middle-click tab close, and added accessible labels/tooltips.
3. **Brain Graph G3 Interactive Engine:**
   - `src/graphlayout.c`, `src/ui/graphgl.c`, `src/graphview.c`: Fixed `bt_graph_layout_reheat()` iteration reset and immediate position publish on `set_pos()`; added fast GL geometry VBO updates (`bt_graph_gl_mark_positions_dirty`) avoiding Pango atlas rebuilds; implemented 2D drag-to-pin, live spring settling (zero idle redraws once settled), `Re-layout` (`R`), context-menu Pin/Unpin/Unpin All, and `Highlight Orphans` (`O`) across OpenGL and Cairo.
4. **Build, Packaging & Release Documentation:**
   - `.github/workflows/ci.yml`: Enabled `master` CI, MSYS2/Ubuntu deps, `xvfb-run -a`, `-j 1`.
   - `scripts/package-win.ps1`, `meson.build`: Added `bt-mcp.exe` DLL closure, SHA-256 parity check, isolated-PATH `bt-mcp` smoke check, and `licenses/` bundle (`THIRD_PARTY_LICENSES.md` + font `OFL-1.1` files).
   - Added `docs/THIRD_PARTY_LICENSES.md`, `docs/ROADMAP.md`, `docs/KNOWN_LIMITATIONS.md`, `docs/RELEASE_CHECKLIST.md`, and aligned `CHANGELOG.md` and `README.md`.

---

## 5. Verification Results (Latest Final Run: 2026-11-27)

- **Clean Build (`-Dwerror=true`):** `meson compile -C build` passed with **0 warnings / 0 errors**.
- **Correctness Suites (`unit`, `crash-rig`, `vtcorpus` via `meson test -C build --suite unit --suite crash-rig --suite vtcorpus --print-errorlogs -j 1`):** **30 / 30 passed (0 failed, 0 skipped)**.
- **Performance Benchmarks (`meson test -C build --suite bench --print-errorlogs -j 1`):** **7 / 7 passed**:
  - `bench_scan_1k`: `1.8 ms` (`1000` notes, budget `2000 ms`)
  - `bench_vterm`: `0.047 ms/frame` avg (`1000` frames, budget `16.0 ms`)
  - `bench_search_1k`: `0.31 ms` (`1000` notes, `20` hits, budget `100 ms`)
  - `bench_graphlayout`: `1k` (`n=1000, m=1200`) = `898.30 ms` (`300` iters, `rem_overlaps=0`); `3k` (`n=3000, m=4000`) = `5176.01 ms` (`300` iters, `rem_overlaps=0`)
  - `bench_createnote`: `0.0059 ms` avg, `0.0280 ms` max (`3k` notes, `200` iters, budget `5.0 ms`)
  - `bench_graph_1k` (Cairo `1.18.4`): `1k/3k low = 12.97 ms` (`77.1 fps`), `1k/3k high = 13.12 ms` (`76.2 fps`), `budget = 16.6 ms`; `3k/9k low = 10.74 ms` (`93.2 fps`), `high = 10.62 ms` (`94.1 fps`)
  - `bench_graph_gl` (`NVIDIA GeForce RTX 5060 Laptop GPU`, GL `4.6.0`, `bloom=1`): `1k/3k 2D low p95 = 0.118 ms` (`high p95 = 0.098 ms`), `1k/3k 3D high p95 = 0.376 ms`, `3k/9k 3D high p95 = 0.572 ms` (`budget 16.6 / 22.2 ms`)
- **Runtime Startup, Frame Timing & Memory Footprint (`dist\brainterm\bt.exe` on sandboxed `vault-template`):**
  - Cold smoke startup + shutdown (`--smoke-test`): `1,931.4 ms` wall time
  - First-open latency (`--bench-first-open` on `examples/links-demo.md`): `cold_open_call_ms = 5.281`, `cold_first_frame_wall_ms = 33.513`, `warm_open_call_ms = 1.154`, `warm_first_frame_wall_ms = 12.039`, `subsequent_p95_ms = 6.06`
  - Idle interactive GUI (`~3s` settled): Working Set `230.7 MB`, Private Memory `222.4 MB`, `2.21%` CPU delta over `1.5 s` (`531.3 ms` CPU across 16 logical CPUs)
- **CLI Self-Tests & `bt-mcp` E2E:**
  - `build\src\bt.exe --version`: `BrainTerm 0.1.0 (build f0eef3c-dirty · 2026-11-27 17:48:43)`
  - `build\src\bt.exe --selftest-links vault-template`: `notes=7 notes_with_links=1 total_links=11 total_ms=2.82 ms_per_1000_links=256.45` (`RESOLVED=5 AMBIGUOUS=1 UNRESOLVED=1 PLACEHOLDER=1 EXTERNAL=2 BLOCKED=1`)
  - `dist\brainterm\bt-mcp.exe` JSON-RPC 2.0 E2E check: `MCP_EXIT=0`, `readOnly:true`, `search_memory` (`top_k`, `hybrid`) cited 3 matches, path traversal (`../../etc/passwd`) and `write_note` refused (`-32601`), malformed JSON returned `-32700` without crashing.
- **Windows Standalone Packaging (`scripts/package-win.ps1 -KillRunning -BuildDir build -OutDir dist\brainterm -SmokeTest`):**
  - **Passed** with matching `build` & `dist` SHA-256 (`CD9944316AFF1EBEA0B14B08C375893195ED1B0A3CB69087843039A00C674035`)
  - Exe `3,477,076 bytes` (`3.32 MB`) | Full Bundle `89.60 MB`
  - Isolated `PATH` (`System32` only) smoke checks:
    - `BT_SMOKE ok notes=2 lang=bt-markdown term=alive ui=adw-ink scheme=dark tabs=1 checkpoint=ok sessions=1 index=ok ask=search graph=nodes:2`
    - `BT_MCP_SMOKE ok readOnly=true`
